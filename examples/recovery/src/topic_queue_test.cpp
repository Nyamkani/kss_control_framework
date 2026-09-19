#include "recovery/test_support.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/ipc/subscriber.hpp"
#include "kcf/dynamic/dynamic_topic_reader.hpp"
#include <array>
#include <limits>
#include <future>
using namespace recovery_test;
using namespace std::chrono_literals;
struct Message { std::uint64_t measurement_sequence; bool valid; unsigned char bytes[8192]; };
namespace kcf {
template<> struct TypeDescriptorTraits<Message> {
    static constexpr bool defined=true;
    static TypeDescriptor Get() noexcept {
        auto d=MakeTypeDescriptor<Message>("kcf.test.Queue.v1");d.field_count=3;
        d.fields[0]=MakeField<Message,std::uint64_t>("measurement_sequence",offsetof(Message,measurement_sequence));
        d.fields[1]=MakeField<Message,bool>("valid",offsetof(Message,valid));
        d.fields[2]=MakeField<Message,unsigned char[8192]>("bytes",offsetof(Message,bytes));return d;
    }
};
}
std::atomic<bool> pause_copy{false},entered{false},release_copy{false},fail_notify{false};
int notice=-1;
extern "C" void* __real_memcpy(void*,const void*,std::size_t);
extern "C" void* __wrap_memcpy(void* dst,const void* src,std::size_t size) {
    if(size==sizeof(Message) && pause_copy.exchange(false)) {
        if(notice>=0){assert(write(notice,"x",1)==1);for(;;)pause();}
        entered=true;while(!release_copy.load())std::this_thread::yield();
    }
    return __real_memcpy(dst,src,size);
}
extern "C" int __real_pthread_cond_broadcast(pthread_cond_t*);
extern "C" int __wrap_pthread_cond_broadcast(pthread_cond_t* c) {
    if(fail_notify.exchange(false))return EINVAL;
    return __real_pthread_cond_broadcast(c);
}
Message Sample(std::uint64_t n,bool valid=true) {Message m{};m.measurement_sequence=n;m.valid=valid;std::memset(m.bytes,n%251,sizeof(m.bytes));return m;}
void Same(const Message& m,std::uint64_t n,bool valid=true){assert(m.measurement_sequence==n && m.valid==valid);for(auto b:m.bytes)assert(b==n%251);}
template<class F> void Until(F f){const auto end=std::chrono::steady_clock::now()+3s;while(!f()){assert(std::chrono::steady_clock::now()<end);std::this_thread::yield();}}
struct Raw {
    unsigned char* base;std::size_t length;kcf::detail::ChannelLayout layout;
    kcf::detail::ChannelHeader& header(){return *reinterpret_cast<kcf::detail::ChannelHeader*>(base);}
    kcf::detail::ChannelSlot<Message>& slot(std::size_t i){return *reinterpret_cast<kcf::detail::ChannelSlot<Message>*>(base+layout.slots+i*layout.stride);}
    Raw(const std::string& name){int fd=shm_open(name.c_str(),O_RDWR,0);assert(fd>=0);struct stat st{};assert(fstat(fd,&st)==0);length=st.st_size;
        base=static_cast<unsigned char*>(mmap(nullptr,length,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0));assert(base!=MAP_FAILED);close(fd);
        assert(kcf::detail::ComputeChannelLayout(sizeof(Message),alignof(Message),header().depth,layout));}
    ~Raw(){munmap(base,length);}
};
void Basic(const std::string& name,std::uint32_t depth) {
    kcf::Publisher<Message> pub;assert(pub.Create(name,depth)==0);
    kcf::Subscriber<Message> fast,slow;assert(fast.Create(name)==0 && slow.Create(name)==0);assert(fast.GetDepth()==depth);
    Message value{};kcf::TopicReadInfo info{77,88};assert(fast.ReadNext(value,info)==-EAGAIN&&info.sequence==77);
    for(std::uint64_t i=1;i<=10;++i){assert(pub.Publish(Sample(i,i!=4))==0);assert(fast.ReadNext(value,info)==0&&info.sequence==i&&info.missed==0);Same(value,i,i!=4);}
    const auto oldest=11-depth;
    assert(slow.ReadLatest(value,info)==0&&info.sequence==10);Same(value,10);
    assert(slow.ReadNext(value,info)==0&&info.sequence==oldest&&info.missed==oldest-1);Same(value,oldest,oldest!=4);
    for(auto i=oldest+1;i<=10;++i){assert(slow.ReadNext(value,info)==0&&info.sequence==i&&info.missed==0);}
    kcf::Subscriber<Message> next,old;assert(next.Create(name)==0);assert(old.Create(name,kcf::TopicStartPosition::OLDEST)==0);
    assert(next.ReadNext(value,info)==-EAGAIN);assert(old.ReadNext(value,info)==0&&info.sequence==oldest&&info.missed==0);
    // Application sequence need not advance: valid=false still commits transport.
    assert(pub.Publish(Sample(10,false))==0);assert(next.ReadNext(value,info)==0&&info.sequence==11);Same(value,10,false);
    kcf::DynamicTopicReader dynamic;assert(dynamic.Open(name,kcf::TypeDescriptorTraits<Message>::Get())==0);
    kcf::DynamicPayload bytes;assert(dynamic.ReadLatest(bytes)==0&&bytes.sequence==11);std::memcpy(&value,bytes.bytes.data(),sizeof(value));Same(value,10,false);
    dynamic.Close();assert(next.Close()==0&&old.Close()==0&&fast.Close()==0&&slow.Close()==0);
    assert(pub.Close()==0&&pub.Unlink()==0);
}
void Callback(const std::string& name){
    kcf::Publisher<Message> pub;assert(pub.Create(name,4)==0);
    std::atomic<std::uint64_t> seen{0};kcf::Subscriber<Message> sub;
    assert(sub.Create(name,[&](const Message& m){seen=m.measurement_sequence;})==0);
    for(unsigned i=1;i<=8;++i)assert(pub.Publish(Sample(i))==0);
    Until([&]{return seen==8;});Message m{};kcf::TopicReadInfo info;
    assert(sub.ReadNext(m,info)==0&&info.sequence==5&&info.missed==4); // callback did not consume cursor
    assert(sub.Close()==0&&pub.Close()==0&&pub.Unlink()==0);
}
void Pins(const std::string& name){
    kcf::Publisher<Message> pub;assert(pub.Create(name)==0);assert(pub.Publish(Sample(1))==0);
    kcf::SharedChannel<Message> read;assert(read.Open(name,kcf::TopicStartPosition::OLDEST)==0);
    Message m{};kcf::TopicReadInfo info;pause_copy=true;entered=false;release_copy=false;
    std::thread reader([&]{assert(read.ReadNext(m,info)==0);});Until([]{return entered.load();});
    for(unsigned i=2;i<=100;++i)assert(pub.Publish(Sample(i))==0);
    release_copy=true;reader.join();Same(m,1);assert(info.sequence==1);
    assert(read.ReadNext(m,info)==0&&info.sequence==100&&info.missed==98);
    Raw raw(name);const auto seq=raw.header().publish_sequence.load();
    for(unsigned i=0;i<3;++i)if(raw.slot(i).sequence!=seq)raw.slot(i).users.store(1);
    assert(pub.Publish(Sample(101))==-EAGAIN&&raw.header().publish_sequence.load()==seq);
    assert(read.ReadLatest(m,info)==0&&info.sequence==100);Same(m,100);
    for(unsigned i=0;i<3;++i)raw.slot(i).users.store(0);
    fail_notify=true;assert(pub.Publish(Sample(101))==-EINVAL&&raw.header().publish_sequence.load()==seq);
    assert(pub.Publish(Sample(101))==0);assert(read.ReadNext(m,info)==0&&info.sequence==101&&info.missed==0);
    // Real process dies while holding a copy pin. One pin is tolerated, not reclaimed.
    int ready[2];assert(pipe(ready)==0);auto child=fork();assert(child>=0);
    if(!child){close(ready[0]);notice=ready[1];pause_copy=true;kcf::SharedChannel<Message> c;assert(c.Open(name)==0);std::uint64_t q;(void)c.ReadLatestSnapshot(m,q);_exit(1);}
    close(ready[1]);char b;assert(::read(ready[0],&b,1)==1);close(ready[0]);kill(child,SIGKILL);Reap(child);
    unsigned pins=0;for(unsigned i=0;i<3;++i)pins+=raw.slot(i).users.load();assert(pins==1);
    for(unsigned i=102;i<=200;++i)assert(pub.Publish(Sample(i))==0);
    pins=0;for(unsigned i=0;i<3;++i)pins+=raw.slot(i).users.load();assert(pins==1);
    assert(read.Close()==0&&pub.Close()==0&&pub.Unlink()==0);
}
void WideAndFailure(const std::string& name){
    kcf::Publisher<Message> pub;assert(pub.Create(name,2)==0);Raw raw(name);
    raw.header().publish_sequence=UINT32_MAX;raw.header().notify_sequence=UINT32_MAX;
    kcf::Subscriber<Message> sub;assert(sub.Create(name)==0);assert(pub.Publish(Sample(7))==0);
    Message m{};kcf::TopicReadInfo info;assert(sub.ReadNext(m,info)==0&&info.sequence==std::uint64_t(UINT32_MAX)+1&&info.missed==0);
    kcf::DynamicTopicReader dynamic;assert(dynamic.Open(name,kcf::TypeDescriptorTraits<Message>::Get())==0);
    kcf::DynamicPayload bytes;assert(dynamic.ReadLatest(bytes)==0&&bytes.sequence==info.sequence);dynamic.Close();
    raw.header().publish_sequence=UINT64_MAX;
    assert(pub.Publish(Sample(8))==-EOVERFLOW&&raw.header().publish_sequence.load()==UINT64_MAX);
    assert(sub.Close()==0&&pub.Close()==0&&pub.Unlink()==0);
}
void Recreate(const std::string& name){
    kcf::Publisher<Message> a,b;assert(a.Create(name,3)==0&&a.Publish(Sample(1))==0);
    kcf::DynamicTopicReader reader;assert(reader.Open(name,kcf::TypeDescriptorTraits<Message>::Get())==0);
    kcf::SharedChannel<Message> old;assert(old.Open(name,kcf::TopicStartPosition::OLDEST)==0);
    assert(a.Close()==0&&a.Unlink()==0);assert(b.Create(name,2)==0&&b.Publish(Sample(2))==0);
    kcf::DynamicPayload payload;assert(reader.ReadLatest(payload)==-ESTALE);
    Message m{};kcf::TopicReadInfo info;assert(old.ReadNext(m,info)==0&&info.sequence==1);Same(m,1); // explicit old generation
    assert(old.Close()==0&&old.Open(name,kcf::TopicStartPosition::OLDEST)==0);
    assert(old.ReadNext(m,info)==0&&info.sequence==1&&info.missed==0);Same(m,2);
    reader.Close();assert(reader.Open(name,kcf::TypeDescriptorTraits<Message>::Get())==0&&reader.ReadLatest(payload)==0);
    reader.Close();assert(old.Close()==0&&b.Close()==0&&b.Unlink()==0);
}
void ProducerRestart(const std::string& name){
    int ready[2];assert(pipe(ready)==0);const auto child=fork();assert(child>=0);
    if(!child){
        close(ready[0]);kcf::Publisher<Message> pub;assert(pub.Create(name,3)==0);
        for(unsigned i=1;i<=5;++i)assert(pub.Publish(Sample(i))==0);
        assert(write(ready[1],"x",1)==1);for(;;)pause();
    }
    close(ready[1]);char signal;assert(read(ready[0],&signal,1)==1);close(ready[0]);
    kcf::SharedChannel<Message> old;assert(old.Open(name,kcf::TopicStartPosition::OLDEST)==0);
    kcf::DynamicTopicReader dynamic;assert(dynamic.Open(name,kcf::TypeDescriptorTraits<Message>::Get())==0);
    kill(child,SIGKILL);Reap(child);
    kcf::SharedChannel<Message> retry;assert(retry.Open(name)==-EAGAIN);
    kcf::Publisher<Message> replacement;assert(replacement.Create(name,4)==0);
    assert(replacement.Publish(Sample(100))==0);
    kcf::DynamicPayload bytes;assert(dynamic.ReadLatest(bytes)==-ESTALE);
    Message value{};kcf::TopicReadInfo info;
    assert(old.ReadNext(value,info)==0&&info.sequence==3);Same(value,3);
    assert(old.Close()==0&&old.Open(name)==0&&old.GetDepth()==4);
    assert(old.ReadNext(value,info)==-EAGAIN); // NEXT applies again on reconnect
    assert(replacement.Publish(Sample(101))==0);
    assert(old.ReadNext(value,info)==0&&info.sequence==2&&info.missed==0);Same(value,101);
    dynamic.Close();assert(old.Close()==0&&replacement.Close()==0&&replacement.Unlink()==0);
}
void Concurrent(const std::string& name){
    kcf::Publisher<Message> pub;assert(pub.Create(name,8)==0);
    kcf::Subscriber<Message> fast,slow;assert(fast.Create(name)==0&&slow.Create(name)==0);
    auto consume=[&](kcf::Subscriber<Message>& sub,bool delayed){
        std::uint64_t previous=0,total_missed=0,received=0;
        Message value{};kcf::TopicReadInfo info;
        const auto deadline=std::chrono::steady_clock::now()+10s;
        while(previous<2000){
            assert(std::chrono::steady_clock::now()<deadline);
            const int result=sub.ReadNext(value,info);
            if(result==-EAGAIN){std::this_thread::yield();continue;}
            assert(result==0 && info.sequence>previous && info.missed==info.sequence-previous-1);
            Same(value,info.sequence,info.sequence%3!=0);
            previous=info.sequence;total_missed+=info.missed;++received;
            if(delayed)std::this_thread::sleep_for(100us);
        }
        assert(received+total_missed==2000);
    };
    std::thread a([&]{consume(fast,false);}),b([&]{consume(slow,true);});
    for(unsigned i=1;i<=2000;++i){int result;do {result=pub.Publish(Sample(i,i%3!=0));if(result==-EAGAIN)std::this_thread::yield();}while(result==-EAGAIN);assert(result==0);}
    a.join();b.join();assert(fast.Close()==0&&slow.Close()==0&&pub.Close()==0&&pub.Unlink()==0);
}
void ReadRetryAndRetention(const std::string& name){
    kcf::Publisher<Message> pub;assert(pub.Create(name,2)==0);
    kcf::Subscriber<Message> sub;assert(sub.Create(name)==0);
    assert(pub.Publish(Sample(1))==0&&pub.Publish(Sample(2))==0);
    Raw raw(name);unsigned current=0;
    for(unsigned i=0;i<3;++i)if(raw.slot(i).sequence==1)current=i;
    raw.slot(current).users.store(0x80000000u);
    Message value=Sample(99);kcf::TopicReadInfo info{99,99};
    assert(sub.ReadNext(value,info)==-EAGAIN&&info.sequence==99&&info.missed==99);Same(value,99);
    raw.slot(current).users.store(0);
    // Exhaust the target entry's spare slots: a failed KEEP_LAST publish must
    // retain BOTH queued messages, not just the latest snapshot.
    for(unsigned i=0;i<3;++i)if(i!=current)raw.slot(i).users.store(1);
    assert(pub.Publish(Sample(3))==-EAGAIN&&raw.header().publish_sequence.load()==2);
    assert(sub.ReadNext(value,info)==0&&info.sequence==1&&info.missed==0);Same(value,1);
    assert(sub.ReadNext(value,info)==0&&info.sequence==2&&info.missed==0);Same(value,2);
    for(unsigned i=0;i<3;++i)raw.slot(i).users.store(0);
    assert(pub.Publish(Sample(3))==0&&sub.ReadNext(value,info)==0&&info.sequence==3&&info.missed==0);
    assert(sub.Close()==0&&pub.Close()==0&&pub.Unlink()==0);
}
void BadFormats(const std::string& name){
    kcf::SharedChannel<Message> invalid;assert(invalid.Create(name,0)==-EINVAL);NoObject(name);
    {kcf::Publisher<Message> p;assert(p.Create(name)==0);Raw raw(name);raw.header().depth=0;kcf::SharedChannel<Message> c;assert(c.Open(name)==-EPROTO);raw.header().depth=1;assert(p.Close()==0&&p.Unlink()==0);}
    // Complete old format must never be migrated/unlinked, including dead owners.
    int fd=shm_open(name.c_str(),O_CREAT|O_EXCL|O_RDWR,0600);assert(fd>=0);
    kcf::detail::OwnerHeader old{kcf::detail::TOPIC_MAGIC,sizeof(Message),alignof(Message),3,1,INT32_MAX,0};
    assert(write(fd,&old,sizeof(old))==sizeof(old));close(fd);
    assert(invalid.Create(name)==-EEXIST);fd=shm_open(name.c_str(),O_RDONLY,0);assert(fd>=0);close(fd);assert(shm_unlink(name.c_str())==0);
}
int main(){alarm(60);const auto base="/kcf_queue_"+std::to_string(getpid());const auto fds=Fds();
 Basic(base+"_one",1);Basic(base+"_four",4);Callback(base+"_callback");Pins(base+"_pins");WideAndFailure(base+"_wide");Recreate(base+"_recreate");BadFormats(base+"_format");ProducerRestart(base+"_restart");Concurrent(base+"_concurrent");ReadRetryAndRetention(base+"_retry");
 assert(Fds()==fds);std::cout<<"Queue PASS: depth, NEXT/OLDEST, independent cursor, missed, callback, pin/CAS, failed commit, dead reader, 64-bit, recreate, dynamic latest, valid=false, formats\n";
}
