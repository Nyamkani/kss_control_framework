#include "kcf/dynamic/dynamic_topic_reader.hpp"
#include "kcf/dynamic/dynamic_parameter_client.hpp"
#include "kcf/ipc/detail/recovery.hpp"
#include "kcf/ipc/detail/storage_access.hpp"
#include "kcf/ipc/detail/storage_identity.hpp"
#include <algorithm>
#include <memory>
#include <new>
#include <utility>

namespace kcf::detail {
namespace {
using Channel = ChannelHeader;
using Param = ParameterStorage<std::byte>;
using Slot = ChannelSlot<std::byte>;
static_assert(offsetof(Channel,type_id)==offsetof(OwnerHeader,type_id));
static_assert(offsetof(Param,layout_id)==offsetof(OwnerHeader,layout_id));
std::size_t Align(std::size_t n,std::size_t a) {return (n+a-1)&~(a-1);}
int Name(const std::string& name,std::string& encoded) {
    if(name.size()<2 || name[0]!='/' || name.size()>240 || name.find('\0')!=std::string::npos)return -EINVAL;
    encoded="/";
    for(std::size_t i=1;i<name.size();++i) {
        if(name[i]=='/')encoded+="%2F";
        else if(name[i]=='%')encoded+="%25";
        else encoded+=name[i];
    }
    return encoded.size()>250 ? -ENAMETOOLONG : 0;
}
// Linux named-object identity, local to a dynamic access instance. Keeping its
// original fd open also prevents inode reuse while that instance is alive.
class SharedObjectIdentity {
public:
    void Capture(std::string name,const struct stat& opened) {
        name_=std::move(name);device_=opened.st_dev;inode_=opened.st_ino;
    }
    int Validate() const {
        const int current=shm_open(name_.c_str(),O_RDONLY|O_CLOEXEC,0);
        if(current<0)return errno==ENOENT?-ESTALE:-errno;
        struct stat now{};
        const int result=fstat(current,&now);
        const int saved_errno=errno;
        close(current);
        if(result!=0)return -saved_errno;
        return now.st_dev==device_ && now.st_ino==inode_ ? 0 : -ESTALE;
    }
private:
    std::string name_;
    dev_t device_{};
    ino_t inode_{};
};

}
class DynamicStorage {
public:
    ~DynamicStorage(){if(base_)munmap(base_,length_);if(fd_>=0)close(fd_);}
    int Open(const std::string& name,std::uint64_t expected,const TypeDescriptor& d,bool topic) {
        if(!expected || expected!=d.type_id)return -EINVAL;
        if(!ValidateTypeDescriptor(d))return -EINVAL;
        const long page=sysconf(_SC_PAGESIZE);
        if(page<=0 || d.payload_alignment>static_cast<std::uint64_t>(page))return -EINVAL;
        // Checked arithmetic below uses 64-bit size_t for the local Linux ABI.
        static_assert(sizeof(std::size_t)>=8);
        const std::size_t a=d.payload_alignment,n=d.payload_size;
        topic_=topic;
        if(!topic) {
            data_=Align(offsetof(Param,value_slots),a);
            length_=Align(data_+2*n,std::max(a,alignof(Param)));
        }
        std::string encoded;
        int result=Name(name,encoded);if(result)return result;
        fd_=shm_open(encoded.c_str(),O_RDWR|O_CLOEXEC,0600);if(fd_<0)return -errno;
        OwnerHeader h{};
        auto count=pread(fd_,&h,sizeof(h),0);
        if(count<0)return -errno;
        if(count!=sizeof(h) || h.initialized==0)return -EAGAIN;
        if(flock(fd_,LOCK_SH|LOCK_NB)!=0)return -errno;
        struct stat st{};if(fstat(fd_,&st)!=0)return -errno;
        count=pread(fd_,&h,sizeof(h),0);
        if(count<0)return -errno;
        if(count!=sizeof(h))return -EPROTO;
        if(h.magic!=(topic?TOPIC_MAGIC:PARAMETER_MAGIC) || h.format!=(topic_?TOPIC_STORAGE_FORMAT:STORAGE_FORMAT) || h.owner_pid<=0)return -EPROTO;
        if(h.initialized!=1 || OwnerDead(h.owner_pid))return -EAGAIN;
        if(!h.type_id || h.type_id!=expected || h.layout_id!=LayoutId(d))return -EPROTOTYPE;
        if(h.size!=n || h.alignment!=a)return -EMSGSIZE;
        if(topic) {
            if(pread(fd_,&depth_,sizeof(depth_),offsetof(Channel,depth))!=sizeof(depth_))return -EPROTO;
            ChannelLayout layout;
            if(!ComputeChannelLayout(n,a,depth_,layout))return -EPROTO;
            slots_=layout.slots; data_=layout.data; stride_=layout.stride; length_=layout.length;
        }
        if(st.st_size<0 || static_cast<std::uint64_t>(st.st_size)!=length_)return -EPROTO;
        identity_.Capture(std::move(encoded),st);
        void* address=mmap(nullptr,length_,PROT_READ|PROT_WRITE,MAP_SHARED,fd_,0);
        if(address==MAP_FAILED)return -errno;
        base_=static_cast<unsigned char*>(address);header_=h;
        result=Validate();if(result)return result;
        if(flock(fd_,LOCK_UN)!=0)return -errno;
        return 0;
    }
    int Validate() const {
        const int identity_result=identity_.Validate();if(identity_result)return identity_result;
        OwnerHeader h{};std::memcpy(&h,base_,sizeof(h));
        if(h.magic!=header_.magic || h.format!=(topic_?TOPIC_STORAGE_FORMAT:STORAGE_FORMAT) || h.initialized!=1 || h.owner_pid!=header_.owner_pid)return -EPROTO;
        if(h.type_id!=header_.type_id || h.layout_id!=header_.layout_id)return -EPROTOTYPE;
        if(h.size!=header_.size || h.alignment!=header_.alignment)return -EMSGSIZE;
        if(topic_ && At<std::uint32_t>(offsetof(Channel,depth))!=depth_)return -EPROTO;
        return 0;
    }
    template<class T> T& At(std::size_t offset) const {return *reinterpret_cast<T*>(base_+offset);}
    ParameterView Parameter() const {
        return {At<pthread_mutex_t>(offsetof(Param,mutex)),At<pthread_cond_t>(offsetof(Param,condition)),
            At<std::uint64_t>(offsetof(Param,version)),At<std::uint64_t>(offsetof(Param,previous_version)),
            At<std::uint32_t>(offsetof(Param,active_index)),At<std::uint32_t>(offsetof(Param,previous_active)),
            At<std::atomic<std::uint32_t>>(offsetof(Param,transaction_active)),base_+data_,static_cast<std::size_t>(header_.size)};
    }
    int Read(DynamicPayload& output) {
        int result=Validate();if(result)return result;
        DynamicPayload value;value.type_id=header_.type_id;value.bytes.resize(header_.size);
        if(topic_) {
            auto& seq=At<std::atomic<std::uint64_t>>(offsetof(Channel,publish_sequence));
            result=-EAGAIN;
            for(int attempt=0;attempt<32 && result;++attempt) {
                const auto wanted=seq.load(); if(!wanted)return -EAGAIN;
                const auto entry=static_cast<std::size_t>((wanted-1)%depth_)*3;
                for(unsigned i=0;i<3;++i) {
                    const auto slot=slots_+(entry+i)*stride_;
                    result=CopyChannelSequence(seq,depth_,
                        {At<std::atomic<std::uint32_t>>(slot+offsetof(Slot,users)),
                         At<std::atomic<std::uint32_t>>(slot+offsetof(Slot,version)),
                         At<std::uint64_t>(slot+offsetof(Slot,sequence)),base_+slot+data_},
                        wanted,value.bytes.data(),header_.size);
                    if(!result){value.sequence=wanted;break;}
                }
            }
        } else result=GetParameter(Parameter(),value.bytes.data(),&value.sequence);
        if(result)return result;
        result=Validate();if(result)return result;
        output=std::move(value);return 0;
    }
    int Set(const DynamicPayload& value) {
        const int result=Validate();if(result)return result;
        if(value.type_id!=header_.type_id)return -EPROTOTYPE;
        if(value.bytes.size()!=header_.size)return -EMSGSIZE;
        return SetParameter(Parameter(),value.bytes.data());
    }
private:
    SharedObjectIdentity identity_;
    int fd_{-1};unsigned char* base_{nullptr};OwnerHeader header_{};
    std::uint32_t depth_{0};
    std::size_t length_{0},slots_{0},data_{0},stride_{0};bool topic_{false};
};
}
namespace kcf {
namespace {
int OpenStorage(std::unique_ptr<detail::DynamicStorage>& storage,const std::string& name,
                std::uint64_t id,const TypeDescriptor& d,bool topic) {
    if(storage)return -EBUSY;
    try {
        auto candidate=std::make_unique<detail::DynamicStorage>();
        const int result=candidate->Open(name,id,d,topic);
        if(result)return result;
        storage=std::move(candidate);return 0;
    }catch(const std::bad_alloc&){return -ENOMEM;}catch(...){return -EIO;}
}
int ReadStorage(detail::DynamicStorage* storage,DynamicPayload& output) {
    if(!storage)return -EBADF;
    try{return storage->Read(output);}catch(const std::bad_alloc&){return -ENOMEM;}catch(...){return -EIO;}
}
}
DynamicTopicReader::DynamicTopicReader()=default;
DynamicTopicReader::~DynamicTopicReader()=default;
int DynamicTopicReader::Open(const std::string& name,const TypeDescriptor& d){return Open(name,d.type_id,d);}
int DynamicTopicReader::Open(const std::string& name,std::uint64_t id,const TypeDescriptor& d){return OpenStorage(storage_,name,id,d,true);}
int DynamicTopicReader::ReadLatest(DynamicPayload& p){return ReadStorage(storage_.get(),p);}
void DynamicTopicReader::Close() noexcept {storage_.reset();}
DynamicParameterClient::DynamicParameterClient()=default;
DynamicParameterClient::~DynamicParameterClient()=default;
int DynamicParameterClient::Open(const std::string& name,const TypeDescriptor& d){return Open(name,d.type_id,d);}
int DynamicParameterClient::Open(const std::string& name,std::uint64_t id,const TypeDescriptor& d){return OpenStorage(storage_,name,id,d,false);}
int DynamicParameterClient::Get(DynamicPayload& p){return ReadStorage(storage_.get(),p);}
int DynamicParameterClient::Set(const DynamicPayload& p){return storage_?storage_->Set(p):-EBADF;}
void DynamicParameterClient::Close() noexcept {storage_.reset();}
}
