#include "kcf/network/control_node.hpp"
#include "kcf/dynamic/dynamic_parameter_client.hpp"
#include "kcf/dynamic/dynamic_service_client.hpp"
#include "kcf/dynamic/dynamic_action_client.hpp"
#include <set>
#include "kcf/introspection/detail/runtime_registry.hpp"
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <limits>
#include <cerrno>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
namespace kcf::network {
namespace {
using Clock=std::chrono::steady_clock;
constexpr std::size_t MAX_PENDING=256,MAX_CONNECTIONS=64,MAX_QUEUE_BYTES=256*1024;
CapabilitySet Supported() {return CapabilityBit(Capability::DISCOVERY)|CapabilityBit(Capability::REMOTE_PARAMETER)|CapabilityBit(Capability::REMOTE_SERVICE)|CapabilityBit(Capability::REMOTE_ACTION)|
    CapabilityBit(Capability::TARGET_ENDPOINT)|CapabilityBit(Capability::TARGET_HOST)|CapabilityBit(Capability::TARGET_APPLICATION)|
    CapabilityBit(Capability::TARGET_GROUP)|CapabilityBit(Capability::TARGET_ALL);}
bool Lower(const HostIdentity& a,const HostIdentity& b){return a.bytes<b.bytes;}
bool Select(const RemoteTarget& t,const NetworkEndpointMetadata& e) {
    switch(t.scope) {
    case TargetScope::ENDPOINT:return t.endpoint==e.identity;
    case TargetScope::HOST:return t.host==e.identity.runtime.host;
    case TargetScope::APPLICATION:return t.application==e.application;
    case TargetScope::GROUP:return std::find(e.groups.begin(),e.groups.end(),t.group)!=e.groups.end();
    case TargetScope::ALL:return true;
    }return false;
}
NetworkEndpointMetadata Consumer(const RemoteRuntimeIdentity& source,const std::string& name,const TypeIdentity& type,const TypeIdentity& response={},const TypeIdentity& feedback={}) {
    NetworkEndpointMetadata m;m.identity={source,1};m.element_name="network_control";m.name=name;
    m.kind=EndpointKind::PARAMETER;m.role=EndpointRole::CONSUMER;m.scope=NetworkScope::REMOTE;m.value_type=type;
    if(!(response==TypeIdentity{})){m.kind=EndpointKind::SERVICE;m.value_type={};m.request_type=type;m.response_type=response;}
    if(!(feedback==TypeIdentity{})){m.kind=EndpointKind::ACTION;m.feedback_type=feedback;}
    return m;
}
bool Request(ControlMessage op){return op==ControlMessage::PARAM_GET_REQUEST||op==ControlMessage::PARAM_SET_REQUEST||op==ControlMessage::SERVICE_REQUEST||op==ControlMessage::ACTION_GOAL_REQUEST||op==ControlMessage::ACTION_CANCEL_REQUEST;}
ControlMessage ResponseOperation(ControlMessage op){return op==ControlMessage::ACTION_GOAL_REQUEST?ControlMessage::ACTION_GOAL_RESPONSE:op==ControlMessage::ACTION_CANCEL_REQUEST?ControlMessage::ACTION_CANCEL_RESPONSE:op==ControlMessage::SERVICE_REQUEST?ControlMessage::SERVICE_RESPONSE:(op==ControlMessage::PARAM_GET_REQUEST?ControlMessage::PARAM_GET_RESPONSE:ControlMessage::PARAM_SET_RESPONSE);}
int ReasonError(CompatibilityReason r){return r==CompatibilityReason::TYPE_MISMATCH?-EPROTOTYPE:r==CompatibilityReason::PEER_LOST?-ENOTCONN:-EACCES;}
}
struct ControlNode::Impl {
    struct Batch {
        RemoteTarget target;std::string name;TypeIdentity type,response_type,feedback_type;std::shared_ptr<RemoteActionSession> cancel;std::vector<std::uint8_t> value;bool set{false},service{false},action{false};
        Clock::time_point deadline;std::promise<ParameterBatchResult> promise;ParameterBatchResult result;std::size_t left{0};
    };
    struct Pending {std::shared_ptr<Batch> batch;std::size_t index;ControlFrame frame;bool sent{false};std::uint64_t connection{0};};
    struct Connection {
        int fd{-1};std::uint64_t id{0};bool connecting{false},ready{false},outbound{false},hello_sent{false};
        HostIdentity host;std::uint64_t session{0};std::string address;Clock::time_point deadline;
        std::vector<std::uint8_t> input;std::deque<std::vector<std::uint8_t>> output;std::size_t offset{0},queued{0};
    };
    struct Job {std::uint64_t connection;ControlFrame frame;Clock::time_point deadline;};
    struct Reply {std::uint64_t connection;ControlFrame frame;};
    struct Seen {HostIdentity host;std::uint64_t session{0},last{0},last_goal{0};};
    struct ServiceTask {std::thread worker;std::atomic<bool> done{true};};
    std::array<ServiceTask,32> service_tasks; // fail admission, never wait behind another Service
    struct Outgoing {std::shared_ptr<RemoteActionSession> session;std::uint64_t connection;};
    struct Incoming {
        Job job;std::mutex call_mutex;DynamicActionClient client;std::uint64_t local_goal{0};
        std::atomic<bool> done{false},accepted{false};
    };
    std::array<ServiceTask,32> action_tasks;
    std::vector<Outgoing> outgoing; // IO thread only
    std::vector<std::shared_ptr<Incoming>> incoming; // protected by mutex
    std::set<std::uint64_t> live_connections,abort_connections; // protected by mutex
    std::uint64_t next_goal{0}; // independent of request correlation; never reset on Start
    struct Storage {RemoteEndpointIdentity endpoint;std::unique_ptr<DynamicParameterClient> client;};
    mutable std::mutex mutex;
    ControlConfig config;RemoteRuntimeIdentity self;PeerDiscovery discovery;TopicDataPlane topics;
    std::atomic<bool> running{false};std::atomic<int> error{0};
    std::thread io,backend;int listener{-1},wake{-1};std::uint64_t session{0},next_request{0},next_connection{0};
    std::deque<std::shared_ptr<Batch>> submissions;std::deque<Job> jobs;std::deque<Reply> replies;
    std::vector<LocalEndpoint> local;std::vector<Storage> storage;
    std::vector<Pending> pending;std::vector<Connection> connections;std::vector<Seen> seen;
    std::atomic<std::uint64_t> connected{0},sent{0},ignored{0},rejected{0};
    void Wake(){if(wake>=0){std::uint64_t v=1;const auto n=write(wake,&v,sizeof(v));(void)n;}}
    void Close(){if(listener>=0)close(listener);if(wake>=0)close(wake);listener=wake=-1;}
    std::vector<LocalEndpoint> Local(){std::lock_guard<std::mutex> lock(mutex);return local;}
    const PeerInfo* Find(const std::vector<PeerInfo>& peers,const HostIdentity& host) {
        auto i=std::find_if(peers.begin(),peers.end(),[&](const auto& p){return p.host==host;});return i==peers.end()?nullptr:&*i;
    }
    bool Allowed(const PeerInfo& p) {
        auto result=CheckCapability(config.discovery.profile,p.profile,CapabilityBit(Capability::TARGET_ENDPOINT),config.discovery.compatibility_policy);
        return p.state==PeerState::ONLINE && result.Compatible() &&
            (result.common_capabilities & (CapabilityBit(Capability::REMOTE_PARAMETER)|CapabilityBit(Capability::REMOTE_SERVICE)|CapabilityBit(Capability::REMOTE_ACTION)));
    }
    void Finish(std::size_t index,int code,CompatibilityReason reason=CompatibilityReason::COMPATIBLE,std::vector<std::uint8_t> value={},bool confirmed=false,bool unknown=false) {
        auto p=std::move(pending[index]);pending.erase(pending.begin()+index);
        auto& out=p.batch->result.endpoints[p.index];out.error=code;out.reason=reason;out.execution_unknown=p.sent&&code&&(p.batch->service||(p.batch->action&&!p.batch->cancel&&(!confirmed||unknown)));
        out.cancel_unknown=bool(p.batch->cancel)&&p.sent&&code&&(!confirmed||unknown);
        if(out.action){std::lock_guard<std::mutex> lock(out.action->mutex_);auto& a=out.action->snapshot_;
            if(p.batch->cancel){a.cancel_unknown=out.cancel_unknown;a.cancel_accepted=!code;}
            else {a.error=code;a.execution_unknown=out.execution_unknown;a.accepted=!code;a.state=code?ActionState::IDLE:ActionState::ACCEPTED;
                if(code){a.communication_lost=out.execution_unknown;outgoing.erase(std::remove_if(outgoing.begin(),outgoing.end(),[&](const auto& o){return o.session==out.action;}),outgoing.end());
                    if(out.execution_unknown){std::lock_guard<std::mutex> global(mutex);if(live_connections.count(p.connection))abort_connections.insert(p.connection);}
                }
            }
        }
        out.value=std::move(value);
        if(!--p.batch->left)p.batch->promise.set_value(std::move(p.batch->result));
    }
    void Drop(std::size_t index) {
        auto& c=connections[index];close(c.fd);
        {std::lock_guard<std::mutex> lock(mutex);live_connections.erase(c.id);abort_connections.erase(c.id);}
        for(auto i=outgoing.begin();i!=outgoing.end();)if(i->connection==c.id){
            std::lock_guard<std::mutex> lock(i->session->mutex_);auto& a=i->session->snapshot_;
            a.communication_lost=true;a.error=-ECONNRESET;i=outgoing.erase(i);
        }else ++i;
        for(std::size_t i=pending.size();i--;)if(pending[i].sent&&pending[i].connection==c.id)Finish(i,-ECONNRESET);
        connections.erase(connections.begin()+index);
    }
    bool Queue(Connection& c,const ControlFrame& f) {
        std::vector<std::uint8_t> bytes;if(EncodeControlFrame(f,bytes)||c.queued+bytes.size()>MAX_QUEUE_BYTES)return false;
        c.queued+=bytes.size();c.output.push_back(std::move(bytes));return true;
    }
    bool Hello(Connection& c) {
        ControlFrame f;f.source=self;f.session_id=session;c.hello_sent=Queue(c,f);return c.hello_sent;
    }
    void Admit(const std::vector<PeerInfo>& peers) {
        std::deque<std::shared_ptr<Batch>> queue;
        {std::lock_guard<std::mutex> lock(mutex);queue.swap(submissions);}
        for(auto& b:queue) {
            std::vector<std::pair<const PeerInfo*,NetworkEndpointMetadata>> selected;
            for(const auto& peer:peers)for(const auto& e:peer.endpoints)
                if(e.kind==(b->action?EndpointKind::ACTION:(b->service?EndpointKind::SERVICE:EndpointKind::PARAMETER)) && e.role==EndpointRole::PROVIDER && e.name==b->name && Select(b->target,e))selected.emplace_back(&peer,e);
            if(selected.empty()) {
                b->result.error=-ENOENT;
                if(b->target.scope==TargetScope::ENDPOINT||b->target.scope==TargetScope::HOST) {
                    const auto host=b->target.scope==TargetScope::HOST?b->target.host:b->target.endpoint.runtime.host;
                    const auto* peer=Find(peers,host);
                    if(!peer||peer->state==PeerState::LOST)b->result.error=-ENOTCONN;
                    else if(!peer->metadata_ready)b->result.error=-EAGAIN;
                }
                b->promise.set_value(std::move(b->result));continue;
            }
            if(selected.size()+pending.size()>MAX_PENDING || (b->action&&!b->cancel&&selected.size()+pending.size()+outgoing.size()>MAX_PENDING) || (b->action&&!b->cancel&&next_goal>UINT64_MAX-selected.size()) || next_request>UINT64_MAX-selected.size()) {
                b->result.error=(next_request>UINT64_MAX-selected.size()||(b->action&&!b->cancel&&next_goal>UINT64_MAX-selected.size()))?-EOVERFLOW:-EAGAIN;b->promise.set_value(std::move(b->result));continue;
            }
            b->left=selected.size();b->result.endpoints.resize(selected.size());
            for(std::size_t i=0;i<selected.size();++i) {
                const auto& endpoint=selected[i].second;b->result.endpoints[i].endpoint=endpoint.identity;
                ControlFrame f;f.source=self;f.session_id=session;f.request_id=++next_request;
                f.operation=b->service?ControlMessage::SERVICE_REQUEST:(b->set?ControlMessage::PARAM_SET_REQUEST:ControlMessage::PARAM_GET_REQUEST);
                f.response_type=b->response_type;f.feedback_type=b->feedback_type;
                if(b->action){
                    f.operation=b->cancel?ControlMessage::ACTION_CANCEL_REQUEST:ControlMessage::ACTION_GOAL_REQUEST;
                    f.goal=b->cancel?b->cancel->request_.goal:RemoteGoalIdentity{self,++next_goal,session};
                    auto action=b->cancel?b->cancel:std::make_shared<RemoteActionSession>();
                    b->result.endpoints[i].action=action;
                    if(!b->cancel){action->snapshot_.goal=f.goal;action->snapshot_.endpoint=endpoint.identity;}
                }
                f.endpoint=endpoint.identity;f.target=b->target.scope;f.name=b->name;f.type=b->type;f.payload=b->value;
                if(b->action&&!b->cancel)b->result.endpoints[i].action->request_=f;
                pending.push_back({b,i,std::move(f),false,0});
            }
        }
    }
    bool Receive(Connection& c,const ControlFrame& f,const std::vector<PeerInfo>& peers) {
        const auto* peer=Find(peers,f.source.host);
        if(!peer||!Allowed(*peer)||peer->boot_id!=f.source.boot_id||peer->session_id!=f.session_id||peer->address!=c.address)return false;
        if(f.operation==ControlMessage::HELLO) {
            if(c.ready)return false;
            if(c.outbound) {if(c.host!=f.source.host)return false;}
            else {
                if(!Lower(f.source.host,self.host))return false;
                for(const auto& other:connections)if(other.id!=c.id&&other.host==f.source.host)return false;
                c.host=f.source.host;c.session=f.session_id;if(!Hello(c))return false;
            }
            c.ready=true;{std::lock_guard<std::mutex> lock(mutex);live_connections.insert(c.id);}
            ++connected;return true;
        }
        if(!c.ready||c.host!=f.source.host||c.session!=f.session_id)return false;
        if(f.operation==ControlMessage::ACTION_FEEDBACK||f.operation==ControlMessage::ACTION_RESULT){
            auto found=std::find_if(outgoing.begin(),outgoing.end(),[&](const auto& o){return o.connection==c.id&&o.session->request_.goal==f.goal;});
            if(found==outgoing.end()){++ignored;return true;}
            const auto& original=found->session->request_;
            if(f.endpoint!=original.endpoint||f.name!=original.name||!(f.type==original.type)||!(f.feedback_type==original.feedback_type)||!(f.response_type==original.response_type)||f.target!=original.target)return false;
            auto action=found->session;std::lock_guard<std::mutex> lock(action->mutex_);auto& a=action->snapshot_;
            if(!a.accepted)return false;
            if(f.operation==ControlMessage::ACTION_FEEDBACK){if(f.feedback_sequence<=a.feedback_sequence){++ignored;return true;}
                a.feedback_sequence=f.feedback_sequence;a.feedback=f.payload;a.state=ActionState::RUNNING;
            }else{a.error=f.status;
                if(f.status)a.communication_lost=true;
                else{a.terminal=true;a.state=f.action_state;a.result=f.payload;}
                outgoing.erase(found);
            }
            return true;
        }
        if(Request(f.operation)) {
            if(f.endpoint.runtime.host!=self.host||f.endpoint.runtime.boot_id!=self.boot_id)return false;
            auto known=std::find_if(seen.begin(),seen.end(),[&](const auto& s){return s.host==c.host;});
            if(known==seen.end()){if(seen.size()>=MAX_CONNECTIONS)return false;seen.push_back({c.host,c.session,0});known=seen.end()-1;}
            if(known->session!=c.session){known->session=c.session;known->last=0;known->last_goal=0;}
            if(f.request_id<=known->last){++ignored;return true;} // no replay, including reconnect in same session
            known->last=f.request_id;
            if(f.operation==ControlMessage::ACTION_GOAL_REQUEST){if(f.goal.goal_id<=known->last_goal){++ignored;return true;}known->last_goal=f.goal.goal_id;}
            std::lock_guard<std::mutex> lock(mutex);
            if(jobs.size()>=64)return false;
            jobs.push_back({c.id,f,Clock::now()+std::chrono::milliseconds(f.timeout_ms)});return true;
        }
        for(std::size_t i=0;i<pending.size();++i) {
            const auto& p=pending[i];if(!p.sent||p.connection!=c.id||p.frame.request_id!=f.request_id)continue;
            if(f.endpoint!=p.frame.endpoint||f.name!=p.frame.name||!(f.type==p.frame.type)||f.target!=p.frame.target||
               !(f.response_type==p.frame.response_type)||!(f.feedback_type==p.frame.feedback_type)||f.goal!=p.frame.goal||f.operation!=ResponseOperation(p.frame.operation))return false;
            if(Clock::now()>=p.batch->deadline){Finish(i,-ETIMEDOUT);++ignored;return true;}
            Finish(i,f.status,f.reason,f.payload,true,f.outcome_unknown);return true;
        }
        ++ignored;return true; // expired/duplicate response never satisfies another request
    }
    bool Read(Connection& c,const std::vector<PeerInfo>& peers) {
        std::uint8_t bytes[4096];
        for(unsigned attempt=0;attempt<16;++attempt) {
            auto n=recv(c.fd,bytes,sizeof(bytes),MSG_DONTWAIT);
            if(n==0)return false;
            if(n<0)return errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR;
            c.input.insert(c.input.end(),bytes,bytes+n);
            for(;;) {
                if(c.input.size()<CONTROL_PREFIX_SIZE)break;
                std::size_t size=0;if(ControlFrameSize(c.input.data(),c.input.size(),size))return false;
                if(c.input.size()<size)break;
                ControlFrame frame;if(DecodeControlFrame(c.input.data(),size,frame)||!Receive(c,frame,peers))return false;
                c.input.erase(c.input.begin(),c.input.begin()+size);
            }
            if(c.input.size()>MAX_CONTROL_FRAME)return false;
        }return true;
    }
    bool Write(Connection& c) {
        for(unsigned i=0;i<16&&!c.output.empty();++i) {
            auto& bytes=c.output.front();auto n=send(c.fd,bytes.data()+c.offset,bytes.size()-c.offset,MSG_NOSIGNAL|MSG_DONTWAIT);
            if(n<0)return errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR;
            if(!n)return false;
            c.offset+=n;c.queued-=n;if(c.offset==bytes.size()){c.output.pop_front();c.offset=0;}
        }return true;
    }
    void Connect(const std::vector<PeerInfo>& peers) {
        for(const auto& p:peers) {
            if(connections.size()>=MAX_CONNECTIONS||!Allowed(p)||!Lower(self.host,p.host))continue;
            if(std::any_of(connections.begin(),connections.end(),[&](const auto& c){return c.host==p.host;}))continue;
            int fd=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(fd<0){error=-errno;continue;}
            sockaddr_in local_address{};local_address.sin_family=AF_INET;inet_pton(AF_INET,config.discovery.interface_address.c_str(),&local_address.sin_addr);
            if(bind(fd,reinterpret_cast<sockaddr*>(&local_address),sizeof(local_address))){close(fd);continue;}
            sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(p.port);inet_pton(AF_INET,p.address.c_str(),&address.sin_addr);
            int code=connect(fd,reinterpret_cast<sockaddr*>(&address),sizeof(address));
            if(code&&errno!=EINPROGRESS){close(fd);continue;}
            Connection c;c.fd=fd;c.id=++next_connection;c.outbound=true;c.connecting=code!=0;c.host=p.host;c.session=p.session_id;c.address=p.address;
            c.deadline=Clock::now()+config.connect_timeout;
            connections.push_back(std::move(c));if(!code&&!Hello(connections.back()))Drop(connections.size()-1);
        }
    }
    void Accept() {
        for(unsigned i=0;i<16;++i) {
            sockaddr_in a{};socklen_t size=sizeof(a);int fd=accept4(listener,reinterpret_cast<sockaddr*>(&a),&size,SOCK_NONBLOCK|SOCK_CLOEXEC);
            if(fd<0)return;
            if(connections.size()>=MAX_CONNECTIONS){close(fd);continue;}
            char ip[INET_ADDRSTRLEN]{};inet_ntop(AF_INET,&a.sin_addr,ip,sizeof(ip));
            Connection c;c.fd=fd;c.id=++next_connection;c.address=ip;c.deadline=Clock::now()+config.connect_timeout;
            connections.push_back(std::move(c));
        }
    }
    void IO() noexcept {
        try {
            auto next_connect=Clock::time_point{};
            while(running) {
                if(config.topics.enabled&&topics.GetLastError()){error=topics.GetLastError();break;}
                auto peers=discovery.GetPeers();Admit(peers);auto now=Clock::now();
                for(std::size_t i=connections.size();i--;) {
                    auto& c=connections[i];const auto* peer=Find(peers,c.host);
                    bool abort=false;{std::lock_guard<std::mutex> lock(mutex);abort=abort_connections.count(c.id);}
                    if(abort||(!c.ready&&now>=c.deadline)||(Valid(c.host)&&(!peer||!Allowed(*peer)||peer->session_id!=c.session)))Drop(i);
                }
                if(now>=next_connect){Connect(peers);next_connect=now+config.reconnect_interval;}
                for(std::size_t i=pending.size();i--;) {
                    auto& p=pending[i];if(now>=p.batch->deadline){Finish(i,-ETIMEDOUT);continue;}
                    const auto* peer=Find(peers,p.frame.endpoint.runtime.host);
                    if(!peer){Finish(i,-ENOTCONN,CompatibilityReason::PEER_LOST);continue;}
                    auto gate=CheckRouteCompatibility(config.discovery,*peer,Consumer(self,p.frame.name,p.frame.type,p.frame.response_type,p.frame.feedback_type),p.frame.endpoint,p.frame.target);
                    if(!gate.Compatible()){Finish(i,ReasonError(gate.reason),gate.reason);continue;}
                }
                // Preserve ascending request IDs on each TCP stream. Responses
                // may complete independently; expiry above erases in reverse.
                for(std::size_t i=0;i<pending.size();) {
                    auto& p=pending[i];bool erased=false;
                    if(!p.sent)for(auto& c:connections)if(c.host==p.frame.endpoint.runtime.host&&c.ready) {
                        p.frame.timeout_ms=std::max<std::int64_t>(1,std::chrono::duration_cast<std::chrono::milliseconds>(p.batch->deadline-now).count());
                        if(!Queue(c,p.frame)){Finish(i,-EAGAIN);erased=true;break;}
                        p.sent=true;p.connection=c.id;
                        if(p.batch->action&&!p.batch->cancel)outgoing.push_back({p.batch->result.endpoints[p.index].action,c.id});
                        ++sent;break;
                    }
                    if(!erased)++i;
                }
                std::deque<Reply> ready;{std::lock_guard<std::mutex> lock(mutex);ready.swap(replies);}
                for(const auto& reply:ready)for(std::size_t i=0;i<connections.size();++i)if(connections[i].id==reply.connection) {
                    if(!Queue(connections[i],reply.frame)&&reply.frame.operation!=ControlMessage::ACTION_FEEDBACK)Drop(i);
                    break;
                }
                std::vector<pollfd> fds{{wake,POLLIN,0},{listener,POLLIN,0}};
                for(const auto& c:connections)fds.push_back({c.fd,static_cast<short>(POLLIN|((c.connecting||!c.output.empty())?POLLOUT:0)),0});
                int count=poll(fds.data(),fds.size(),10);if(count<0){if(errno==EINTR)continue;error=-errno;break;}
                if(fds[0].revents){std::uint64_t v;const auto n=read(wake,&v,sizeof(v));(void)n;}
                for(std::size_t i=connections.size();i--;) {
                    auto& c=connections[i];auto flags=fds[i+2].revents;bool good=true;
                    if(c.connecting&&(flags&POLLOUT)){int e=0;socklen_t size=sizeof(e);good=!getsockopt(c.fd,SOL_SOCKET,SO_ERROR,&e,&size)&&!e;c.connecting=false;if(good)good=Hello(c);}
                    if(good&&(flags&POLLIN))good=Read(c,peers);
                    if(good&&(flags&POLLOUT)&&!c.connecting)good=Write(c);
                    if(!good||(flags&(POLLERR|POLLHUP|POLLNVAL))){if(!good)++rejected;Drop(i);}
                }
                if(fds[1].revents&POLLIN)Accept();
            }
        }catch(const std::bad_alloc&){error=-ENOMEM;}catch(...){error=-EFAULT;}
        running=false;
        topics.Stop();
        discovery.Stop(); // withdraw on worker failure as well as explicit Stop
        for(std::size_t i=pending.size();i--;)Finish(i,-ECANCELED);
        for(std::size_t i=connections.size();i--;)Drop(i);
        std::lock_guard<std::mutex> lock(mutex);
        for(auto& b:submissions){b->result.error=-ECANCELED;b->promise.set_value(std::move(b->result));}submissions.clear();
    }
    void Execute(Job job) {
        auto& request=job.frame;ControlFrame response=request;response.source=self;response.session_id=session;
        response.operation=request.operation==ControlMessage::PARAM_GET_REQUEST?ControlMessage::PARAM_GET_RESPONSE:ControlMessage::PARAM_SET_RESPONSE;
        response.timeout_ms=0;response.payload.clear();
        auto endpoints=Local();auto entry=std::find_if(endpoints.begin(),endpoints.end(),[&](const auto& e){return e.metadata.identity==request.endpoint;});
        int code=0;auto peers=discovery.GetPeers();const auto* peer=Find(peers,request.source.host);
        if(Clock::now()>=job.deadline)code=-ETIMEDOUT;
        else if(!peer||!Allowed(*peer)||peer->session_id!=request.session_id)code=-ENOTCONN;
        else if(entry==endpoints.end())code=-ENOENT;
        else {
            auto gate=CheckParameterCompatibility(config.discovery.profile,peer->profile,entry->metadata,
                Consumer(request.source,request.name,request.type),config.discovery.compatibility_policy,request.target);
            if(!gate.Compatible()){code=ReasonError(gate.reason);response.reason=gate.reason;}
            else {
                std::uint64_t ticks=0;
                if(!kcf::detail::ReadProcessIdentity(entry->metadata.identity.runtime.pid,ticks)||ticks!=entry->metadata.identity.runtime.process_start_ticks)code=-ESTALE;
                else {
                    auto found=std::find_if(storage.begin(),storage.end(),[&](const auto& s){return s.endpoint==request.endpoint;});
                    if(found==storage.end()) {
                        auto client=std::make_unique<DynamicParameterClient>();code=client->Open(entry->metadata.name,entry->descriptor);
                        if(!code){storage.push_back({request.endpoint,std::move(client)});found=storage.end()-1;}
                    }
                    if(!code)code=ValidateLocalParameterEndpoint(*entry);
                    if(!code) {
                        DynamicPayload value;
                        if(request.operation==ControlMessage::PARAM_GET_REQUEST) {code=found->client->Get(value);if(!code)code=EncodeParameterValue(entry->descriptor,value,response.payload);}
                        else {code=DecodeParameterValue(entry->descriptor,request.payload,value);if(!code)code=found->client->Set(value);}
                    }
                }
            }
        }
        response.status=code;if(code)response.payload.clear();
        {std::lock_guard<std::mutex> lock(mutex);if(replies.size()<64)replies.push_back({job.connection,std::move(response)});}
        Wake();
    }
    void ServiceReply(const Job& job,int code,CompatibilityReason reason,std::vector<std::uint8_t> bytes={}) {
        auto response=job.frame;response.source=self;response.session_id=session;
        response.operation=ResponseOperation(job.frame.operation);response.timeout_ms=0;response.status=code;response.reason=reason;
        response.payload=code?std::vector<std::uint8_t>{}:std::move(bytes);
        if(IsActionMessage(response.operation))response.action_state=code?ActionState::IDLE:ActionState::ACCEPTED;
        {std::lock_guard<std::mutex> lock(mutex);if(replies.size()<64)replies.push_back({job.connection,std::move(response)});}
        Wake();
    }
    void ExecuteService(const Job& job) {
        const auto& request=job.frame;auto entries=Local();
        auto entry=std::find_if(entries.begin(),entries.end(),[&](const auto& e){return e.metadata.identity==request.endpoint;});
        auto peers=discovery.GetPeers();const auto* peer=Find(peers,request.source.host);
        int code=0;auto reason=CompatibilityReason::COMPATIBLE;std::vector<std::uint8_t> wire;
        if(!running||!peer||!Allowed(*peer)||peer->session_id!=request.session_id)code=-ENOTCONN;
        else if(entry==entries.end())code=-ESTALE;
        else {
            const auto gate=CheckServiceCompatibility(config.discovery.profile,peer->profile,entry->metadata,
                Consumer(request.source,request.name,request.type,request.response_type),config.discovery.compatibility_policy,request.target);
            if(!gate.Compatible()){code=ReasonError(gate.reason);reason=gate.reason;}
            else {
                kcf::DynamicPayload input,output;
                code=DecodeParameterValue(entry->descriptor,request.payload,input);
                if(!code)code=ValidateLocalServiceEndpoint(*entry);
                const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(job.deadline-Clock::now()).count();
                if(!code&&remaining<=0)code=-ETIMEDOUT;
                if(!code) {
                    // Existing Local Service API. A fresh client per call avoids
                    // serializing independent endpoints behind a slow callback.
                    DynamicServiceClient client;
                    code=client.Open(entry->service_port,entry->service_id,entry->descriptor,entry->response_descriptor,
                        static_cast<std::uint32_t>(remaining),0); // NO Local or Network retry
                    if(!code)code=client.Call(input,output);
                    if(!code)code=EncodeParameterValue(entry->response_descriptor,output,wire);
                }
            }
        }
        ServiceReply(job,code,reason,std::move(wire));
    }
    void LaunchService(Job job) {
        for(auto& task:service_tasks)if(task.done.load()) {
            if(task.worker.joinable())task.worker.join();
            task.done=false;
            try {task.worker=std::thread([this,&task,job]{
                try{if(job.frame.operation==ControlMessage::ACTION_CANCEL_REQUEST)ExecuteCancel(job);else ExecuteService(job);}catch(...){try{
                    if(job.frame.operation==ControlMessage::ACTION_CANCEL_REQUEST){
                        std::lock_guard<std::mutex> lock(mutex);if(live_connections.count(job.connection))abort_connections.insert(job.connection);Wake();
                    }else ServiceReply(job,-EIO,CompatibilityReason::COMPATIBLE);
                }catch(...){error=-ENOMEM;}}
                task.done=true;
            });}catch(...){task.done=true;ServiceReply(job,-EAGAIN,CompatibilityReason::COMPATIBLE);}
            return;
        }
        ServiceReply(job,-EAGAIN,CompatibilityReason::COMPATIBLE);
    }
    bool Live(std::uint64_t connection){std::lock_guard<std::mutex> lock(mutex);return running&&live_connections.count(connection)&&!abort_connections.count(connection);}
    void ActionReply(std::uint64_t connection,ControlFrame frame){
        std::lock_guard<std::mutex> lock(mutex);
        if(!running||!live_connections.count(connection))return;
        if(frame.operation==ControlMessage::ACTION_FEEDBACK){
            for(auto& reply:replies)if(reply.connection==connection&&reply.frame.operation==ControlMessage::ACTION_FEEDBACK&&reply.frame.goal==frame.goal){reply.frame=std::move(frame);Wake();return;}
            if(replies.size()>=48)return; // reserve terminal/control response capacity
        }
        if(replies.size()>=64){abort_connections.insert(connection);Wake();return;}
        replies.push_back({connection,std::move(frame)});Wake();
    }
    ControlFrame ActionResponse(const Job& job,ControlMessage operation,int status=0){
        auto frame=job.frame;frame.source=self;frame.session_id=session;frame.operation=operation;
        frame.timeout_ms=0;frame.status=status;frame.payload.clear();frame.feedback_sequence=0;
        frame.action_state=status?ActionState::IDLE:ActionState::ACCEPTED;return frame;
    }
    int ActionGate(const Job& job,LocalEndpoint& entry,CompatibilityReason& reason){
        const auto& request=job.frame;auto entries=Local();
        auto found=std::find_if(entries.begin(),entries.end(),[&](const auto& e){return e.metadata.identity==request.endpoint;});
        auto peers=discovery.GetPeers();const auto* peer=Find(peers,request.source.host);
        if(!Live(job.connection)||!peer||!Allowed(*peer)||peer->session_id!=request.session_id)return -ENOTCONN;
        if(found==entries.end())return -ESTALE;
        auto gate=CheckActionCompatibility(config.discovery.profile,peer->profile,found->metadata,
            Consumer(request.source,request.name,request.type,request.response_type,request.feedback_type),config.discovery.compatibility_policy,request.target);
        if(!gate.Compatible()){reason=gate.reason;return ReasonError(reason);}
        if(Clock::now()>=job.deadline)return -ETIMEDOUT;
        entry=*found;return ValidateLocalActionEndpoint(entry);
    }
    void ExecuteAction(const std::shared_ptr<Incoming>& context){
        const auto& job=context->job;LocalEndpoint entry;auto reason=CompatibilityReason::COMPATIBLE;
        int code=ActionGate(job,entry,reason);DynamicPayload goal;bool unknown=false;
        if(!code)code=DecodeParameterValue(entry.descriptor,job.frame.payload,goal);
        const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(job.deadline-Clock::now()).count();
        if(!code&&remaining<=0)code=-ETIMEDOUT;
        if(!code){
            code=context->client.Open(entry.action,entry.descriptor,entry.feedback_descriptor,entry.response_descriptor,
                entry.metadata.identity.runtime.pid,entry.metadata.identity.runtime.process_start_ticks,static_cast<std::uint32_t>(std::min<std::int64_t>(remaining,200)));
            if(!code)code=ValidateLocalActionEndpoint(entry);
            if(!code&&!Live(job.connection))code=-ENOTCONN;
            if(!code)code=context->client.SendGoal(goal,context->local_goal,&unknown);
        }
        auto response=ActionResponse(job,ControlMessage::ACTION_GOAL_RESPONSE,code);response.reason=reason;response.outcome_unknown=unknown;
        context->accepted=!code;ActionReply(job.connection,std::move(response));
        if(code)return;
        std::uint64_t feedback_sequence=0;auto next_result=Clock::time_point{};
        while(Live(job.connection)){
            ActionHeader header;DynamicPayload feedback,result;ActionState state=ActionState::FAILED;
            code=ValidateLocalActionEndpoint(entry);
            if(!code){
                std::lock_guard<std::mutex> lock(context->call_mutex);
                code=context->client.ReadFeedback(header,feedback);
                if(!code&&header.goal_id==context->local_goal&&header.feedback_sequence>feedback_sequence&&header.state==ActionState::RUNNING){
                    auto event=ActionResponse(job,ControlMessage::ACTION_FEEDBACK);event.request_id=0;event.action_state=ActionState::RUNNING;event.feedback_sequence=header.feedback_sequence;
                    code=EncodeParameterValue(entry.feedback_descriptor,feedback,event.payload);
                    if(!code){feedback_sequence=header.feedback_sequence;ActionReply(job.connection,std::move(event));}
                }
                if(code==-EAGAIN)code=0;
                if(!code&&Clock::now()>=next_result){
                    code=context->client.GetResult(context->local_goal,state,result);next_result=Clock::now()+std::chrono::milliseconds(50);
                    if(!code){auto event=ActionResponse(job,ControlMessage::ACTION_RESULT);event.request_id=0;event.action_state=state;
                        event.status=EncodeParameterValue(entry.response_descriptor,result,event.payload);if(event.status)event.payload.clear();
                        ActionReply(job.connection,std::move(event));return;
                    }
                    if(code==-EINPROGRESS)code=0;
                }
            }
            if(code){auto event=ActionResponse(job,ControlMessage::ACTION_RESULT,code);event.request_id=0;event.action_state=ActionState::FAILED;ActionReply(job.connection,std::move(event));return;}
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // Lost connection releases only gateway resources. Never Local Cancel.
    }
    void LaunchAction(Job job){
        {std::lock_guard<std::mutex> lock(mutex);
            incoming.erase(std::remove_if(incoming.begin(),incoming.end(),[](const auto& c){return c->done.load();}),incoming.end());
            for(const auto& c:incoming)if(c->job.frame.goal==job.frame.goal){abort_connections.insert(job.connection);return;}
        }
        for(auto& task:action_tasks)if(task.done.load()){
            if(task.worker.joinable())task.worker.join();
            auto context=std::make_shared<Incoming>();context->job=job;
            {std::lock_guard<std::mutex> lock(mutex);incoming.push_back(context);}
            task.done=false;
            try{task.worker=std::thread([this,&task,context]{
                try{ExecuteAction(context);}catch(...){std::lock_guard<std::mutex> lock(mutex);if(live_connections.count(context->job.connection))abort_connections.insert(context->job.connection);Wake();}
                {std::lock_guard<std::mutex> lock(context->call_mutex);context->done=true;context->client.Close();}
                {std::lock_guard<std::mutex> lock(mutex);incoming.erase(std::remove(incoming.begin(),incoming.end(),context),incoming.end());}
                task.done=true;
            });}catch(...){context->done=true;task.done=true;ActionReply(job.connection,ActionResponse(job,ControlMessage::ACTION_GOAL_RESPONSE,-EAGAIN));}
            return;
        }
        ActionReply(job.connection,ActionResponse(job,ControlMessage::ACTION_GOAL_RESPONSE,-EAGAIN));
    }
    void ExecuteCancel(const Job& job){
        LocalEndpoint entry;auto reason=CompatibilityReason::COMPATIBLE;int code=ActionGate(job,entry,reason);bool unknown=false;
        std::shared_ptr<Incoming> context;
        if(!code){std::lock_guard<std::mutex> lock(mutex);
            for(const auto& c:incoming)if(c->job.connection==job.connection&&c->job.frame.goal==job.frame.goal&&
                c->job.frame.endpoint==job.frame.endpoint&&c->accepted&&!c->done){context=c;break;}
            if(!context)code=-ENOENT;
        }
        if(!code){std::lock_guard<std::mutex> lock(context->call_mutex);
            if(context->done||Clock::now()>=job.deadline)code=-ETIMEDOUT;
            else code=context->client.Cancel(context->local_goal,&unknown);
        }
        auto response=ActionResponse(job,ControlMessage::ACTION_CANCEL_RESPONSE,code);response.reason=reason;response.outcome_unknown=unknown;ActionReply(job.connection,std::move(response));
    }
    void Backend() noexcept {
        try {
            auto next=Clock::time_point{};
            while(running) {
                if(Clock::now()>=next) {
                    std::vector<LocalEndpoint> collected;int code=CollectLocalMetadata(config.metadata,collected);
                    if(code){error=code;collected.clear();}
                    std::vector<NetworkEndpointMetadata> list;for(const auto& e:collected)list.push_back(e.metadata);
                    if(config.topics.enabled)topics.UpdateLocal(collected);
                    code=discovery.UpdateMetadata(list);if(code&&running)error=code;
                    storage.erase(std::remove_if(storage.begin(),storage.end(),[&](const auto& s){return std::none_of(collected.begin(),collected.end(),[&](const auto& e){return e.metadata.identity==s.endpoint;});}),storage.end());
                    {std::lock_guard<std::mutex> lock(mutex);local=std::move(collected);}
                    next=Clock::now()+config.metadata_interval;
                }
                for(unsigned i=0;i<8&&running;++i) {
                    Job job;{std::lock_guard<std::mutex> lock(mutex);if(jobs.empty())break;job=std::move(jobs.front());jobs.pop_front();}
                    if(job.frame.operation==ControlMessage::ACTION_GOAL_REQUEST)LaunchAction(std::move(job));
                    else if(job.frame.operation==ControlMessage::SERVICE_REQUEST||job.frame.operation==ControlMessage::ACTION_CANCEL_REQUEST)LaunchService(std::move(job));
                    else Execute(std::move(job));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }catch(const std::bad_alloc&){error=-ENOMEM;running=false;Wake();}catch(...){error=-EFAULT;running=false;Wake();}
        for(auto& task:service_tasks)if(task.worker.joinable())task.worker.join();
        for(auto& task:action_tasks)if(task.worker.joinable())task.worker.join();
        {std::lock_guard<std::mutex> lock(mutex);incoming.clear();}
        storage.clear();
    }
    std::future<ParameterBatchResult> Submit(const RemoteTarget& target,const std::string& name,const TypeIdentity& type,
        const std::vector<std::uint8_t>& value,bool set,std::chrono::milliseconds timeout,const TypeIdentity& response_type={},const TypeIdentity& feedback_type={},std::shared_ptr<RemoteActionSession> cancel={}) {
        auto b=std::make_shared<Batch>();auto future=b->promise.get_future();
        const bool action=!(feedback_type==TypeIdentity{});
        const bool service=!action&&!(response_type==TypeIdentity{});
        int code=0;if((service||action)&&(!Valid(response_type)||response_type.payload_size>MAX_PARAMETER_WIRE))code=-EINVAL;
        if(!Valid(target)||!Text(name,240)||name[0]!='/'||!Valid(type)||type.payload_size>MAX_PARAMETER_WIRE||
            timeout.count()<1||timeout.count()>60000||((set||service||(action&&!cancel))?value.size()!=type.payload_size:!value.empty()))code=-EINVAL;
        if(action&&(!Valid(feedback_type)||feedback_type.payload_size>MAX_PARAMETER_WIRE))code=-EINVAL;
        b->action=action;b->feedback_type=feedback_type;b->cancel=std::move(cancel);
        b->target=target;b->name=name;b->type=type;b->value=value;b->set=set;b->service=service;b->response_type=response_type;b->deadline=Clock::now()+timeout;
        std::lock_guard<std::mutex> lock(mutex);
        if(!code&&!running)code=-ENOTCONN;
        if(!code&&submissions.size()>=64)code=-EAGAIN;
        if(code){b->result.error=code;b->promise.set_value(std::move(b->result));}
        else{submissions.push_back(b);Wake();}return future;
    }
};
ControlNode::ControlNode():impl_(std::make_unique<Impl>()){}
ControlNode::~ControlNode(){Stop();}
int ControlNode::Start(const ControlConfig& input) {
    auto& s=*impl_;if(s.io.joinable()||s.backend.joinable())return -EBUSY;
    if(input.metadata_interval.count()<20||input.metadata_interval.count()>60000||input.connect_timeout.count()<20||
        input.connect_timeout.count()>60000||input.reconnect_interval.count()<20||input.reconnect_interval.count()>60000)return -EINVAL;
    s.config=input;s.config.discovery.profile.capabilities=Supported();
    if(input.topics.enabled)s.config.discovery.profile.capabilities|=CapabilityBit(Capability::REMOTE_TOPIC);
    s.config.metadata.host=input.discovery.host;s.config.metadata.boot_id=input.discovery.boot_id;
    s.self={input.discovery.host,input.discovery.boot_id,getpid(),0};
    if(!Valid(s.self.host)||!Valid(s.self.boot_id)||!kcf::detail::ReadProcessIdentity(getpid(),s.self.process_start_ticks))return -EINVAL;
    for(const auto& name:s.config.metadata.remote_parameters)if(!Text(name,240)||name[0]!='/')return -EINVAL;
    for(const auto& name:s.config.metadata.remote_topics)if(!Text(name,240)||name[0]!='/')return -EINVAL;
    for(const auto& name:s.config.metadata.remote_actions)if(!Text(name,240)||name[0]!='/')return -EINVAL;
    for(const auto& name:s.config.metadata.remote_services)if(!Text(name,240)||name[0]!='/')return -EINVAL;
    for(const auto& group:s.config.metadata.groups)if(!Text(group,128))return -EINVAL;
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(input.discovery.metadata_port);
    if(inet_pton(AF_INET,input.discovery.interface_address.c_str(),&address.sin_addr)!=1)return -EINVAL;
    s.listener=socket(AF_INET,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(s.listener<0)return -errno;
    auto fail=[&](int code){s.topics.Stop();s.Close();return code;};int one=1;
    if(setsockopt(s.listener,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one))||bind(s.listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))||listen(s.listener,32))return fail(-errno);
    socklen_t length=sizeof(address);if(getsockname(s.listener,reinterpret_cast<sockaddr*>(&address),&length))return fail(-errno);
    s.config.discovery.metadata_port=ntohs(address.sin_port);
    s.wake=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);if(s.wake<0)return fail(-errno);
    int code=0;
    if(input.topics.enabled){code=s.topics.Start(s.config.topics,s.config.discovery,s.config.metadata,s.self,s.discovery);if(code)return fail(code);}
    code=s.discovery.Start(s.config.discovery);if(code)return fail(code);
    s.session=s.discovery.GetSessionId();s.error=0;s.connected=0;s.sent=0;s.ignored=0;s.rejected=0;s.running=true;s.seen.clear();
    try{s.backend=std::thread([&s]{s.Backend();});s.io=std::thread([&s]{s.IO();});}
    catch(...){Stop();return -EAGAIN;}return 0;
}
void ControlNode::Stop() {
    auto& s=*impl_;s.running=false;s.Wake();
    if(s.io.joinable())s.io.join();
    s.topics.Stop();
    if(s.backend.joinable())s.backend.join();
    s.discovery.Stop();std::lock_guard<std::mutex> lock(s.mutex);s.jobs.clear();s.replies.clear();s.local.clear();s.live_connections.clear();s.abort_connections.clear();s.Close();
}
std::future<ParameterBatchResult> ControlNode::Get(const RemoteTarget& t,const std::string& name,const TypeIdentity& type,std::chrono::milliseconds timeout){return impl_->Submit(t,name,type,{},false,timeout);}
std::future<ParameterBatchResult> ControlNode::Set(const RemoteTarget& t,const std::string& name,const TypeIdentity& type,const std::vector<std::uint8_t>& value,std::chrono::milliseconds timeout){return impl_->Submit(t,name,type,value,true,timeout);}
std::future<ServiceBatchResult> ControlNode::CallService(const RemoteTarget& t,const std::string& name,const TypeIdentity& request,
    const TypeIdentity& response,const std::vector<std::uint8_t>& value,std::chrono::milliseconds timeout){
    if(response==TypeIdentity{}) {std::promise<ServiceBatchResult> p;auto f=p.get_future();ServiceBatchResult r;r.error=-EINVAL;p.set_value(std::move(r));return f;}
    return impl_->Submit(t,name,request,value,false,timeout,response);
}
std::future<ActionBatchResult> ControlNode::StartAction(const RemoteTarget& target,const std::string& name,const TypeIdentity& goal,
    const TypeIdentity& feedback,const TypeIdentity& result,const std::vector<std::uint8_t>& payload,std::chrono::milliseconds timeout){
    if(!Valid(goal)||!Valid(feedback)||!Valid(result)){std::promise<ActionBatchResult> promise;auto future=promise.get_future();ActionBatchResult out;out.error=-EINVAL;promise.set_value(std::move(out));return future;}
    return impl_->Submit(target,name,goal,payload,false,timeout,result,feedback);
}
std::future<ActionBatchResult> ControlNode::CancelAction(const std::shared_ptr<RemoteActionSession>& action,std::chrono::milliseconds timeout){
    int code=0;ControlFrame original;
    if(!action)code=-EINVAL;
    else {std::lock_guard<std::mutex> lock(action->mutex_);original=action->request_;
        if(!(original.goal.source==impl_->self)||original.session_id!=impl_->session)code=-EACCES;
        else if(!action->snapshot_.accepted||action->snapshot_.terminal||action->snapshot_.communication_lost)code=-ENOENT;
    }
    if(code){std::promise<ActionBatchResult> promise;auto future=promise.get_future();ActionBatchResult out;out.error=code;promise.set_value(std::move(out));return future;}
    RemoteTarget target;target.scope=TargetScope::ENDPOINT;target.endpoint=original.endpoint;
    return impl_->Submit(target,original.name,original.type,{},false,timeout,original.response_type,original.feedback_type,action);
}
std::vector<PeerInfo> ControlNode::GetPeers() const{return impl_->discovery.GetPeers();}
ControlStats ControlNode::GetStats() const{return {impl_->connected.load(),impl_->sent.load(),impl_->ignored.load(),impl_->rejected.load()};}
TopicDiagnostics ControlNode::GetTopicDiagnostics() const{return impl_->topics.GetDiagnostics();}
int ControlNode::GetLastError() const{return impl_->error.load();}
}
