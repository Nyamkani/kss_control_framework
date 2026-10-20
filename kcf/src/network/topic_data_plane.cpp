#include "kcf/network/topic_data_plane.hpp"
#include "kcf/network/remote_topic_client.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include "kcf/network/detail/topic_proxy.hpp"
#include "kcf/dynamic/dynamic_topic_reader.hpp"
#include "kcf/ipc/detail/storage_identity.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <optional>
#include <thread>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
namespace kcf::network {
namespace {
using Clock=std::chrono::steady_clock;
using namespace std::chrono_literals;
bool Topic(const NetworkEndpointMetadata& e,EndpointRole role){return e.kind==EndpointKind::TOPIC&&e.role==role&&e.scope==NetworkScope::REMOTE;}
bool SameRoute(const TopicPacket& a,const TopicPacket& b){return a.source==b.source&&a.destination==b.destination&&a.source_session==b.source_session&&a.destination_session==b.destination_session&&a.route_id==b.route_id;}
TopicRouteInfo Info(const TopicPacket& p,bool sending){TopicRouteInfo s;s.source=p.source;s.destination=p.destination;s.name=p.name;s.source_session=p.source_session;s.destination_session=p.destination_session;s.route_id=p.route_id;s.sending=sending;s.state=TopicRouteState::ACTIVE;return s;}
}
struct RemoteTopicSubscription::State {
    mutable std::mutex mutex;RemoteTopicSnapshot snapshot;
    NetworkEndpointMetadata consumer,source;RemoteEndpointIdentity proxy_identity;TypeDescriptor descriptor;
    std::unique_ptr<detail::TopicProxy> writer;std::uint64_t local_session{0};
};
RemoteTopicSnapshot RemoteTopicSubscription::Snapshot() const {std::lock_guard<std::mutex> lock(state_->mutex);return state_->snapshot;}
struct TopicDataPlane::Impl {
    struct Proxy {TopicProxyInfo info;TypeDescriptor descriptor;std::unique_ptr<detail::TopicProxy> writer;};
    struct Incoming {std::shared_ptr<RemoteTopicSubscription::State> subscription;TopicPacket packet;LocalEndpoint consumer;std::size_t proxy;Clock::time_point interest{};TopicRouteInfo stats;std::optional<TopicPacket> latest;};
    struct Outgoing {TopicPacket packet;LocalEndpoint publisher;std::unique_ptr<DynamicTopicReader> reader;Clock::time_point expires,next{};std::string address;std::uint16_t port{0};TopicRouteInfo stats;};
    TopicConfig config;DiscoveryConfig discovery_config;MetadataConfig metadata;RemoteRuntimeIdentity self;PeerDiscovery* discovery{nullptr};
    int fd{-1},wake{-1};std::atomic<bool> running{false},ready{false};std::atomic<int> error{0};std::thread worker;
    std::uint64_t session{0},next_route{0},next_proxy{0};
    mutable std::mutex local_mutex,stats_mutex,subscription_mutex;
    std::vector<std::shared_ptr<RemoteTopicSubscription::State>> subscriptions;
    std::uint16_t bound_port{0};
    std::shared_ptr<const std::vector<LocalEndpoint>> local=std::make_shared<const std::vector<LocalEndpoint>>();
    TopicDiagnostics published;std::vector<Proxy> proxies;std::vector<Incoming> incoming;std::vector<Outgoing> outgoing;
    std::vector<TopicRouteInfo> history,notices;std::uint64_t malformed{0},stale{0},resource_drops{0},socket_errors{0};
    void Close(){if(fd>=0)close(fd);if(wake>=0)close(wake);fd=wake=-1;}
    const PeerInfo* Peer(const std::vector<PeerInfo>& peers,const HostIdentity& host){for(const auto& p:peers)if(p.host==host)return &p;return nullptr;}
    CompatibilityResult PeerGate(const PeerInfo& p){
        auto result=CheckPeerCompatibility(discovery_config.profile,p.profile,discovery_config.compatibility_policy);
        if(p.state!=PeerState::ONLINE){result.reason=CompatibilityReason::PEER_LOST;return result;}
        auto supports=[](const PeerProfile& profile){return (profile.capabilities&CapabilityBit(Capability::TARGET_ENDPOINT))&&(profile.capabilities&(CapabilityBit(Capability::REMOTE_TOPIC)|CapabilityBit(Capability::REMOTE_TOPIC_RECEIVER)));};
        if(result.Compatible()&&(!supports(p.profile)||!supports(discovery_config.profile)))result.reason=CompatibilityReason::CAPABILITY_MISSING;
        return result;
    }
    bool Gate(const PeerInfo& peer,const NetworkEndpointMetadata& local,const RemoteEndpointIdentity& remote){return PeerGate(peer).Compatible()&&CheckRouteCompatibility(discovery_config,peer,local,remote).Compatible();}
    const LocalEndpoint* Find(const std::vector<LocalEndpoint>& entries,const RemoteEndpointIdentity& id){for(const auto& e:entries)if(e.metadata.identity==id)return &e;return nullptr;}
    void Archive(TopicRouteInfo info){info.state=TopicRouteState::CLOSED;if(history.size()==64)history.erase(history.begin());history.push_back(std::move(info));}
    void Notice(const NetworkEndpointMetadata& source,int code,CompatibilityReason reason=CompatibilityReason::COMPATIBLE,TopicRouteState state=TopicRouteState::REJECTED){if(notices.size()>=64)return;TopicRouteInfo n;n.source=source.identity;n.name=source.name;n.error=code;n.reason=reason;n.state=state;notices.push_back(std::move(n));}
    NetworkEndpointMetadata Consumer(const NetworkEndpointMetadata& source){NetworkEndpointMetadata c;c.identity={self,1};c.name=source.name;c.element_name="network_topic_gateway";c.role=EndpointRole::CONSUMER;c.scope=NetworkScope::REMOTE;c.value_type=source.value_type;return c;}
    const TypeDescriptor* Descriptor(const std::string& name,const TypeIdentity& type,const std::vector<LocalEndpoint>& entries){
        for(const auto& b:config.local_types)if(b.name==name){TypeIdentity t;if(!ParameterWireType(b.descriptor,t)&&t==type)return &b.descriptor;return nullptr;}
        for(const auto& e:entries)if(e.metadata.name==name&&e.metadata.role==EndpointRole::CONSUMER&&e.metadata.value_type==type)return &e.descriptor;
        return nullptr;
    }
    int ProxyFor(const NetworkEndpointMetadata& source,const TypeDescriptor& descriptor,std::size_t& index,ProxyRecoveryState& recovery){
        for(std::size_t i=0;i<proxies.size();++i)if(proxies[i].info.name==source.name){
            if(kcf::detail::LayoutId(proxies[i].descriptor)!=kcf::detail::LayoutId(descriptor))return -EPROTOTYPE;
            proxies[i].info.source=source.identity;index=i;return 0;
        }
        if(proxies.size()>=config.max_proxies)return -ENOSPC;
        detail::ProxyOwnership ownership;ownership.proxy={self,(1ull<<61)|++next_proxy};ownership.source=source.identity;ownership.gateway_session=session;
        for(const auto& peer:discovery->GetPeers())if(peer.host==source.identity.runtime.host)ownership.source_session=peer.session_id;
        auto writer=std::make_unique<detail::TopicProxy>();int code=writer->Create(source.name,descriptor,ownership);recovery=writer->RecoveryState();if(code)return code;
        Proxy p;p.info.recovery=recovery;p.info.stale_detected=writer->StaleDetected();p.info.identity=ownership.proxy;p.info.source=source.identity;p.info.name=source.name;p.info.type=source.value_type;p.descriptor=descriptor;p.writer=std::move(writer);
        index=proxies.size();proxies.push_back(std::move(p));return 0;
    }
    void Refresh(const std::vector<PeerInfo>& peers,const std::vector<LocalEndpoint>& entries){
        notices.clear();std::vector<bool> keep(incoming.size(),false);
        for(const auto& e:entries)if(Topic(e.metadata,EndpointRole::PROVIDER)&&e.metadata.value_type.payload_size>MAX_TOPIC_WIRE_PAYLOAD)Notice(e.metadata,-EMSGSIZE);
        if(!config.receive_only)for(const auto& name:metadata.remote_topics){
            const NetworkEndpointMetadata* source=nullptr;const PeerInfo* owner=nullptr;unsigned matches=0;
            for(const auto& peer:peers)for(const auto& endpoint:peer.endpoints){
                if(endpoint.name!=name||!Topic(endpoint,EndpointRole::PROVIDER)||!peer.metadata_ready)continue;
                auto gate=CheckRouteCompatibility(discovery_config,peer,Consumer(endpoint),endpoint.identity);
                if(!gate.Compatible()){Notice(endpoint,-EACCES,gate.reason);continue;}
                if(endpoint.value_type.payload_size>MAX_TOPIC_WIRE_PAYLOAD){Notice(endpoint,-EMSGSIZE);continue;}
                source=&endpoint;owner=&peer;++matches;
            }
            if(!source)continue;
            if(matches!=1){Notice(*source,-EADDRINUSE);continue;}
            if(std::any_of(entries.begin(),entries.end(),[&](const auto& e){return e.metadata.name==name&&e.metadata.role==EndpointRole::PROVIDER;})){auto count=notices.size();Notice(*source,-EEXIST);if(notices.size()>count)notices.back().proxy_recovery=ProxyRecoveryState::LOCAL_PUBLISHER;continue;}
            const auto* descriptor=Descriptor(name,source->value_type,entries);if(!descriptor){
                const bool declared=std::any_of(config.local_types.begin(),config.local_types.end(),[&](const auto& t){return t.name==name;});
                Notice(*source,declared?-EPROTOTYPE:-ENODATA,declared?CompatibilityReason::TYPE_MISMATCH:CompatibilityReason::COMPATIBLE);continue;
            }
            std::size_t proxy=0;ProxyRecoveryState recovery{};int code=ProxyFor(*source,*descriptor,proxy,recovery);if(code){auto count=notices.size();Notice(*source,code);if(notices.size()>count)notices.back().proxy_recovery=recovery;continue;}
            const LocalEndpoint* consumer=nullptr;
            for(const auto& e:entries)if(e.metadata.name==name&&Topic(e.metadata,EndpointRole::CONSUMER)){
                if(!Gate(*owner,e.metadata,source->identity)){Notice(*source,-EPROTOTYPE,CompatibilityReason::TYPE_MISMATCH);continue;}
                if(kcf::detail::LayoutId(e.descriptor)!=kcf::detail::LayoutId(*descriptor)){Notice(*source,-EPROTOTYPE);continue;}
                if(ValidateLocalTopicEndpoint(e,false))continue;
                if(!consumer||e.metadata.identity.registration_id<consumer->metadata.identity.registration_id)consumer=&e;
            }
            if(!consumer){Notice(*source,0,CompatibilityReason::COMPATIBLE,TopicRouteState::WAITING_SUBSCRIBER);continue;}
            TopicPacket packet;packet.source=source->identity;packet.destination=consumer->metadata.identity;packet.source_session=owner->session_id;packet.destination_session=session;packet.name=name;packet.type=source->value_type;
            bool found=false;
            for(std::size_t i=0;i<incoming.size();++i){auto& r=incoming[i];packet.route_id=r.packet.route_id;if(SameRoute(packet,r.packet)){
                if(i<keep.size())keep[i]=true;
                r.consumer=*consumer;found=true;break;
            }}
            if(found)continue;
            if(incoming.size()+outgoing.size()>=config.max_routes||next_route==UINT64_MAX){Notice(*source,-ENOSPC);++resource_drops;continue;}
            packet.route_id=++next_route;Incoming r;r.packet=packet;r.consumer=*consumer;r.proxy=proxy;r.stats=Info(packet,false);incoming.push_back(std::move(r));keep.push_back(true);
        }
        for(std::size_t i=incoming.size();i--;)if(!keep[i]&&!incoming[i].subscription){Archive(incoming[i].stats);incoming.erase(incoming.begin()+i);}
        for(std::size_t i=outgoing.size();i--;){auto& r=outgoing[i];const auto* peer=Peer(peers,r.packet.destination.runtime.host);const auto* entry=Find(entries,r.packet.source);
            if(!peer||peer->session_id!=r.packet.destination_session||!entry||!Topic(entry->metadata,EndpointRole::PROVIDER)||!Gate(*peer,entry->metadata,r.packet.destination)){Archive(r.stats);outgoing.erase(outgoing.begin()+i);}
            else if(Clock::now()>=r.expires)r.stats.state=TopicRouteState::CLOSED;
        }
    }
    int Send(const TopicPacket& packet,const std::string& ip,std::uint16_t port=0){std::vector<std::uint8_t> bytes;int code=EncodeTopicPacket(packet,bytes);if(code)return code;
        sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(port?port:config.port);if(inet_pton(AF_INET,ip.c_str(),&address.sin_addr)!=1)return -EINVAL;
        auto count=sendto(fd,bytes.data(),bytes.size(),MSG_DONTWAIT|MSG_NOSIGNAL,reinterpret_cast<sockaddr*>(&address),sizeof(address));return count==static_cast<ssize_t>(bytes.size())?0:(count<0?-errno:-EIO);
    }
    void Interest(const TopicPacket& packet,const PeerInfo& peer,const std::vector<LocalEndpoint>& entries,std::uint16_t port){
        if(config.receive_only)return;
        const auto* entry=Find(entries,packet.source);
        if(packet.source_session!=session||packet.destination_session!=peer.session_id||!entry||!Topic(entry->metadata,EndpointRole::PROVIDER)||entry->metadata.name!=packet.name||!(entry->metadata.value_type==packet.type)||!Gate(peer,entry->metadata,packet.destination)||ValidateLocalTopicEndpoint(*entry,true)){++stale;return;}
        for(auto& r:outgoing)if(SameRoute(r.packet,packet)){if(r.port!=port){++stale;return;}r.expires=Clock::now()+750ms;r.stats.state=TopicRouteState::ACTIVE;return;}
        for(std::size_t i=outgoing.size();i--;){const auto& p=outgoing[i].packet;
            if(p.source==packet.source&&p.destination==packet.destination){
                // A receive-only registration has one immutable route and port.
                // Reselect uses a new Consumer identity, never a higher replay ID.
                if((peer.profile.capabilities&CapabilityBit(Capability::REMOTE_TOPIC_RECEIVER))&&p.destination_session==packet.destination_session){++stale;return;}
                if(p.destination_session==packet.destination_session&&p.route_id>=packet.route_id){++stale;return;}
                Archive(outgoing[i].stats);outgoing.erase(outgoing.begin()+i);
            }
        }
        if(incoming.size()+outgoing.size()>=config.max_routes){
            // Keep sequence state for an expired interest while its endpoint/session
            // still exists. Eviction would let a delayed interest reset that route.
            ++resource_drops;return;
        }
        auto reader=std::make_unique<DynamicTopicReader>();int code=reader->Open(entry->metadata.name,entry->descriptor);
        if(code){Notice(entry->metadata,code);return;}
        Outgoing r;r.packet=packet;r.packet.kind=TopicMessage::DATA;r.publisher=*entry;r.reader=std::move(reader);r.address=peer.address;r.port=port;r.expires=Clock::now()+750ms;r.stats=Info(packet,true);outgoing.push_back(std::move(r));
    }
    void Data(const TopicPacket& packet,const PeerInfo& peer){
        for(auto& r:incoming)if(SameRoute(r.packet,packet)){
            if(packet.name!=r.packet.name||!(packet.type==r.packet.type)||!Gate(peer,r.consumer.metadata,packet.source)){++r.stats.malformed;++malformed;return;}
            if(packet.sequence==r.packet.sequence){++r.stats.duplicate;return;}
            if(packet.sequence<r.packet.sequence||packet.sample_sequence<=r.packet.sample_sequence){++r.stats.out_of_order;return;}
            DynamicPayload decoded;if(DecodeParameterValue(r.consumer.descriptor,packet.payload,decoded)){++r.stats.malformed;++malformed;return;}
            r.stats.missed_sequence+=packet.sequence-r.packet.sequence-1;r.packet.sequence=packet.sequence;r.packet.sample_sequence=packet.sample_sequence;++r.stats.packets_received;
            if(r.latest)++r.stats.dropped;
            r.latest=packet;return;
        }
        ++stale;
    }
    void Receive(const std::vector<PeerInfo>& peers,const std::vector<LocalEndpoint>& entries){
        for(unsigned budget=0;budget<64;++budget){std::uint8_t bytes[MAX_TOPIC_DATAGRAM+1];sockaddr_in address{};socklen_t length=sizeof(address);
            auto count=recvfrom(fd,bytes,sizeof(bytes),MSG_DONTWAIT|MSG_TRUNC,reinterpret_cast<sockaddr*>(&address),&length);
            if(count<0){
                if(errno==EBADF||errno==ENOTSOCK)error=-errno;
                else if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)++socket_errors;
                return; // transient UDP/ICMP errors never take down HIGH Control
            }
            TopicPacket packet;if(count>static_cast<ssize_t>(MAX_TOPIC_DATAGRAM)||DecodeTopicPacket(bytes,count,packet)){++malformed;continue;}
            const auto& host=packet.kind==TopicMessage::INTEREST?packet.destination.runtime.host:packet.source.runtime.host;
            const auto* peer=Peer(peers,host);char ip[INET_ADDRSTRLEN]{};inet_ntop(AF_INET,&address.sin_addr,ip,sizeof(ip));
            if(!peer||!peer->metadata_ready||!PeerGate(*peer).Compatible()||peer->address!=ip||(!((peer->profile.capabilities&CapabilityBit(Capability::REMOTE_TOPIC_RECEIVER))&&packet.kind==TopicMessage::INTEREST)&&ntohs(address.sin_port)!=config.port)){++stale;continue;}
            if(packet.kind==TopicMessage::INTEREST)Interest(packet,*peer,entries,ntohs(address.sin_port));else Data(packet,*peer);
        }
    }
    void Deliver(){for(auto& r:incoming)if(r.latest){
        DynamicPayload value;int code=r.subscription?0:ValidateLocalTopicEndpoint(r.consumer,false);
        if(!code)code=DecodeParameterValue(r.consumer.descriptor,r.latest->payload,value);
        if(!code)code=r.subscription?r.subscription->writer->Publish(value):proxies[r.proxy].writer->Publish(value);
        if(code){++r.stats.dropped;r.stats.error=code;}else{++r.stats.samples_published;r.stats.error=0;r.stats.state=TopicRouteState::ACTIVE;}
        if(r.subscription){std::lock_guard<std::mutex> lock(r.subscription->mutex);auto& out=r.subscription->snapshot;
            out.route=r.stats;out.error=code;if(!code){out.state=RemoteTopicState::ACTIVE;out.has_value=true;out.payload=r.latest->payload;out.sequence=r.latest->sequence;out.sample_sequence=r.latest->sample_sequence;}}
        r.latest.reset();
    }}
    void Transmit(const std::vector<PeerInfo>& peers){
        const auto now=Clock::now();
        for(auto& r:incoming)if(now>=r.interest){
            const auto* peer=Peer(peers,r.packet.source.runtime.host);if(peer&&Gate(*peer,r.consumer.metadata,r.packet.source)){
                auto packet=r.packet;packet.sequence=packet.sample_sequence=0;packet.payload.clear();int code=Send(packet,peer->address);if(code)r.stats.error=code;
            }
            r.interest=now+200ms;
        }
        for(auto& r:outgoing){
            if(r.stats.state!=TopicRouteState::ACTIVE||now<r.next)continue;
            double hz=config.max_hz;for(const auto& limit:config.rate_limits)if(limit.name==r.packet.name)hz=limit.max_hz;
            r.next=now+std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0/hz));
            DynamicPayload value;int code=ValidateLocalTopicEndpoint(r.publisher,true);if(!code)code=r.reader->ReadLatest(value);
            if(code){if(code!=-EAGAIN)r.stats.error=code;continue;}
            if(value.sequence==r.packet.sample_sequence)continue;
            if(value.sequence<r.packet.sample_sequence||r.packet.sequence==UINT64_MAX){r.stats.error=-ESTALE;continue;}
            if(r.packet.sample_sequence)r.stats.local_samples_skipped+=value.sequence-r.packet.sample_sequence-1;
            r.packet.sample_sequence=value.sequence;++r.packet.sequence;
            code=EncodeParameterValue(r.publisher.descriptor,value,r.packet.payload);if(!code)code=Send(r.packet,r.address,r.port);
            if(code){++r.stats.dropped;r.stats.error=code;}else{++r.stats.packets_sent;r.stats.error=0;}
        }
    }
    void Export(){TopicDiagnostics snapshot;snapshot.routes=history;snapshot.routes.insert(snapshot.routes.end(),notices.begin(),notices.end());
        for(const auto& r:incoming){snapshot.routes.push_back(r.stats);if(r.latest)++snapshot.pending_samples;if(r.subscription){std::lock_guard<std::mutex> lock(r.subscription->mutex);r.subscription->snapshot.route=r.stats;r.subscription->snapshot.error=r.stats.error;}}
        for(const auto& r:outgoing)snapshot.routes.push_back(r.stats);
        for(const auto& p:proxies)snapshot.proxies.push_back(p.info);
        for(const auto& state:subscriptions)if(state->writer){TopicProxyInfo p;p.identity=state->proxy_identity;p.source=state->source.identity;p.name=state->snapshot.proxy_name;p.type=state->source.value_type;p.recovery=state->writer->RecoveryState();snapshot.proxies.push_back(p);}
        snapshot.socket_errors=socket_errors;snapshot.malformed=malformed;snapshot.stale=stale;snapshot.resource_drops=resource_drops;snapshot.active_send_routes=std::count_if(outgoing.begin(),outgoing.end(),[](const auto& r){return r.stats.state==TopicRouteState::ACTIVE;});snapshot.active_receive_routes=incoming.size();
        std::lock_guard<std::mutex> lock(stats_mutex);published=std::move(snapshot);
    }
    void CloseSubscription(const std::shared_ptr<RemoteTopicSubscription::State>& state,RemoteTopicState status,int code){
        for(std::size_t i=incoming.size();i--;)if(incoming[i].subscription==state){
            {std::lock_guard<std::mutex> lock(state->mutex);state->snapshot.route=incoming[i].stats;state->snapshot.route.state=TopicRouteState::CLOSED;}
            Archive(incoming[i].stats);incoming.erase(incoming.begin()+i);
        }
        if(state->writer){int cleanup=state->writer->Cleanup();if(cleanup&&!code)code=cleanup;state->writer.reset();}
        int removed=0;try{removed=discovery->UnregisterConsumer(state->local_session,state->consumer.identity);}catch(const std::bad_alloc&){removed=-ENOMEM;}if(removed&&removed!=-ENOENT&&removed!=-ESTALE&&!code)code=removed;
        std::lock_guard<std::mutex> lock(state->mutex);state->snapshot.state=status;state->snapshot.error=code;
    }
    void CheckSubscriptions(const std::vector<PeerInfo>& peers){
        for(const auto& state:subscriptions){if(!state->writer)continue;
            const auto* peer=Peer(peers,state->source.identity.runtime.host);int code=0;RemoteTopicState status=RemoteTopicState::STALE;
            if(discovery->GetSessionId()!=state->local_session)code=-ESTALE;
            else if(!peer||peer->state==PeerState::LOST){code=-ENOTCONN;status=RemoteTopicState::LOST;}
            else if(peer->session_id!=state->snapshot.route.source_session||peer->boot_id!=state->source.identity.runtime.boot_id)code=-ESTALE;
            else if(peer->metadata_ready&&!Gate(*peer,state->consumer,state->source.identity)){code=-ESTALE;if(peer->metadata_ready){for(const auto& endpoint:peer->endpoints)if(endpoint.identity==state->source.identity&&!(endpoint.value_type==state->source.value_type)){code=-EPROTOTYPE;status=RemoteTopicState::ERROR;}}}
            if(code)CloseSubscription(state,status,code);
        }
    }
    void Run() noexcept {try{
        auto next=Clock::time_point{};std::vector<PeerInfo> peers;std::shared_ptr<const std::vector<LocalEndpoint>> entries;
        while(running){
            if(!ready){std::this_thread::sleep_for(2ms);continue;}
            if(!session){session=discovery->GetSessionId();if(!session){std::this_thread::sleep_for(2ms);continue;}}
            {std::lock_guard<std::mutex> subscriptions_lock(subscription_mutex);
            if(Clock::now()>=next){peers=discovery->GetPeers();{std::lock_guard<std::mutex> lock(local_mutex);entries=local;}CheckSubscriptions(peers);Refresh(peers,*entries);Export();next=Clock::now()+20ms;}
            Receive(peers,*entries);Deliver();Transmit(peers);if(error)break;}
            pollfd fds[2]{{fd,POLLIN,0},{wake,POLLIN,0}};int code=poll(fds,2,2);if(code<0&&errno!=EINTR){error=-errno;break;}if(fds[1].revents)break;
        }
        std::lock_guard<std::mutex> subscriptions_lock(subscription_mutex);
        for(const auto& state:subscriptions)CloseSubscription(state,error?RemoteTopicState::ERROR:RemoteTopicState::CLOSED,error?error.load():-ECANCELED);subscriptions.clear();
        for(const auto& r:incoming)Archive(r.stats);
        for(const auto& r:outgoing)Archive(r.stats);
        incoming.clear();outgoing.clear();Export();
    }catch(...){error=-EFAULT;try{std::lock_guard<std::mutex> lock(subscription_mutex);for(const auto& state:subscriptions)CloseSubscription(state,RemoteTopicState::ERROR,-EFAULT);subscriptions.clear();incoming.clear();outgoing.clear();Export();}catch(...){} }running=false;}
};
TopicDataPlane::TopicDataPlane():impl_(std::make_unique<Impl>()){}TopicDataPlane::~TopicDataPlane(){Stop();}
int TopicDataPlane::Start(const TopicConfig& config,const DiscoveryConfig& dc,const MetadataConfig& mc,const RemoteRuntimeIdentity& self,PeerDiscovery& discovery){
    auto& s=*impl_;if(s.worker.joinable())return -EBUSY;
    auto rate=[](double n){return std::isfinite(n)&&n>=0.1&&n<=1000;};
    if((!config.receive_only&&config.receive_port&&*config.receive_port!=config.port)||!config.enabled||!config.port||!rate(config.max_hz)||!config.max_routes||config.max_routes>64||!config.max_proxies||config.max_proxies>64||config.local_types.size()>64||config.rate_limits.size()>64||mc.remote_topics.size()>64||config.socket_buffer_bytes<4096||config.socket_buffer_bytes>1024*1024)return -EINVAL;
    for(std::size_t i=0;i<mc.remote_topics.size();++i){
        if(!Text(mc.remote_topics[i],240)||mc.remote_topics[i][0]!='/')return -EINVAL;
        for(std::size_t j=0;j<i;++j)if(mc.remote_topics[i]==mc.remote_topics[j])return -EINVAL;
    }
    for(const auto& r:config.rate_limits)if(!Text(r.name,240)||r.name[0]!='/'||!rate(r.max_hz))return -EINVAL;
    for(const auto& t:config.local_types){TypeIdentity wire;if(!Text(t.name,240)||t.name[0]!='/'||ParameterWireType(t.descriptor,wire))return -EINVAL;}
    for(std::size_t i=0;i<config.local_types.size();++i)for(std::size_t j=0;j<i;++j)if(config.local_types[i].name==config.local_types[j].name)return -EINVAL;
    if(!s.proxies.empty()&&(!(s.self==self)||s.proxies.size()>config.max_proxies))return -EINVAL;
    s.ready=false;
    {std::lock_guard<std::mutex> lock(s.local_mutex);s.local=std::make_shared<const std::vector<LocalEndpoint>>();}
    s.config=config;s.discovery_config=dc;s.metadata=mc;s.self=self;s.discovery=&discovery;s.session=config.receive_only?discovery.GetSessionId():0;s.error=0;s.malformed=s.stale=s.resource_drops=s.socket_errors=0;s.history.clear();s.notices.clear();
    s.fd=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(s.fd<0)return -errno;
    auto fail=[&](int code){s.Close();return code;};int buffer=config.socket_buffer_bytes,df=IP_PMTUDISC_DO;
    if(setsockopt(s.fd,SOL_SOCKET,SO_RCVBUF,&buffer,sizeof(buffer))||setsockopt(s.fd,SOL_SOCKET,SO_SNDBUF,&buffer,sizeof(buffer))||setsockopt(s.fd,IPPROTO_IP,IP_MTU_DISCOVER,&df,sizeof(df)))return fail(-errno);
    sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(config.receive_port.value_or(config.port));if(inet_pton(AF_INET,dc.interface_address.c_str(),&a.sin_addr)!=1)return fail(-EINVAL);
    if(bind(s.fd,reinterpret_cast<sockaddr*>(&a),sizeof(a)))return fail(-errno);
    socklen_t address_size=sizeof(a);if(getsockname(s.fd,reinterpret_cast<sockaddr*>(&a),&address_size))return fail(-errno);s.bound_port=ntohs(a.sin_port);
    s.wake=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);if(s.wake<0)return fail(-errno);
    s.running=true;try{s.worker=std::thread([&s]{s.Run();});}catch(...){s.running=false;return fail(-EAGAIN);}return 0;
}
void TopicDataPlane::UpdateLocal(const std::vector<LocalEndpoint>& all){auto entries=std::make_shared<std::vector<LocalEndpoint>>();for(const auto& e:all)if(e.metadata.kind==EndpointKind::TOPIC){if(entries->size()==64)break;entries->push_back(e);}std::lock_guard<std::mutex> lock(impl_->local_mutex);impl_->local=std::move(entries);impl_->ready=true;}
void TopicDataPlane::Stop(){auto& s=*impl_;s.running=false;if(s.wake>=0){std::uint64_t n=1;auto ignored=write(s.wake,&n,sizeof(n));(void)ignored;}if(s.worker.joinable())s.worker.join();{std::lock_guard<std::mutex> lock(s.subscription_mutex);for(const auto& state:s.subscriptions)s.CloseSubscription(state,RemoteTopicState::CLOSED,-ECANCELED);s.subscriptions.clear();}s.Close();}
TopicDiagnostics TopicDataPlane::GetDiagnostics() const {std::lock_guard<std::mutex> lock(impl_->stats_mutex);return impl_->published;}
int TopicDataPlane::GetLastError() const{return impl_->error.load();}
int TopicDataPlane::Subscribe(const NetworkEndpointMetadata& source,std::uint64_t expected,const TypeDescriptor& descriptor,std::shared_ptr<RemoteTopicSubscription>& output){
    auto& s=*impl_;std::lock_guard<std::mutex> guard(s.subscription_mutex);
    if(!s.running||!s.config.receive_only)return -ENOTCONN;
    if(s.subscriptions.size()>=s.config.max_routes)return -ENOSPC;
    TypeIdentity type;int code=ParameterWireType(descriptor,type);if(code)return code;
    if(!(type==source.value_type)||type.payload_size>MAX_TOPIC_WIRE_PAYLOAD)return -EPROTOTYPE;
    auto peers=s.discovery->GetPeers();const auto* peer=s.Peer(peers,source.identity.runtime.host);
    if(!peer||peer->state!=PeerState::ONLINE)return -ENOTCONN;
    if(peer->session_id!=expected||peer->boot_id!=source.identity.runtime.boot_id)return -ESTALE;
    if(!(peer->profile.capabilities&CapabilityBit(Capability::TOPIC_ENDPOINT_SUBSCRIPTION)))return -ENOTSUP;
    static std::atomic<std::uint64_t> ids{0};auto id=++ids;if(id>=(1ull<<60))return -EOVERFLOW;
    auto state=std::make_shared<RemoteTopicSubscription::State>();state->source=source;state->descriptor=descriptor;
    state->local_session=s.discovery->GetSessionId();if(state->local_session!=s.session)return -ESTALE;
    state->consumer=s.Consumer(source);state->consumer.identity={s.self,(1ull<<60)|id};
    if(!s.Gate(*peer,state->consumer,source.identity))return -EPROTOTYPE;
    state->snapshot.descriptor=descriptor;
    state->snapshot.proxy_name="/kcf_remote_topic_"+std::to_string(getpid())+"_"+std::to_string(s.self.process_start_ticks)+"_"+std::to_string(s.session)+"_"+std::to_string(id);
    detail::ProxyOwnership ownership;ownership.proxy={s.self,(1ull<<61)|id};state->proxy_identity=ownership.proxy;ownership.source=source.identity;ownership.gateway_session=s.session;ownership.source_session=expected;
    state->writer=std::make_unique<detail::TopicProxy>();code=state->writer->Create(state->snapshot.proxy_name,descriptor,ownership);if(code)return code;
    auto handle=std::shared_ptr<RemoteTopicSubscription>(new RemoteTopicSubscription(state));
    Impl::Incoming route;route.subscription=state;route.consumer.metadata=state->consumer;route.consumer.descriptor=descriptor;
    route.packet.source=source.identity;route.packet.destination=state->consumer.identity;route.packet.source_session=expected;route.packet.destination_session=s.session;
    route.packet.route_id=id;route.packet.name=source.name;route.packet.type=type;route.stats=Info(route.packet,false);route.stats.state=TopicRouteState::WAITING_SUBSCRIBER;
    state->snapshot.route=route.stats;
    s.subscriptions.reserve(s.subscriptions.size()+1);s.incoming.reserve(s.incoming.size()+1);
    code=s.discovery->RegisterConsumer(s.session,state->consumer);if(code)return code;
    s.subscriptions.push_back(state);s.incoming.push_back(std::move(route));output=std::move(handle);return 0;
}
int TopicDataPlane::Unsubscribe(const std::shared_ptr<RemoteTopicSubscription>& handle){
    if(!handle)return -EINVAL;auto& s=*impl_;std::lock_guard<std::mutex> lock(s.subscription_mutex);
    auto i=std::find(s.subscriptions.begin(),s.subscriptions.end(),handle->state_);if(i==s.subscriptions.end())return -ENOENT;
    s.CloseSubscription(*i,RemoteTopicState::CLOSED,0);s.subscriptions.erase(i);s.Export();return handle->Snapshot().error;
}
std::uint16_t TopicDataPlane::ReceivePort() const{return impl_->bound_port;}
RemoteTopicClient::RemoteTopicClient(PeerDiscovery& discovery,const DiscoveryConfig& config,RemoteControlClient& control):discovery_(discovery),discovery_config_(config),control_(control){}
RemoteTopicClient::~RemoteTopicClient(){Stop();}
int RemoteTopicClient::Start(const RemoteTopicConfig& config){
    std::lock_guard<std::mutex> lock(mutex_);if(running_)return -EBUSY;
    if(!IsObserverProfile(discovery_config_.profile)||!(discovery_config_.profile.capabilities&CapabilityBit(Capability::REMOTE_TOPIC_RECEIVER))||!discovery_.GetSessionId()||!config.max_subscriptions||config.max_subscriptions>64)return -EINVAL;
    RemoteRuntimeIdentity self{discovery_config_.host,discovery_config_.boot_id,getpid(),0};if(!kcf::detail::ReadProcessIdentity(getpid(),self.process_start_ticks))return -ESRCH;
    TopicConfig tc;tc.enabled=true;tc.receive_only=true;tc.receive_port=config.receive_port;tc.port=config.publisher_port;tc.max_routes=tc.max_proxies=config.max_subscriptions;tc.socket_buffer_bytes=config.socket_buffer_bytes;
    MetadataConfig metadata;metadata.host=self.host;metadata.boot_id=self.boot_id;
    int code=plane_.Start(tc,discovery_config_,metadata,self,discovery_);if(code)return code;
    plane_.UpdateLocal({});++generation_;running_=true;return 0;
}
int RemoteTopicClient::Subscribe(const RemoteEndpointIdentity& endpoint,std::uint64_t expected,const std::string& name,const TypeIdentity& type,std::shared_ptr<RemoteTopicSubscription>& output,std::chrono::milliseconds timeout){
    std::uint64_t generation;{std::lock_guard<std::mutex> lock(mutex_);if(!running_)return -ENOTCONN;generation=generation_;}
    // Descriptor request has its own finite deadline and independent Control IO.
    TypeDescriptor descriptor;int code=control_.QueryTypeDescriptor(endpoint,expected,name,DescriptorRole::VALUE,type,descriptor,timeout);if(code)return code;
    std::lock_guard<std::mutex> lock(mutex_);if(!running_||generation!=generation_)return -ECANCELED;
    try {for(const auto& peer:discovery_.GetPeers())if(peer.host==endpoint.runtime.host){
        if(peer.session_id!=expected||peer.boot_id!=endpoint.runtime.boot_id)return -ESTALE;
        for(const auto& e:peer.endpoints)if(e.identity==endpoint){if(!Topic(e,EndpointRole::PROVIDER)||e.name!=name||!(e.value_type==type))return -EPROTOTYPE;return plane_.Subscribe(e,expected,descriptor,output);}
        return -ENOENT;
    }return -ENOENT;}catch(const std::bad_alloc&){return -ENOMEM;}
}
int RemoteTopicClient::Unsubscribe(const std::shared_ptr<RemoteTopicSubscription>& handle){std::lock_guard<std::mutex> lock(mutex_);return plane_.Unsubscribe(handle);}
void RemoteTopicClient::Stop(){std::lock_guard<std::mutex> lock(mutex_);running_=false;++generation_;plane_.Stop();}
TopicDiagnostics RemoteTopicClient::GetDiagnostics() const{return plane_.GetDiagnostics();}
std::uint16_t RemoteTopicClient::ReceivePort() const{std::lock_guard<std::mutex> lock(mutex_);return plane_.ReceivePort();}

}
