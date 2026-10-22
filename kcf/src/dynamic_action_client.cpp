#include "kcf/dynamic/dynamic_action_client.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include "kcf/ipc/detail/recovery.hpp"
#include "kcf/ipc/detail/storage_access.hpp"
#include <sys/random.h>
#include <algorithm>
namespace kcf {
struct DynamicActionClient::Storage {
    int fd{-1};unsigned char* base{nullptr};std::string name;struct stat identity{};
    detail::ChannelLayout layout;detail::OwnerHeader header{};
    std::int32_t owner{0};std::uint64_t ticks{0};
    ~Storage(){if(base)munmap(base,layout.length);if(fd>=0)close(fd);}
    int Validate() const {
        std::uint64_t now=0;if(!detail::ReadProcessIdentity(owner,now)||now!=ticks)return -ESTALE;
        int current=shm_open(name.c_str(),O_RDONLY|O_CLOEXEC,0);if(current<0)return -ESTALE;
        struct stat st{};int code=fstat(current,&st);close(current);
        if(code||st.st_dev!=identity.st_dev||st.st_ino!=identity.st_ino)return -ESTALE;
        detail::OwnerHeader h{};std::memcpy(&h,base,sizeof(h));
        if(h.magic!=header.magic||h.format!=header.format||h.initialized!=1||h.owner_pid!=owner||
           h.size!=header.size||h.alignment!=header.alignment||h.type_id!=header.type_id||h.layout_id!=header.layout_id||
           reinterpret_cast<detail::ChannelHeader*>(base)->depth!=1)return -ESTALE;
        return 0;
    }
    int Open(const ActionInfo& info,std::int32_t p,std::uint64_t t){
        owner=p;ticks=t;name="/";
        for(std::size_t i=1;info.name[i];++i){if(info.name[i]=='/')name+="%2F";else if(info.name[i]=='%')name+="%25";else name+=info.name[i];}
        if(name.size()>250)return -ENAMETOOLONG;
        fd=shm_open(name.c_str(),O_RDWR|O_CLOEXEC,0);if(fd<0)return -errno;
        if(flock(fd,LOCK_SH|LOCK_NB))return -errno;
        if(fstat(fd,&identity))return -errno;
        if(pread(fd,&header,sizeof(header),0)!=sizeof(header))return -EPROTO;
        if(header.magic!=detail::TOPIC_MAGIC||header.format!=detail::TOPIC_STORAGE_FORMAT||header.initialized!=1||header.owner_pid!=owner)return -ESTALE;
        if(header.size!=info.status_size||header.alignment!=info.status_alignment||header.type_id!=info.status_identity.type_id||header.layout_id!=info.status_identity.layout_id)return -EPROTOTYPE;
        std::uint32_t depth=0;if(pread(fd,&depth,sizeof(depth),offsetof(detail::ChannelHeader,depth))!=sizeof(depth)||depth!=1)return -EPROTO;
        if(!detail::ComputeChannelLayout(header.size,header.alignment,1,layout)||identity.st_size<0||static_cast<std::uint64_t>(identity.st_size)!=layout.length)return -EPROTO;
        void* mapped=mmap(nullptr,layout.length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);if(mapped==MAP_FAILED)return -errno;
        base=static_cast<unsigned char*>(mapped);int code=Validate();if(code)return code;
        return flock(fd,LOCK_UN)?-errno:0;
    }
    int Read(std::vector<std::uint8_t>& bytes){
        int code=Validate();if(code)return code;bytes.resize(header.size);
        auto& sequence=reinterpret_cast<detail::ChannelHeader*>(base)->publish_sequence;
        for(unsigned attempt=0;attempt<32;++attempt){auto wanted=sequence.load();if(!wanted)return -EAGAIN;
            for(unsigned i=0;i<3;++i){auto* slot=reinterpret_cast<detail::ChannelSlot<std::byte>*>(base+layout.slots+i*layout.stride);
                code=detail::CopyChannelSequence(sequence,1,{slot->users,slot->version,slot->sequence,base+layout.slots+i*layout.stride+layout.data},wanted,bytes.data(),bytes.size());
                if(!code)return Validate();
            }
        }return -EAGAIN;
    }
};
DynamicActionClient::DynamicActionClient()=default;
DynamicActionClient::~DynamicActionClient()=default;
int DynamicActionClient::Open(const ActionInfo& info,const TypeDescriptor& g,const TypeDescriptor& f,const TypeDescriptor& r,std::int32_t owner,std::uint64_t ticks,std::uint32_t timeout){
    if(storage_)return -EBUSY;
    if(!ValidateTypeDescriptor(g)||!ValidateTypeDescriptor(f)||!ValidateTypeDescriptor(r)||!info.ids.Valid()||!info.port||
       info.name[0]!='/'||!std::memchr(info.name,0,sizeof(info.name))||info.goal_type_id!=g.type_id||info.feedback_type_id!=f.type_id||info.result_type_id!=r.type_id||
       info.goal_request_size>SERVICE_MAX_PAYLOAD||info.result_response_size>SERVICE_MAX_PAYLOAD||info.status_size>1024*1024||
       info.goal_offset<8||info.feedback_offset<sizeof(ActionHeader)||info.result_offset<16||
       std::uint64_t(info.goal_offset)+g.payload_size>info.goal_request_size||std::uint64_t(info.feedback_offset)+f.payload_size>info.status_size||
       std::uint64_t(info.result_offset)+r.payload_size>info.result_response_size||!info.status_alignment||info.status_alignment>4096||
       (info.status_alignment&(info.status_alignment-1)))return -EINVAL;
    auto storage=std::make_unique<Storage>();int code=storage->Open(info,owner,ticks);if(code)return code;
    code=client_.Create(info.port,timeout,0);if(code)return code;
    info_=info;goal_=g;feedback_=f;result_=r;storage_=std::move(storage);return 0;
}
int DynamicActionClient::SendGoal(const DynamicPayload& goal,std::uint64_t& id,bool* unknown){
    if(unknown)*unknown=false;
    if(!storage_)return -EBADF;
    if(goal.type_id!=goal_.type_id||goal.bytes.size()!=goal_.payload_size)return -EPROTOTYPE;
    int code=storage_->Validate();if(code)return code;
    std::uint64_t seed=0;ssize_t count;do{count=getrandom(&seed,sizeof(seed),0);}while(count<0&&errno==EINTR);
    if(count!=sizeof(seed))return count<0?-errno:-EIO;
    seed=(seed&0x7fffffffffffffffULL)+1;
    std::vector<std::uint8_t> request(info_.goal_request_size);std::memcpy(request.data(),&seed,8);std::memcpy(request.data()+info_.goal_offset,goal.bytes.data(),goal.bytes.size());
    ActionGoalResponse response{};if(unknown)*unknown=true;code=client_.CallRaw(info_.ids.goal_service_id,request.data(),request.size(),&response,sizeof(response),info_.goal_request_identity,info_.goal_response_identity);
    if(code)return code;
    if(response.goal_id!=seed||response.accepted>1||response.result_code>0)return -EPROTO;
    if(unknown)*unknown=false;
    if(response.result_code)return response.result_code;
    if(!response.accepted)return -ECANCELED;
    id=seed;return 0;
}
int DynamicActionClient::Cancel(std::uint64_t id,bool* unknown){
    if(unknown)*unknown=false;
    if(!storage_)return -EBADF;
    int code=storage_->Validate();if(code)return code;
    ActionCancelResponse response{};if(unknown)*unknown=true;code=client_.CallRaw(info_.ids.cancel_service_id,&id,sizeof(id),&response,sizeof(response),info_.cancel_request_identity,info_.cancel_response_identity);
    if(code)return code;
    if(response.goal_id!=id||response.accepted>1||response.result_code>0)return -EPROTO;
    if(unknown)*unknown=false;
    return response.result_code?response.result_code:(response.accepted?0:-ECANCELED);
}
int DynamicActionClient::ReadFeedback(ActionHeader& header,DynamicPayload& feedback){
    if(!storage_)return -EBADF;
    std::vector<std::uint8_t> bytes;int code=storage_->Read(bytes);if(code)return code;
    ActionHeader h{};std::memcpy(&h,bytes.data(),sizeof(h));if(h.state>ActionState::CANCELED||h.cancel_requested>1)return -EPROTO;
    DynamicPayload value;value.type_id=feedback_.type_id;value.bytes.resize(feedback_.payload_size);std::memcpy(value.bytes.data(),bytes.data()+info_.feedback_offset,value.bytes.size());
    header=h;feedback=std::move(value);return 0;
}
int DynamicActionClient::GetResult(std::uint64_t id,ActionState& state,DynamicPayload& result){
    if(!storage_)return -EBADF;
    int code=storage_->Validate();if(code)return code;
    std::vector<std::uint8_t> bytes(info_.result_response_size);code=client_.CallRaw(info_.ids.result_service_id,&id,sizeof(id),bytes.data(),bytes.size(),info_.result_request_identity,info_.result_response_identity);
    if(code)return code;
    std::uint64_t returned=0;std::int32_t status=0;std::memcpy(&returned,bytes.data(),8);std::memcpy(&status,bytes.data()+12,4);
    if(returned!=id||status>0)return -EPROTO;
    if(status)return status;
    if(!bytes[9])return -EINPROGRESS;
    auto s=static_cast<ActionState>(bytes[8]);if(bytes[9]!=1||!IsActionTerminal(s))return -EPROTO;
    DynamicPayload value;value.type_id=result_.type_id;value.bytes.resize(result_.payload_size);std::memcpy(value.bytes.data(),bytes.data()+info_.result_offset,value.bytes.size());result=std::move(value);state=s;return 0;
}
void DynamicActionClient::Close(){client_.Close();storage_.reset();}
}
