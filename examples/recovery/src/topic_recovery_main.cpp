#include "recovery/test_support.hpp"
#include "kcf/ipc/publisher.hpp"
#include "kcf/ipc/subscriber.hpp"
using namespace recovery_test;
struct Message { std::uint64_t value; };
struct Slot {std::atomic<std::uint32_t> users,version;std::uint32_t sequence;alignas(Message) unsigned char data[sizeof(Message)];};
struct Storage
{
    kcf::detail::OwnerHeader header;
    std::atomic<std::uint32_t> published_index,publish_sequence;
    Slot slots[3];pthread_mutex_t mutex;pthread_cond_t condition;std::uint32_t notify_sequence;
};
int main()
{
    alarm(45);
    const auto baseline=Fds();
    const auto base="/kcf_topic_recovery_"+std::to_string(getpid());
    for(int iteration=0;iteration<20;++iteration)
    {
        const auto name=base+"_"+std::to_string(iteration);
        int ready[2];assert(pipe(ready)==0);
        auto child=fork();assert(child>=0);
        if(!child){close(ready[0]);kcf::Publisher<Message> owner;assert(owner.Create(name)==0);assert(owner.Publish({42})==0);assert(write(ready[1],"x",1)==1);for(;;)pause();}
        close(ready[1]);char byte;assert(read(ready[0],&byte,1)==1);close(ready[0]);
        kcf::Publisher<Message> next;assert(next.Create(name)==-EEXIST);
        kcf::SharedChannel<Message> old;assert(old.Open(name)==0);Message value{};std::uint32_t seq=0;
        assert(old.ReadLatestSnapshot(value,seq)==0&&value.value==42);
        kill(child,SIGKILL);Reap(child);assert(old.StopWait()==0);assert(old.Close()==0);
        int stale_fd=shm_open(name.c_str(),O_RDWR,0600);assert(stale_fd>=0);
        assert(flock(stale_fd,LOCK_EX)==0);
        kcf::SharedChannel<Message> premature;assert(premature.Open(name)==-EAGAIN);
        // Dead-owner Open must return before taking a shared flock, even while
        // a reclaiming owner has the old object's exclusive lock.
        assert(flock(stale_fd,LOCK_UN)==0);close(stale_fd);
        assert(next.Create(name)==0);assert(next.Publish({99})==0);
        kcf::SharedChannel<Message> peer;assert(peer.Open(name)==0);
        assert(peer.ReadLatestSnapshot(value,seq)==0&&value.value==99&&seq==1);
        assert(peer.Close()==0);assert(next.Close()==0);assert(next.Unlink()==0);NoObject(name);
        assert(Fds()==baseline);
    }
    {
        auto name=base+"_init_header";kcf::Publisher<Message> owner;
        Incomplete(name,[&]{return owner.Create(name);},true);assert(owner.Close()==0);assert(owner.Unlink()==0);
    }
    {
        auto name=base+"_init";kcf::Publisher<Message> owner;
        Incomplete(name,[&]{return owner.Create(name);});assert(owner.Close()==0);assert(owner.Unlink()==0);
    }
    {
        auto name=base+"_mutex";kcf::Publisher<Message> owner;assert(owner.Create(name)==0);assert(owner.Publish({7})==0);
        kcf::SharedChannel<Message> peer;assert(peer.Open(name)==0);Mapping<Storage> raw(name);
        KillLocked([&]{assert(pthread_mutex_lock(&raw.p->mutex)==0);raw.p->notify_sequence=999;});
        std::uint32_t seq=0;assert(peer.Wait(seq)==0&&seq==1); // lock EOWNERDEAD repair
        int wait_result=0;
        std::thread waiter([&]{wait_result=peer.Wait(seq);});
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        KillLocked([&]{assert(pthread_mutex_lock(&raw.p->mutex)==0);raw.p->publish_sequence.store(2);});
        waiter.join();assert(wait_result==0&&seq==2); // cond timed reacquire EOWNERDEAD
        kcf::Subscriber<Message> subscriber;assert(subscriber.Create(name,[](const auto&){})==0);
        KillLocked([&]{assert(pthread_mutex_lock(&raw.p->mutex)==0);});
        assert(subscriber.Close()==0);assert(peer.StopWait()==0);assert(peer.Close()==0);
        assert(owner.Close()==0);assert(owner.Unlink()==0);
    }
    {
        auto name=base+"_poison";kcf::Publisher<Message> owner;assert(owner.Create(name)==0);Mapping<Storage> raw(name);
        kcf::Subscriber<Message> peer;
        KillLocked([&]{assert(pthread_mutex_lock(&raw.p->mutex)==0);});
        assert(pthread_mutex_lock(&raw.p->mutex)==EOWNERDEAD);assert(pthread_mutex_unlock(&raw.p->mutex)==0);
        assert(peer.Create(name,[](const auto&){})==0); // start waiter after deterministic poisoning
        assert(owner.Publish({8})==-ENOTRECOVERABLE);assert(peer.Close()==-ENOTRECOVERABLE);
        assert(owner.Close()==0);assert(owner.Unlink()==0);
    }
    assert(Fds()==baseline);
    std::cout<<"Topic recovery PASS: 20 generations, active/incomplete owner protection, robust lock/cond Wait/StopWait, unrecoverable error, FD/SHM cleanup\n";
}
