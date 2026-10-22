#include "control_client_io.hpp"
#include "kcf/network/remote_control_client.hpp"
#include "kcf/network/control_frame.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <limits>
#include <new>
#include <condition_variable>
#include <thread>

namespace kcf::network {
namespace {
using Clock = std::chrono::steady_clock;
std::atomic<std::uint64_t> next_request{0};
int NextRequest(std::uint64_t& id) {
    auto previous=next_request.load();
    do {if(previous==std::numeric_limits<std::uint64_t>::max())return -EOVERFLOW;}
    while(!next_request.compare_exchange_weak(previous,previous+1));
    id=previous+1;return 0;
}
using namespace client_io;
int Peer(PeerDiscovery& discovery,const DiscoveryConfig& config,const HostIdentity& host,PeerInfo& out,Capability capability) {
    if(discovery.GetLastError())return discovery.GetLastError();
    for(const auto& peer:discovery.GetPeers())if(peer.host==host) {
        if(peer.state!=PeerState::ONLINE)return -ENOTCONN;
        if(!CheckPeerCompatibility(config.profile,peer.profile,config.compatibility_policy).Compatible())return -EPROTONOSUPPORT;
        if(!(peer.profile.capabilities&CapabilityBit(capability)))return -ENOTSUP;
        in_addr ip{};
        if(!Valid(peer.host)||!Valid(peer.boot_id)||!peer.session_id||!peer.port||
           inet_pton(AF_INET,peer.address.c_str(),&ip)!=1||!ip.s_addr||
           ip.s_addr==INADDR_BROADCAST||IN_MULTICAST(ntohl(ip.s_addr)))return -EPROTO;
        out=peer;return 0;
    }
    return -ENOENT;
}
bool Identity(const ControlFrame& frame,const PeerInfo& peer) {
    return frame.source.host==peer.host && frame.source.boot_id==peer.boot_id && frame.session_id==peer.session_id;
}
}
struct RemoteControlActionHandle::State {
    struct Cancel {
        RemoteActionCancelResult result;Clock::time_point deadline;
        std::uint64_t id{0};bool sent{false},done{false};
    };
    mutable std::mutex mutex;std::condition_variable cv;std::thread worker;
    RemoteActionSnapshot snapshot;ControlFrame request;
    Clock::time_point ack_deadline;std::chrono::milliseconds tracking_timeout{60000};
    std::uint64_t generation{0};bool ack_done{false},ack_received{false},ack_unknown{false},goal_sent{false},done{false};
    int ack_error{0};std::shared_ptr<Cancel> cancel;
};
RemoteActionSnapshot RemoteControlActionHandle::Snapshot() const {
    std::lock_guard<std::mutex> lock(state_->mutex);return state_->snapshot;
}
RemoteControlClient::RemoteControlClient(PeerDiscovery& discovery,const DiscoveryConfig& config)
    :discovery_(discovery),config_(config) {}
RemoteControlClient::~RemoteControlClient() { Stop(); }
int RemoteControlClient::Start() {
    if(running_)return -EBUSY;
    if(!Valid(config_.host)||!Valid(config_.boot_id)||
       !IsObserverProfile(config_.profile))return -EINVAL;
    if(!discovery_.GetSessionId())return -ENOTCONN;
    std::lock_guard<std::timed_mutex> lock(mutex_);
    ++generation_;canceled_=false;running_=true;return 0;
}
void RemoteControlClient::Stop() {
    running_=false;canceled_=true;++generation_;
    std::lock_guard<std::timed_mutex> lock(mutex_);
    std::lock_guard<std::mutex> actions_lock(actions_mutex_);
    for(auto& state:actions_)if(state->worker.joinable())state->worker.join();
    actions_.clear();
}
int RemoteControlClient::GetParameter(const RemoteEndpointIdentity& endpoint,std::uint64_t gateway_session,
    const std::string& name,const TypeIdentity& type,std::vector<std::uint8_t>& value,std::chrono::milliseconds timeout) {
    return ControlRequest(endpoint,gateway_session,name,type,value,timeout,nullptr,nullptr);
}
RemoteParameterSetResult RemoteControlClient::SetParameter(const RemoteEndpointIdentity& endpoint,
    std::uint64_t gateway_session,const std::string& name,const TypeIdentity& type,
    const std::vector<std::uint8_t>& payload,std::chrono::milliseconds timeout) {
    RemoteParameterSetResult result;std::vector<std::uint8_t> unused;
    result.error=ControlRequest(endpoint,gateway_session,name,type,unused,timeout,&payload,&result);
    return result;
}
RemoteServiceCallResult RemoteControlClient::CallService(const RemoteEndpointIdentity& endpoint,
    std::uint64_t session,const std::string& name,const TypeIdentity& request_type,const TypeIdentity& response_type,
    const std::vector<std::uint8_t>& request,std::chrono::milliseconds timeout) {
    RemoteServiceCallResult result;RemoteParameterSetResult status;
    result.error=ControlRequest(endpoint,session,name,request_type,result.value,timeout,&request,&status,&response_type);
    result.response_received=status.response_received;result.execution_unknown=status.execution_unknown;
    return result;
}
int RemoteControlClient::QueryTypeDescriptor(const RemoteEndpointIdentity& endpoint,std::uint64_t session,
    const std::string& name,DescriptorRole role,const TypeIdentity& type,TypeDescriptor& output,std::chrono::milliseconds timeout) {
    if(role<DescriptorRole::VALUE||role>DescriptorRole::FEEDBACK)return -EINVAL;
    try {std::vector<std::uint8_t> bytes;int code=ControlRequest(endpoint,session,name,type,bytes,timeout,nullptr,nullptr,nullptr,role);
        return code?code:DecodeTypeDescriptor(bytes,type,output);
    }catch(const std::bad_alloc&){return -ENOMEM;}
}
int RemoteControlClient::ControlRequest(const RemoteEndpointIdentity& endpoint,std::uint64_t gateway_session,
    const std::string& name,const TypeIdentity& type,std::vector<std::uint8_t>& value,
    std::chrono::milliseconds timeout,const std::vector<std::uint8_t>* payload,RemoteParameterSetResult* set_result,const TypeIdentity* response_type,DescriptorRole role) {
    const bool descriptor=role!=DescriptorRole::NONE;
    const bool service=response_type!=nullptr;
    if(service&&(!Valid(*response_type)||response_type->payload_size>MAX_PARAMETER_WIRE))return -EINVAL;
    if(payload && (payload->size()!=type.payload_size||payload->size()>MAX_PARAMETER_WIRE))return -EMSGSIZE;
    if(timeout.count()<1||timeout.count()>60000||!Valid(endpoint)||!gateway_session||
       endpoint.runtime.host==config_.host||!Text(name,240)||name[0]!='/'||!Valid(type)||type.payload_size>MAX_PARAMETER_WIRE)return -EINVAL;
    const auto generation=generation_.load();
    if(!running_)return -ENOTCONN;
    const auto deadline=Clock::now()+timeout;
    std::unique_lock<std::timed_mutex> lock(mutex_,std::defer_lock);
    while(!lock.try_lock_for(std::chrono::milliseconds(10))) {
        if(!running_||generation_!=generation)return -ECANCELED;
        if(Clock::now()>=deadline)return -ETIMEDOUT;
    }
    if(!running_||generation_!=generation)return -ECANCELED;
    const auto& gateway=endpoint.runtime.host;
    try {
        const auto session=discovery_.GetSessionId();if(!session)return -ENOTCONN;
        PeerInfo peer;int code=Peer(discovery_,config_,gateway,peer,descriptor?Capability::TYPE_DESCRIPTOR_QUERY:service?Capability::REMOTE_SERVICE:Capability::REMOTE_PARAMETER);if(code)return code;
        const auto selected=[&](const PeerInfo& p) {
            if(p.boot_id!=endpoint.runtime.boot_id||p.session_id!=gateway_session)return -ESTALE;
            if(!p.metadata_ready)return -EAGAIN;
            for(const auto& e:p.endpoints)if(e.identity==endpoint) {
                if(descriptor){
                    if(e.name!=name)return -ENOENT;
                    if(e.scope!=NetworkScope::REMOTE)return -EACCES;
                    TypeIdentity selected;int error=DescriptorType(e,role,selected);if(error)return error;
                    return selected==type?0:-EPROTOTYPE;
                }
                if(e.kind!=(service?EndpointKind::SERVICE:EndpointKind::PARAMETER)||e.role!=EndpointRole::PROVIDER||e.name!=name)return -ENOENT;
                if(e.scope!=NetworkScope::REMOTE)return -EACCES;
                if(service ? (!(e.request_type==type)||!(e.response_type==*response_type)) : !(e.value_type==type))return -EPROTOTYPE;
                return 0;
            }
            return -ENOENT;
        };
        code=selected(peer);if(code)return code;
        const auto fresh=[&] {
            if(canceled_||generation_!=generation)return -ECANCELED;
            if(discovery_.GetSessionId()!=session)return -ESTALE;
            PeerInfo current;int error=Peer(discovery_,config_,gateway,current,descriptor?Capability::TYPE_DESCRIPTOR_QUERY:service?Capability::REMOTE_SERVICE:Capability::REMOTE_PARAMETER);if(error)return error;
            error=selected(current);if(error)return error;
            return current.boot_id==peer.boot_id && current.session_id==peer.session_id &&
                   current.address==peer.address && current.port==peer.port?0:-ESTALE;
        };
        ControlFrame hello;hello.source={config_.host,config_.boot_id,getpid(),0};hello.session_id=session;
        if(!kcf::detail::ReadProcessIdentity(getpid(),hello.source.process_start_ticks))return -ESRCH;
        Socket socket;ControlFrame greeting;
        code=ConnectHello(socket,config_,peer,hello,greeting,deadline,&canceled_);if(code)return code;
        if(!Identity(greeting,peer))return -ESTALE;
        code=fresh();if(code)return code;
        auto request=hello;request.descriptor_role=role;request.operation=descriptor?ControlMessage::DESCRIPTOR_REQUEST:service?ControlMessage::SERVICE_REQUEST:payload?ControlMessage::PARAM_SET_REQUEST:ControlMessage::PARAM_GET_REQUEST;request.target=TargetScope::ENDPOINT;
        request.endpoint=endpoint;request.name=name;request.type=type;
        if(service)request.response_type=*response_type;
        const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
        if(remaining<1)return -ETIMEDOUT;
        request.timeout_ms=static_cast<std::uint32_t>(remaining);
        code=NextRequest(request.request_id);if(code)return code;
        if(payload)request.payload=*payload;
        // Encoding failure is known-not-sent. Transfer failures are conservative:
        // the existing transport does not expose how much reached the gateway.
        if(payload) { std::vector<std::uint8_t> checked;code=EncodeControlFrame(request,checked);if(code)return code;set_result->execution_unknown=true; }
        code=SendFrame(socket.fd,request,deadline,&canceled_);if(code)return code;
        ControlFrame response;code=ReceiveFrame(socket.fd,descriptor?ControlMessage::DESCRIPTOR_RESPONSE:service?ControlMessage::SERVICE_RESPONSE:payload?ControlMessage::PARAM_SET_RESPONSE:ControlMessage::PARAM_GET_RESPONSE,response,deadline,&canceled_);if(code)return code;
        if(response.descriptor_role!=role||response.request_id!=request.request_id||response.endpoint!=endpoint||response.name!=name||
            !(response.type==type)||(service&&!(response.response_type==*response_type))||response.target!=TargetScope::ENDPOINT)return -EPROTO;
        if(!Identity(response,peer)||response.source!=greeting.source)return -ESTALE;
        code=fresh();if(code)return code;
        if(Clock::now()>=deadline)return -ETIMEDOUT;
        if(payload) {
            set_result->response_received=true;
            if(service&&!response.status) {
                if(response.payload.size()!=response_type->payload_size)return -EMSGSIZE;
                value=std::move(response.payload);
            }
            if(!response.status||response.status==-EACCES)set_result->execution_unknown=false;
            return response.status;
        }
        if(response.status)return response.status;
        if(!descriptor&&response.payload.size()!=type.payload_size)return -EMSGSIZE;
        if(Clock::now()>=deadline)return -ETIMEDOUT;
        value=std::move(response.payload);return 0;
    } catch(const std::bad_alloc&) {return -ENOMEM;}
}

RemoteActionStartResult RemoteControlClient::StartAction(const RemoteEndpointIdentity& endpoint,std::uint64_t session,
    const std::string& name,const TypeIdentity& goal,const TypeIdentity& feedback,const TypeIdentity& result,
    const std::vector<std::uint8_t>& payload,std::chrono::milliseconds timeout,std::chrono::milliseconds tracking) {
    RemoteActionStartResult out;
    if(!Valid(endpoint)||!session||endpoint.runtime.host==config_.host||!Text(name,240)||name[0]!='/'||
       !Valid(goal)||!Valid(feedback)||!Valid(result)||goal.payload_size>MAX_PARAMETER_WIRE||
       feedback.payload_size>MAX_PARAMETER_WIRE||result.payload_size>MAX_PARAMETER_WIRE||payload.size()!=goal.payload_size||
       timeout.count()<1||timeout.count()>60000||tracking.count()<1||tracking.count()>3600000){out.error=-EINVAL;return out;}
    const auto generation=generation_.load();
    try {
        auto state=std::make_shared<RemoteControlActionHandle::State>();
        state->request.endpoint=endpoint;state->request.session_id=session;state->request.name=name;state->request.type=goal;
        state->request.feedback_type=feedback;state->request.response_type=result;state->request.payload=payload;
        state->ack_deadline=Clock::now()+timeout;state->tracking_timeout=tracking;state->generation=generation;
        state->snapshot.endpoint=endpoint;
        {
            std::lock_guard<std::mutex> lock(actions_mutex_);
            if(!running_||generation_!=generation){out.error=-ENOTCONN;return out;}
            for(auto i=actions_.begin();i!=actions_.end();) {
                bool done;{std::lock_guard<std::mutex> guard((*i)->mutex);done=(*i)->done;}
                if(done){if((*i)->worker.joinable())(*i)->worker.join();i=actions_.erase(i);}else ++i;
            }
            if(actions_.size()>=4){out.error=-EAGAIN;return out;}
            out.handle=std::shared_ptr<RemoteControlActionHandle>(new RemoteControlActionHandle(state));
            actions_.push_back(state);
            try {state->worker=std::thread([this,state]{RunAction(state);});}
            catch(...){actions_.pop_back();out.handle.reset();out.error=-EAGAIN;return out;}
        }
        std::unique_lock<std::mutex> lock(state->mutex);state->cv.wait(lock,[&]{return state->ack_done;});
        out.error=state->ack_error;out.response_received=state->ack_received;out.execution_unknown=state->ack_unknown;
    }catch(const std::bad_alloc&){out.error=-ENOMEM;}
    return out;
}
RemoteActionCancelResult RemoteControlClient::CancelAction(const std::shared_ptr<RemoteControlActionHandle>& handle,
    std::chrono::milliseconds timeout) {
    RemoteActionCancelResult out;
    if(!handle||timeout.count()<1||timeout.count()>60000){out.error=-EINVAL;return out;}
    try {
        auto state=handle->state_;auto cancel=std::make_shared<RemoteControlActionHandle::State::Cancel>();
        cancel->deadline=Clock::now()+timeout;
        {
            std::lock_guard<std::mutex> lock(actions_mutex_);
            if(std::find(actions_.begin(),actions_.end(),state)==actions_.end()){out.error=-ENOENT;return out;}
            std::lock_guard<std::mutex> guard(state->mutex);
            if(!running_||state->generation!=generation_){out.error=-ECANCELED;return out;}
            if(state->done||state->snapshot.terminal||!state->snapshot.accepted){out.error=-EALREADY;return out;}
            if(state->cancel&&!state->cancel->done){out.error=-EBUSY;return out;}
            state->cancel=cancel;
        }
        std::unique_lock<std::mutex> lock(state->mutex);state->cv.wait(lock,[&]{return cancel->done;});
        return cancel->result;
    }catch(const std::bad_alloc&){out.error=-ENOMEM;return out;}
}
void RemoteControlClient::RunAction(const std::shared_ptr<RemoteControlActionHandle::State>& state) {
    auto finish=[&](int error) {
        std::lock_guard<std::mutex> lock(state->mutex);
        if(!state->ack_done){state->ack_done=true;state->ack_error=error;state->ack_unknown=state->goal_sent;}
        if(error&&!state->snapshot.terminal) {
            state->snapshot.error=error;
            if(!state->ack_received||state->snapshot.accepted) {
                state->snapshot.communication_lost=state->goal_sent;
                state->snapshot.execution_unknown=state->goal_sent;
            }
        }
        if(state->cancel&&!state->cancel->done){auto& c=*state->cancel;c.done=true;c.result.error=error?error:-EALREADY;
            c.result.cancel_unknown=c.sent;state->snapshot.cancel_unknown|=c.sent;}
        state->done=true;state->cv.notify_all();
    };
    try {
        auto request=state->request;
        PeerInfo peer;int code=Peer(discovery_,config_,request.endpoint.runtime.host,peer,Capability::REMOTE_ACTION);
        if(code){finish(code);return;}
        const auto expected=state->request.session_id;
        auto validate=[&](const PeerInfo& p) {
            if(p.boot_id!=request.endpoint.runtime.boot_id||p.session_id!=expected)return -ESTALE;
            if(!p.metadata_ready)return -EAGAIN;
            for(const auto& e:p.endpoints)if(e.identity==request.endpoint) {
                if(e.kind!=EndpointKind::ACTION||e.role!=EndpointRole::PROVIDER||e.name!=request.name)return -ENOENT;
                if(e.scope!=NetworkScope::REMOTE)return -EACCES;
                if(!(e.request_type==request.type)||!(e.feedback_type==request.feedback_type)||!(e.response_type==request.response_type))return -EPROTOTYPE;
                return 0;
            }
            return -ENOENT;
        };
        code=validate(peer);if(code){finish(code);return;}
        const auto local_session=discovery_.GetSessionId();
        auto fresh=[&] {
            if(canceled_||generation_!=state->generation)return -ECANCELED;
            if(!local_session||discovery_.GetSessionId()!=local_session)return -ESTALE;
            PeerInfo p;int error=Peer(discovery_,config_,request.endpoint.runtime.host,p,Capability::REMOTE_ACTION);if(error)return error;
            error=validate(p);if(error)return error;
            return p.address==peer.address&&p.port==peer.port?0:-ESTALE;
        };
        ControlFrame hello;hello.source={config_.host,config_.boot_id,getpid(),0};hello.session_id=local_session;
        if(!kcf::detail::ReadProcessIdentity(getpid(),hello.source.process_start_ticks)){finish(-ESRCH);return;}
        Socket socket;ControlFrame greeting;
        code=ConnectHello(socket,config_,peer,hello,greeting,state->ack_deadline,&canceled_);
        if(code){finish(code);return;}
        if(!Identity(greeting,peer)){finish(-ESTALE);return;}
        code=fresh();if(code){finish(code);return;}
        request.source=hello.source;request.session_id=local_session;request.operation=ControlMessage::ACTION_GOAL_REQUEST;
        code=NextRequest(request.request_id);if(code){finish(code);return;}
        request.goal={hello.source,request.request_id,local_session};
        auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(state->ack_deadline-Clock::now()).count();
        if(remaining<1){finish(-ETIMEDOUT);return;}request.timeout_ms=static_cast<std::uint32_t>(remaining);
        std::vector<std::uint8_t> encoded;code=EncodeControlFrame(request,encoded);if(code){finish(code);return;}
        {std::lock_guard<std::mutex> lock(state->mutex);state->goal_sent=true;state->snapshot.goal=request.goal;}
        code=SendFrame(socket.fd,request,state->ack_deadline,&canceled_);if(code){finish(code);return;}
        FrameReader reader;auto tracking_deadline=Clock::time_point::max();bool accepted=false,terminal=false;
        for(;;) {
            code=fresh();if(code){finish(code);return;}
            auto now=Clock::now();
            if((!accepted&&now>=state->ack_deadline)||(!terminal&&now>=tracking_deadline)){finish(-ETIMEDOUT);return;}
            std::shared_ptr<RemoteControlActionHandle::State::Cancel> cancel;
            {std::lock_guard<std::mutex> lock(state->mutex);cancel=state->cancel;}
            if(cancel&&!cancel->done) {
                if(now>=cancel->deadline||(terminal&&!cancel->sent)) {
                    std::lock_guard<std::mutex> lock(state->mutex);cancel->done=true;
                    cancel->result.error=terminal&&!cancel->sent?-EALREADY:-ETIMEDOUT;
                    cancel->result.cancel_unknown=cancel->sent;state->snapshot.cancel_unknown|=cancel->sent;state->cv.notify_all();
                } else if(!cancel->sent) {
                    auto frame=request;frame.operation=ControlMessage::ACTION_CANCEL_REQUEST;frame.payload.clear();
                    code=NextRequest(frame.request_id);if(code){finish(code);return;}
                    frame.timeout_ms=static_cast<std::uint32_t>(std::max<std::int64_t>(1,std::chrono::duration_cast<std::chrono::milliseconds>(cancel->deadline-now).count()));
                    cancel->id=frame.request_id;cancel->sent=true;
                    code=SendFrame(socket.fd,frame,cancel->deadline,&canceled_);if(code){finish(code);return;}
                }
            }
            if(terminal&&(!cancel||cancel->done)){finish(0);return;}
            ControlFrame frame;code=reader.Read(socket.fd,frame);
            if(code<0){finish(code);return;}
            if(!code){pollfd p{socket.fd,POLLIN,0};if(poll(&p,1,10)<0&&errno!=EINTR){finish(-errno);return;}continue;}
            code=fresh();if(code){finish(code);return;}
            if((!accepted&&Clock::now()>=state->ack_deadline)||(!terminal&&Clock::now()>=tracking_deadline)){finish(-ETIMEDOUT);return;}
            if(!Identity(frame,peer)||frame.source!=greeting.source){finish(-ESTALE);return;}
            if(frame.goal!=request.goal||frame.endpoint!=request.endpoint||frame.name!=request.name||
               frame.target!=TargetScope::ENDPOINT||!(frame.type==request.type)||!(frame.feedback_type==request.feedback_type)||!(frame.response_type==request.response_type)){
                finish(-EPROTO);return;
            }
            if(frame.operation==ControlMessage::ACTION_GOAL_RESPONSE) {
                if(frame.request_id!=request.request_id){finish(-EPROTO);return;}
                if(accepted)continue; // duplicate ACK cannot reset an active handle
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->ack_received=true;state->ack_done=true;state->ack_error=frame.status;
                    state->ack_unknown=frame.outcome_unknown;state->snapshot.execution_unknown=frame.outcome_unknown;
                    state->snapshot.accepted=!frame.status;state->snapshot.state=frame.action_state;state->snapshot.error=frame.status;
                    state->cv.notify_all();
                }
                if(frame.status){finish(frame.status);return;}
                accepted=true;tracking_deadline=Clock::now()+state->tracking_timeout;
            } else if(frame.operation==ControlMessage::ACTION_FEEDBACK||frame.operation==ControlMessage::ACTION_RESULT) {
                if(!accepted||frame.request_id){finish(-EPROTO);return;}
                std::lock_guard<std::mutex> lock(state->mutex);
                if(state->snapshot.terminal)continue;
                if(frame.operation==ControlMessage::ACTION_FEEDBACK) {
                    if(frame.feedback_sequence>state->snapshot.feedback_sequence) {
                        state->snapshot.feedback_sequence=frame.feedback_sequence;state->snapshot.feedback=std::move(frame.payload);state->snapshot.state=frame.action_state;
                    }
                } else {
                    if(frame.status){code=frame.status;}else {
                        state->snapshot.terminal=true;state->snapshot.state=frame.action_state;state->snapshot.result=std::move(frame.payload);
                        terminal=true;
                    }
                }
            } else if(frame.operation==ControlMessage::ACTION_CANCEL_RESPONSE) {
                if(!accepted){finish(-EPROTO);return;}
                if(cancel&&!cancel->done&&cancel->sent&&frame.request_id==cancel->id) {
                    std::lock_guard<std::mutex> lock(state->mutex);cancel->done=true;cancel->result.error=frame.status;
                    cancel->result.response_received=true;cancel->result.cancel_unknown=frame.outcome_unknown;
                    cancel->result.accepted=!frame.status;state->snapshot.cancel_accepted=!frame.status;
                    state->snapshot.cancel_unknown|=frame.outcome_unknown;state->cv.notify_all();
                }
            } else {finish(-EPROTO);return;}
            if(code<0){finish(code);return;}
        }
    }catch(const std::bad_alloc&){finish(-ENOMEM);}catch(...){finish(-EFAULT);}
}
}
