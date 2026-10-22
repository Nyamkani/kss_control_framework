#include "kcf/network/detail/topic_proxy.hpp"
#include "kcf/ipc/detail/recovery.hpp"
#include "kcf/ipc/detail/storage_identity.hpp"
#include <new>
#include <cstring>
#include "kcf/network/parameter_codec.hpp"
#include "kcf/network/validation.hpp"
#include <sstream>
#include <cstdio>
#include <limits>
namespace kcf::network::detail {
namespace d=kcf::detail;
namespace {
struct FD {int value{-1};explicit FD(int n=-1):value(n){}~FD(){if(value>=0)close(value);}operator int()const{return value;}};
std::uint64_t Hash(const std::uint8_t* p,std::size_t n){std::uint64_t h=14695981039346656037ull;while(n--){h^=*p++;h*=1099511628211ull;}return h;}
struct Writer {std::vector<std::uint8_t> b;void U(std::uint64_t v,unsigned n){while(n)b.push_back(v>>(8*--n));}
    void Endpoint(const RemoteEndpointIdentity& e){for(auto c:e.runtime.host.bytes)U(c,1);for(auto c:e.runtime.boot_id.bytes)U(c,1);U(e.runtime.pid,4);U(e.runtime.process_start_ticks,8);U(e.registration_id,8);}};
struct Reader {const std::vector<std::uint8_t>& b;std::size_t at{0};bool ok{true};std::uint64_t U(unsigned n){if(n>b.size()-at){ok=false;return 0;}std::uint64_t v=0;while(n--)v=v*256+b[at++];return v;}
    RemoteEndpointIdentity Endpoint(){RemoteEndpointIdentity e;for(auto& c:e.runtime.host.bytes)c=U(1);for(auto& c:e.runtime.boot_id.bytes)c=U(1);auto pid=U(4);if(pid>INT32_MAX)ok=false;e.runtime.pid=pid;e.runtime.process_start_ticks=U(8);e.registration_id=U(8);return e;}};
bool ValidRecord(const ProxyRecord& r){return Valid(r.owner.proxy)&&Valid(r.owner.source)&&r.owner.proxy.runtime.host!=r.owner.source.runtime.host&&
    (r.owner.proxy.registration_id&(1ull<<61))&&r.owner.gateway_session&&r.owner.source_session&&Text(r.topic,240)&&r.topic.size()>1&&r.topic[0]=='/'&&Valid(r.type)&&r.size&&r.size<=16384&&r.alignment&&r.alignment<=4096&&!(r.alignment&(r.alignment-1))&&r.layout&&r.inode&&r.length;}
int ReadRecord(const std::string& name,ProxyRecord& r){FD fd(shm_open(name.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW,0));if(fd<0)return -errno;struct stat st{};
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||st.st_nlink!=1||(st.st_mode&077)||st.st_size<0||st.st_size>static_cast<off_t>(PROXY_RECORD_MAX))return -EPROTO;
    std::vector<std::uint8_t> bytes(st.st_size);if(pread(fd,bytes.data(),bytes.size(),0)!=static_cast<ssize_t>(bytes.size()))return -EIO;return DecodeProxyRecord(bytes,r);}
int LockDirectory(){int fd=open("/dev/shm",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(fd<0)return -errno;if(flock(fd,LOCK_EX|LOCK_NB)){int e=errno;close(fd);return -e;}return fd;}
bool NamedObject(const std::string& name,const struct stat& expected){FD fd(shm_open(name.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW,0));struct stat st{};return fd>=0&&!fstat(fd,&st)&&st.st_dev==expected.st_dev&&st.st_ino==expected.st_ino;}
std::string EncodeName(const std::string& name){std::string encoded="/";for(std::size_t i=1;i<name.size();++i){if(name[i]=='/')encoded+="%2F";else if(name[i]=='%')encoded+="%25";else encoded+=name[i];}return encoded;}
}
std::string ProxySidecarName(const std::string& topic){char name[64];std::snprintf(name,sizeof(name),"/kcf_network_proxy_%016llx",static_cast<unsigned long long>(Hash(reinterpret_cast<const std::uint8_t*>(topic.data()),topic.size())));return name;}
int EncodeProxyRecord(const ProxyRecord& r,std::vector<std::uint8_t>& out){if(!ValidRecord(r))return -EINVAL;Writer w;w.U(0x4b43464e50525859ull,8);w.U(1,2);w.U(0,2);w.U(r.topic.size(),2);
    w.Endpoint(r.owner.proxy);w.Endpoint(r.owner.source);w.U(r.owner.gateway_session,8);w.U(r.owner.source_session,8);w.U(r.type.type_id,8);w.U(r.type.schema_id,8);w.U(r.type.encoding_id,4);w.U(r.type.payload_size,4);
    for(auto v:{r.size,r.alignment,r.layout,r.device,r.inode,r.length})w.U(v,8);
    for(char c:r.topic)w.U(static_cast<unsigned char>(c),1);
    auto hash=Hash(w.b.data(),w.b.size());w.U(hash,8);
    if(w.b.size()>PROXY_RECORD_MAX)return -E2BIG;
    out=std::move(w.b);return 0;}
int DecodeProxyRecord(const std::vector<std::uint8_t>& bytes,ProxyRecord& out){if(bytes.size()<214||bytes.size()>PROXY_RECORD_MAX)return -EPROTO;Reader rd{bytes};if(rd.U(8)!=0x4b43464e50525859ull||rd.U(2)!=1||rd.U(2))return -EPROTO;auto n=rd.U(2);if(n>240)return -EPROTO;
    ProxyRecord r;r.owner.proxy=rd.Endpoint();r.owner.source=rd.Endpoint();r.owner.gateway_session=rd.U(8);r.owner.source_session=rd.U(8);r.type.type_id=rd.U(8);r.type.schema_id=rd.U(8);r.type.encoding_id=rd.U(4);r.type.payload_size=rd.U(4);
    r.size=rd.U(8);r.alignment=rd.U(8);r.layout=rd.U(8);r.device=rd.U(8);r.inode=rd.U(8);r.length=rd.U(8);
    if(!rd.ok||rd.at+n+8!=bytes.size())return -EPROTO;
    for(unsigned i=0;i<n;++i)r.topic+=static_cast<char>(rd.U(1));
    auto hash=rd.U(8);if(!rd.ok||hash!=Hash(bytes.data(),bytes.size()-8)||!ValidRecord(r))return -EPROTO;out=std::move(r);return 0;}
OwnerLiveness ProbeProxyOwner(std::int32_t pid,std::uint64_t ticks){
    if(pid<=0||!ticks)return OwnerLiveness::UNKNOWN;
    auto path="/proc/"+std::to_string(pid)+"/stat";FD fd(open(path.c_str(),O_RDONLY|O_CLOEXEC));
    if(fd<0){int e=errno;if((e==ENOENT||e==ESRCH)&&kill(pid,0)<0&&errno==ESRCH)return OwnerLiveness::DEAD;return OwnerLiveness::UNKNOWN;}
    char bytes[4096];auto count=read(fd,bytes,sizeof(bytes));if(count<=0||count==sizeof(bytes))return OwnerLiveness::UNKNOWN;
    std::string stat(bytes,count);auto end=stat.rfind(')');if(end==std::string::npos)return OwnerLiveness::UNKNOWN;std::istringstream input(stat.substr(end+1));std::string field;std::uint64_t observed=0;
    for(int i=3;i<=22;++i){if(!(input>>field))return OwnerLiveness::UNKNOWN;if(i==22){try{std::size_t used=0;observed=std::stoull(field,&used);if(used!=field.size()||!observed)return OwnerLiveness::UNKNOWN;}catch(...){return OwnerLiveness::UNKNOWN;}}}
    return observed==ticks?OwnerLiveness::LIVE:OwnerLiveness::DEAD;
}
struct TopicProxy::Impl {
    int fd{-1};d::ChannelHeader* header{nullptr};d::ChannelLayout layout;TypeDescriptor descriptor;std::string name,sidecar;ProxyRecord record;struct stat identity{};
    ~Impl(){if(header)munmap(header,layout.length);if(fd>=0)close(fd);}
    unsigned char* Base(){return reinterpret_cast<unsigned char*>(header);}
    d::ChannelSlot<std::byte>& Slot(unsigned i){return *reinterpret_cast<d::ChannelSlot<std::byte>*>(Base()+layout.slots+i*layout.stride);}
    int InitNotify(){
        pthread_mutexattr_t ma;int code=pthread_mutexattr_init(&ma);if(code)return -code;
        code=pthread_mutexattr_setpshared(&ma,PTHREAD_PROCESS_SHARED);if(!code)code=pthread_mutexattr_setrobust(&ma,PTHREAD_MUTEX_ROBUST);if(!code)code=pthread_mutex_init(&header->notify_mutex,&ma);pthread_mutexattr_destroy(&ma);if(code)return -code;
        pthread_condattr_t ca;code=pthread_condattr_init(&ca);if(code)return -code;
        code=pthread_condattr_setpshared(&ca,PTHREAD_PROCESS_SHARED);if(!code)code=pthread_condattr_setclock(&ca,CLOCK_MONOTONIC);if(!code)code=pthread_cond_init(&header->notify_cond,&ca);pthread_condattr_destroy(&ca);return -code;
    }
    int Validate(){int current=shm_open(name.c_str(),O_RDONLY|O_CLOEXEC,0);if(current<0)return -ESTALE;struct stat st{};int code=fstat(current,&st);close(current);return code||st.st_dev!=identity.st_dev||st.st_ino!=identity.st_ino?-ESTALE:0;}
};
TopicProxy::TopicProxy()=default;TopicProxy::~TopicProxy(){Cleanup();}
int TopicProxy::Cleanup(){
    if(!impl_)return 0;
    auto& s=*impl_;int lock=LockDirectory();if(lock<0){state_=ProxyRecoveryState::CLEANUP_FAILED;return lock;}FD directory(lock);
    ProxyRecord r;int code=ReadRecord(s.sidecar,r);
    if(code||!(r.owner.proxy==s.record.owner.proxy)||r.inode!=s.identity.st_ino||r.device!=s.identity.st_dev||r.topic!=s.record.topic||!(r.type==s.record.type)||r.size!=s.record.size||r.alignment!=s.record.alignment||r.layout!=s.record.layout||r.length!=s.record.length||
       !(r.owner.source==s.record.owner.source)||r.owner.gateway_session!=s.record.owner.gateway_session||r.owner.source_session!=s.record.owner.source_session||!NamedObject(s.name,s.identity)){
        state_=ProxyRecoveryState::VALIDATION_FAILED;return -ESTALE;
    }
    d::ChannelHeader header{};const auto prefix=offsetof(d::ChannelHeader,publish_sequence);
    if(s.record.owner.proxy.runtime.pid!=getpid()||ProbeProxyOwner(getpid(),s.record.owner.proxy.runtime.process_start_ticks)!=OwnerLiveness::LIVE||
       pread(s.fd,&header,prefix,0)!=static_cast<ssize_t>(prefix)||header.magic!=d::TOPIC_MAGIC||header.format!=4||header.initialized!=1||header.depth!=1||header.owner_pid!=getpid()||
       header.size!=s.record.size||header.alignment!=s.record.alignment||header.type_id!=s.record.type.type_id||header.layout_id!=s.record.layout){state_=ProxyRecoveryState::VALIDATION_FAILED;return -ESTALE;}
    // Topic first: a crash leaves a verifiable orphan sidecar, never an
    // unmarked old Topic. Only this process's proven owned incarnation is removed.
    if(shm_unlink(s.name.c_str())||shm_unlink(s.sidecar.c_str())){state_=ProxyRecoveryState::CLEANUP_FAILED;return -errno;}
    impl_.reset();return 0;
}
int TopicProxy::Create(const std::string& name,const TypeDescriptor& descriptor,const ProxyOwnership& owner){
    if(impl_)return -EBUSY;
    state_=ProxyRecoveryState::NONE;stale_=false;
    if(!ValidateTypeDescriptor(descriptor)||descriptor.payload_size>16384||descriptor.payload_alignment>4096||name.size()<2||name.size()>240||name[0]!='/'||name.find('\0')!=std::string::npos)return -EINVAL;
    if(!Valid(owner.proxy)||!Valid(owner.source)||owner.proxy.runtime.host==owner.source.runtime.host||!(owner.proxy.registration_id&(1ull<<61))||!owner.gateway_session||!owner.source_session)return -EINVAL;
    auto s=std::make_unique<Impl>();s->descriptor=descriptor;s->name=EncodeName(name);s->sidecar=ProxySidecarName(name);
    if(s->name.size()>250)return -ENAMETOOLONG;
    if(!d::ComputeChannelLayout(descriptor.payload_size,descriptor.payload_alignment,1,s->layout))return -EINVAL;
    s->record.owner=owner;s->record.topic=name;s->record.size=descriptor.payload_size;s->record.alignment=descriptor.payload_alignment;s->record.layout=d::LayoutId(descriptor);s->record.length=s->layout.length;
    if(ParameterWireType(descriptor,s->record.type)||owner.proxy.runtime.pid!=getpid()||ProbeProxyOwner(owner.proxy.runtime.pid,owner.proxy.runtime.process_start_ticks)!=OwnerLiveness::LIVE)return -EINVAL;
    auto reject=[&](ProxyRecoveryState state,int code=-EEXIST){state_=state;return code;};
    int lock=LockDirectory();if(lock<0)return lock;FD directory(lock);
    s->fd=shm_open(s->name.c_str(),O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);
    if(s->fd<0){
        if(errno!=EEXIST)return -errno;
        FD old(shm_open(s->name.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW,0));if(old<0||flock(old,LOCK_EX|LOCK_NB))return reject(ProxyRecoveryState::VALIDATION_FAILED);
        ProxyRecord record;int code=ReadRecord(s->sidecar,record);if(code)return reject(code==-ENOENT?ProxyRecoveryState::UNKNOWN_OWNERSHIP:ProxyRecoveryState::VALIDATION_FAILED);
        struct stat st{};d::ChannelHeader header{};const auto prefix=offsetof(d::ChannelHeader,publish_sequence);
        if(fstat(old,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||st.st_nlink!=1||st.st_size!=static_cast<off_t>(s->layout.length)||
           pread(old,&header,prefix,0)!=static_cast<ssize_t>(prefix)||record.topic!=name||record.owner.proxy.runtime.host!=owner.proxy.runtime.host||record.owner.proxy.runtime.boot_id!=owner.proxy.runtime.boot_id||
           !(record.type==s->record.type)||record.size!=s->record.size||record.alignment!=s->record.alignment||record.layout!=s->record.layout||record.length!=s->record.length||
           record.device!=static_cast<std::uint64_t>(st.st_dev)||record.inode!=st.st_ino||header.magic!=d::TOPIC_MAGIC||header.format!=4||header.initialized!=1||header.depth!=1||
           header.owner_pid!=record.owner.proxy.runtime.pid||header.type_id!=descriptor.type_id||header.layout_id!=s->record.layout||header.size!=descriptor.payload_size||header.alignment!=descriptor.payload_alignment||header.reserved||header.reserved1)
            return reject(ProxyRecoveryState::VALIDATION_FAILED);
        auto live=ProbeProxyOwner(record.owner.proxy.runtime.pid,record.owner.proxy.runtime.process_start_ticks);
        if(live==OwnerLiveness::LIVE)return reject(ProxyRecoveryState::LIVE_NETWORK_OWNER);
        if(live!=OwnerLiveness::DEAD)return reject(ProxyRecoveryState::VALIDATION_FAILED);
        stale_=true;state_=ProxyRecoveryState::STALE_DETECTED;
        if(!NamedObject(s->name,st))return reject(ProxyRecoveryState::VALIDATION_FAILED);
        if(shm_unlink(s->name.c_str())||shm_unlink(s->sidecar.c_str()))return reject(ProxyRecoveryState::CLEANUP_FAILED,-errno);
        s->fd=shm_open(s->name.c_str(),O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600);if(s->fd<0)return reject(ProxyRecoveryState::CLEANUP_FAILED,-errno);
    }
    if(fstat(s->fd,&s->identity))return reject(ProxyRecoveryState::CLEANUP_FAILED,-errno);
    auto fail=[&](ProxyRecoveryState state,int code){if(NamedObject(s->name,s->identity))shm_unlink(s->name.c_str());return reject(state,code);};
    // Cleanup crash window: Topic was already removed but its sidecar survived.
    ProxyRecord orphan;int prior=ReadRecord(s->sidecar,orphan);
    if(!prior){
        if(orphan.topic!=name||orphan.owner.proxy.runtime.host!=owner.proxy.runtime.host||orphan.owner.proxy.runtime.boot_id!=owner.proxy.runtime.boot_id||!(orphan.type==s->record.type)||
           ProbeProxyOwner(orphan.owner.proxy.runtime.pid,orphan.owner.proxy.runtime.process_start_ticks)!=OwnerLiveness::DEAD)
            return fail(ProxyRecoveryState::VALIDATION_FAILED,-EEXIST);
        if(shm_unlink(s->sidecar.c_str()))return fail(ProxyRecoveryState::CLEANUP_FAILED,-errno);
        stale_=true;
    }else if(prior!=-ENOENT)return fail(ProxyRecoveryState::VALIDATION_FAILED,-EEXIST);
    if(flock(s->fd,LOCK_EX|LOCK_NB)||ftruncate(s->fd,s->layout.length))return fail(ProxyRecoveryState::CLEANUP_FAILED,-errno);
    auto* mapped=mmap(nullptr,s->layout.length,PROT_READ|PROT_WRITE,MAP_SHARED,s->fd,0);if(mapped==MAP_FAILED)return fail(ProxyRecoveryState::CLEANUP_FAILED,-errno);
    s->header=new(mapped)d::ChannelHeader;s->header->size=descriptor.payload_size;s->header->alignment=descriptor.payload_alignment;s->header->owner_pid=getpid();s->header->type_id=descriptor.type_id;s->header->layout_id=s->record.layout;
    for(unsigned i=0;i<3;++i)new(&s->Slot(i))d::ChannelSlot<std::byte>;
    int code=s->InitNotify();if(code)return fail(ProxyRecoveryState::CLEANUP_FAILED,code);
    s->header->magic=d::TOPIC_MAGIC;s->header->initialized=1;s->record.device=s->identity.st_dev;s->record.inode=s->identity.st_ino;
    std::vector<std::uint8_t> bytes;code=EncodeProxyRecord(s->record,bytes);if(code)return fail(ProxyRecoveryState::VALIDATION_FAILED,code);
    FD sidecar(shm_open(s->sidecar.c_str(),O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC,0600));if(sidecar<0)return fail(ProxyRecoveryState::CLEANUP_FAILED,-errno);
    if(pwrite(sidecar,bytes.data(),bytes.size(),0)!=static_cast<ssize_t>(bytes.size())){code=-errno;shm_unlink(s->sidecar.c_str());return fail(ProxyRecoveryState::CLEANUP_FAILED,code?code:-EIO);}
    flock(s->fd,LOCK_UN);impl_=std::move(s);state_=stale_?ProxyRecoveryState::RECOVERED:ProxyRecoveryState::CREATED;return 0;
}
int TopicProxy::Publish(const DynamicPayload& value){
    if(!impl_)return -EBADF;
    auto& s=*impl_;if(value.type_id!=s.descriptor.type_id||value.bytes.size()!=s.descriptor.payload_size)return -EPROTOTYPE;
    int code=s.Validate();if(code)return code;
    auto& h=*s.header;code=pthread_mutex_trylock(&h.notify_mutex);
    if(code==EOWNERDEAD){h.notify_sequence=h.publish_sequence.load();code=pthread_mutex_consistent(&h.notify_mutex);if(code)pthread_mutex_unlock(&h.notify_mutex);}
    if(code)return code==EBUSY?-EAGAIN:-code;
    const auto previous=h.publish_sequence.load();if(previous==UINT64_MAX){pthread_mutex_unlock(&h.notify_mutex);return -EOVERFLOW;}
    for(unsigned i=0;i<3;++i){auto& slot=s.Slot(i);std::uint32_t free=0;if(!slot.users.compare_exchange_strong(free,0x80000000u))continue;
        if(previous&&slot.sequence==previous){slot.users.store(0);continue;}
        code=pthread_cond_broadcast(&h.notify_cond);if(code){slot.users.store(0);pthread_mutex_unlock(&h.notify_mutex);return -code;}
        slot.version.fetch_add(1);std::memcpy(s.Base()+s.layout.slots+i*s.layout.stride+s.layout.data,value.bytes.data(),value.bytes.size());slot.sequence=previous+1;slot.version.fetch_add(1);slot.users.store(0);
        h.publish_sequence.store(previous+1);h.notify_sequence=previous+1;pthread_mutex_unlock(&h.notify_mutex);return 0;
    }
    pthread_mutex_unlock(&h.notify_mutex);return -EAGAIN;
}
}
