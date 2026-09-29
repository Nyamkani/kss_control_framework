#include "kcf/network/control_frame.hpp"
#include <cerrno>
#include <limits>
namespace kcf::network {
namespace {
struct Writer {
    std::vector<std::uint8_t> b;
    void U(std::uint64_t v,unsigned n){for(unsigned i=n;i;--i)b.push_back(v>>((i-1)*8));}
    void Host(const HostIdentity& h){for(auto v:h.bytes)U(v,1);}
    void Runtime(const RemoteRuntimeIdentity& r){Host(r.host);Host(r.boot_id);U(r.pid,4);U(r.process_start_ticks,8);}
};
struct Reader {
    const std::uint8_t* p;std::size_t left;bool ok{true};
    std::uint64_t U(unsigned n){if(left<n){ok=false;return 0;}std::uint64_t v=0;while(n--){v=(v<<8)|*p++;--left;}return v;}
    HostIdentity Host(){HostIdentity h;for(auto& b:h.bytes)b=U(1);return h;}
    RemoteRuntimeIdentity Runtime(){RemoteRuntimeIdentity r;r.host=Host();r.boot_id=Host();auto pid=U(4);if(pid>INT32_MAX)ok=false;r.pid=pid;r.process_start_ticks=U(8);return r;}
};
bool ValidFrame(const ControlFrame& f) {
    if(!Valid(f.source)||!f.session_id||f.payload.size()>MAX_PARAMETER_WIRE)return false;
    const bool action=IsActionMessage(f.operation);
    const bool service=f.operation==ControlMessage::SERVICE_REQUEST||f.operation==ControlMessage::SERVICE_RESPONSE;
    if((service||action)?(!Valid(f.response_type)||f.response_type.payload_size>MAX_PARAMETER_WIRE):!(f.response_type==TypeIdentity{}))return false;
    if(!action&&(!(f.feedback_type==TypeIdentity{})||f.goal!=RemoteGoalIdentity{}||f.action_state!=ActionState::IDLE||f.feedback_sequence||f.outcome_unknown))return false;
    if(f.operation==ControlMessage::HELLO)return !f.request_id && f.endpoint==RemoteEndpointIdentity{} && f.name.empty() &&
        f.type==TypeIdentity{} && !f.timeout_ms && !f.status && f.payload.empty() && f.target==TargetScope::ENDPOINT && f.reason==CompatibilityReason::COMPATIBLE;
    if((!f.request_id&&f.operation!=ControlMessage::ACTION_FEEDBACK&&f.operation!=ControlMessage::ACTION_RESULT)||!Valid(f.endpoint)||!Text(f.name,240)||f.name[0]!='/'||!Valid(f.type)||f.type.payload_size>MAX_PARAMETER_WIRE||!TargetCapability(f.target))return false;
    if(action){
        if(!Valid(f.goal.source)||!f.goal.goal_id||!f.goal.session_id||!Valid(f.feedback_type)||f.feedback_type.payload_size>MAX_PARAMETER_WIRE||f.action_state>ActionState::CANCELED)return false;
        if(f.outcome_unknown&&(!f.status||(f.operation!=ControlMessage::ACTION_GOAL_RESPONSE&&f.operation!=ControlMessage::ACTION_CANCEL_RESPONSE)))return false;
        const bool request=f.operation==ControlMessage::ACTION_GOAL_REQUEST||f.operation==ControlMessage::ACTION_CANCEL_REQUEST;
        if(request)return f.goal.source==f.source&&f.goal.session_id==f.session_id&&!f.status&&f.reason==CompatibilityReason::COMPATIBLE&&f.timeout_ms>0&&f.timeout_ms<=60000&&
            f.action_state==ActionState::IDLE&&!f.feedback_sequence&&
            (f.operation==ControlMessage::ACTION_GOAL_REQUEST?f.payload.size()==f.type.payload_size:f.payload.empty());
        if(f.timeout_ms||f.status>0||static_cast<unsigned>(f.reason)>static_cast<unsigned>(CompatibilityReason::UNKNOWN_ENDPOINT))return false;
        if(f.operation==ControlMessage::ACTION_FEEDBACK)return !f.request_id&&!f.status&&f.reason==CompatibilityReason::COMPATIBLE&&f.feedback_sequence&&
            f.action_state==ActionState::RUNNING&&f.payload.size()==f.feedback_type.payload_size;
        if(f.operation==ControlMessage::ACTION_RESULT)return !f.request_id&&IsActionTerminal(f.action_state)&&
            (f.status?f.payload.empty():f.payload.size()==f.response_type.payload_size);
        return f.payload.empty()&&!f.feedback_sequence&&f.action_state==(f.status?ActionState::IDLE:ActionState::ACCEPTED);
    }
    if(!(f.feedback_type==TypeIdentity{})||f.goal!=RemoteGoalIdentity{}||f.action_state!=ActionState::IDLE||f.feedback_sequence)return false;
    const bool request=f.operation==ControlMessage::PARAM_GET_REQUEST||f.operation==ControlMessage::PARAM_SET_REQUEST||f.operation==ControlMessage::SERVICE_REQUEST;
    const bool response=f.operation==ControlMessage::PARAM_GET_RESPONSE||f.operation==ControlMessage::PARAM_SET_RESPONSE||f.operation==ControlMessage::SERVICE_RESPONSE;
    if(!request&&!response)return false;
    if(request) return !f.status && f.reason==CompatibilityReason::COMPATIBLE && f.timeout_ms>0 && f.timeout_ms<=60000 &&
        (f.operation==ControlMessage::PARAM_GET_REQUEST?f.payload.empty():f.payload.size()==f.type.payload_size);
    if(f.timeout_ms || f.status>0 || static_cast<unsigned>(f.reason)>static_cast<unsigned>(CompatibilityReason::UNKNOWN_ENDPOINT))return false;
    if(f.status)return f.payload.empty();
    return f.reason==CompatibilityReason::COMPATIBLE && (f.operation==ControlMessage::SERVICE_RESPONSE?f.payload.size()==f.response_type.payload_size:(f.operation==ControlMessage::PARAM_GET_RESPONSE?f.payload.size()==f.type.payload_size:f.payload.empty()));
}
}
int ControlFrameSize(const std::uint8_t* p,std::size_t size,std::size_t& output) {
    if(!p)return -EINVAL;
    if(size<CONTROL_PREFIX_SIZE)return -EAGAIN;
    Reader r{p,size};if(r.U(4)!=CONTROL_MAGIC)return -EPROTO;if(r.U(2)!=CONTROL_WIRE_VERSION)return -EPROTONOSUPPORT;
    const auto op=r.U(2);if(op<1||op>13)return -EPROTO;
    auto length=r.U(4);if(length<172||length>MAX_CONTROL_FRAME)return -EMSGSIZE;
    if(r.U(4))return -EPROTO;
    output=length;return 0;
}
int EncodeControlFrame(const ControlFrame& f,std::vector<std::uint8_t>& output) {
    if(!ValidFrame(f))return -EINVAL;
    Writer w;w.U(CONTROL_MAGIC,4);w.U(CONTROL_WIRE_VERSION,2);w.U(static_cast<unsigned>(f.operation),2);
    const bool action=IsActionMessage(f.operation);
    const bool service=f.operation==ControlMessage::SERVICE_REQUEST||f.operation==ControlMessage::SERVICE_RESPONSE;
    w.U(172+(service?24:0)+(action?120:0)+f.name.size()+f.payload.size(),4);w.U(0,4);
    w.Runtime(f.source);w.U(f.session_id,8);w.U(f.request_id,8);w.Runtime(f.endpoint.runtime);w.U(f.endpoint.registration_id,8);
    w.U(static_cast<unsigned>(f.target),1);w.U(static_cast<unsigned>(f.reason),1);w.U(0,2);
    w.U(f.type.type_id,8);w.U(f.type.schema_id,8);w.U(f.type.encoding_id,4);w.U(f.type.payload_size,4);
    w.U(f.timeout_ms,4);w.U(static_cast<std::uint32_t>(f.status),4);w.U(f.name.size(),2);w.U(0,2);w.U(f.payload.size(),4);
    if(service||action){w.U(f.response_type.type_id,8);w.U(f.response_type.schema_id,8);w.U(f.response_type.encoding_id,4);w.U(f.response_type.payload_size,4);}
    if(action){w.U(f.feedback_type.type_id,8);w.U(f.feedback_type.schema_id,8);w.U(f.feedback_type.encoding_id,4);w.U(f.feedback_type.payload_size,4);
        w.Runtime(f.goal.source);w.U(f.goal.goal_id,8);w.U(f.goal.session_id,8);w.U(static_cast<unsigned>(f.action_state),1);w.U(f.outcome_unknown?1:0,1);w.U(0,2);w.U(f.feedback_sequence,8);}
    w.b.insert(w.b.end(),f.name.begin(),f.name.end());w.b.insert(w.b.end(),f.payload.begin(),f.payload.end());
    output=std::move(w.b);return 0;
}
int DecodeControlFrame(const std::uint8_t* p,std::size_t size,ControlFrame& output) {
    std::size_t length=0;int code=ControlFrameSize(p,size,length);if(code)return code;if(length!=size)return -EMSGSIZE;
    Reader r{p+6,size-6};ControlFrame f;f.operation=static_cast<ControlMessage>(r.U(2));r.U(8);
    f.source=r.Runtime();f.session_id=r.U(8);f.request_id=r.U(8);f.endpoint.runtime=r.Runtime();f.endpoint.registration_id=r.U(8);
    f.target=static_cast<TargetScope>(r.U(1));f.reason=static_cast<CompatibilityReason>(r.U(1));if(r.U(2))return -EPROTO;
    f.type.type_id=r.U(8);f.type.schema_id=r.U(8);f.type.encoding_id=r.U(4);f.type.payload_size=r.U(4);
    f.timeout_ms=r.U(4);const auto status=r.U(4);f.status=status<=INT32_MAX?static_cast<std::int32_t>(status):-1-static_cast<std::int32_t>(UINT32_MAX-status);
    const auto name_size=r.U(2);if(r.U(2))return -EPROTO;const auto payload_size=r.U(4);
    if(IsActionMessage(f.operation)||f.operation==ControlMessage::SERVICE_REQUEST||f.operation==ControlMessage::SERVICE_RESPONSE){f.response_type.type_id=r.U(8);f.response_type.schema_id=r.U(8);f.response_type.encoding_id=r.U(4);f.response_type.payload_size=r.U(4);}
    if(IsActionMessage(f.operation)){f.feedback_type.type_id=r.U(8);f.feedback_type.schema_id=r.U(8);f.feedback_type.encoding_id=r.U(4);f.feedback_type.payload_size=r.U(4);
        f.goal.source=r.Runtime();f.goal.goal_id=r.U(8);f.goal.session_id=r.U(8);f.action_state=static_cast<ActionState>(r.U(1));const auto unknown=r.U(1);if(unknown>1||r.U(2))return -EPROTO;f.outcome_unknown=unknown!=0;f.feedback_sequence=r.U(8);}
    if(!r.ok||name_size>240||payload_size>MAX_PARAMETER_WIRE||name_size+payload_size!=r.left)return -EPROTO;
    f.name.assign(reinterpret_cast<const char*>(r.p),name_size);r.p+=name_size;
    f.payload.assign(r.p,r.p+payload_size);
    if(!ValidFrame(f))return -EPROTO;
    output=std::move(f);return 0;
}
}
