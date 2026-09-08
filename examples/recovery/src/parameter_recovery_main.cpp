#include "recovery/test_support.hpp"
#include "kcf/parameter/parameter.hpp"
using namespace recovery_test;
struct Large {std::uint64_t words[131072];}; // 1 MiB POD
struct Storage
{
    kcf::detail::OwnerHeader header;pthread_mutex_t mutex;pthread_cond_t condition;
    std::uint64_t version,previous_version;std::uint32_t active_index,previous_active;
    std::atomic<std::uint32_t> transaction_active;
    alignas(Large) unsigned char value_slots[2][sizeof(Large)];
};
int main()
{
    alarm(60);const auto baseline=Fds();const auto base="/kcf_parameter_recovery_"+std::to_string(getpid());
    Large old{},fresh{},value{};for(auto&x:old.words)x=11;for(auto&x:fresh.words)x=22;
    for(int i=0;i<20;++i)
    {
        auto name=base+"_"+std::to_string(i);int ready[2];assert(pipe(ready)==0);auto child=fork();assert(child>=0);
        if(!child){close(ready[0]);kcf::Parameter<Large> owner;assert(owner.Create(name,old)==0);assert(write(ready[1],"x",1)==1);for(;;)pause();}
        close(ready[1]);char b;assert(read(ready[0],&b,1)==1);close(ready[0]);
        kcf::Parameter<Large> owner;assert(owner.Create(name,fresh)==-EEXIST);
        kcf::Parameter<Large> peer;assert(peer.Open(name,[](const auto&){})==0);assert(peer.Set(old)==0);
        assert(peer.Get(value)==0&&value.words[0]==11);
        kill(child,SIGKILL);Reap(child);assert(peer.Close()==0);
        int stale_fd=shm_open(name.c_str(),O_RDWR,0600);assert(stale_fd>=0);
        assert(flock(stale_fd,LOCK_EX)==0);
        kcf::SharedParameter<Large> premature;assert(premature.Open(name)==-EAGAIN);
        // Dead-owner Open must return before taking a shared flock, even while
        // a reclaiming owner has the old object's exclusive lock.
        assert(flock(stale_fd,LOCK_UN)==0);close(stale_fd);
        assert(owner.Create(name,fresh)==0);std::uint64_t version=99;
        assert(owner.Get(value,&version)==0&&version==0&&std::memcmp(&value,&fresh,sizeof(value))==0);
        assert(owner.Close()==0);assert(owner.Unlink()==0);NoObject(name);assert(Fds()==baseline);
    }
    {
        auto name=base+"_init_header";kcf::Parameter<Large> owner;
        Incomplete(name,[&]{return owner.Create(name,old);},true);assert(owner.Close()==0);assert(owner.Unlink()==0);
    }
    {
        auto name=base+"_init";kcf::Parameter<Large> owner;
        Incomplete(name,[&]{return owner.Create(name,old);});assert(owner.Close()==0);assert(owner.Unlink()==0);
    }
    {
        auto name=base+"_txn";kcf::SharedParameter<Large> owner;assert(owner.Create(name,old)==0);Mapping<Storage> raw(name);
        // Deterministic interruption fixtures at partial-copy, metadata-switch,
        // and committed-before-unlock stages, without production hooks.
        for(int stage=0;stage<3;++stage)
        {
            assert(owner.Set(old)==0);std::uint64_t previous;assert(owner.Get(value,&previous)==0);
            KillLocked([&]{
                auto*p=raw.p;assert(pthread_mutex_lock(&p->mutex)==0);
                p->previous_active=p->active_index;p->previous_version=p->version;p->transaction_active.store(1);
                const auto inactive=1-p->active_index;
                std::memcpy(p->value_slots[inactive],&fresh,stage==0?sizeof(fresh)/2:sizeof(fresh));
                if(stage>0){p->active_index=inactive;++p->version;}
                if(stage==2)p->transaction_active.store(0);
            });
            std::uint64_t version;assert(owner.Get(value,&version)==0);
            assert(version==previous+(stage==2));assert(std::memcmp(&value,stage==2?&fresh:&old,sizeof(value))==0);
        }
        // Real public Set stress: kill a client at varying points, including
        // memcpy. Every recovered value/version must be a complete commit.
        for(int i=0;i<30;++i)
        {
            assert(owner.Set(old)==0);std::uint64_t prior;assert(owner.Get(value,&prior)==0);
            pid_t child=fork();assert(child>=0);
            if(!child){kcf::SharedParameter<Large> client;assert(client.Open(name)==0);for(;;){assert(client.Set(fresh)==0);assert(client.Set(old)==0);}}
            usleep(100+(i*131)%3000);kill(child,SIGKILL);Reap(child);
            std::uint64_t version;assert(owner.Get(value,&version)==0);
            assert(std::memcmp(&value,(version-prior)%2?&fresh:&old,sizeof(value))==0);
        }
        std::uint64_t version;assert(owner.Get(value,&version)==0);int waited=0;
        std::thread waiter([&]{waited=owner.Wait(value,version);});
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        KillLocked([&]{auto*p=raw.p;assert(pthread_mutex_lock(&p->mutex)==0);++p->version;});
        waiter.join();assert(waited==0);
        kcf::Parameter<Large> watcher;assert(watcher.Open(name,[](const auto&){})==0);
        KillLocked([&]{assert(pthread_mutex_lock(&raw.p->mutex)==0);});
        assert(watcher.Close()==0);assert(owner.StopWait()==0);assert(owner.Close()==0);assert(owner.Unlink()==0);
    }
    {
        auto name=base+"_poison";
        std::atomic<bool> entered{false},release{false};
        kcf::Parameter<Large> owner;
        assert(owner.Create(name,old,[&](const auto&){entered=true;while(!release)std::this_thread::sleep_for(std::chrono::milliseconds(1));})==0);
        assert(owner.Set(fresh)==0);
        while(!entered)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Mapping<Storage> raw(name);
        KillLocked([&]{assert(pthread_mutex_lock(&raw.p->mutex)==0);});
        assert(pthread_mutex_lock(&raw.p->mutex)==EOWNERDEAD);assert(pthread_mutex_unlock(&raw.p->mutex)==0);
        release=true; // watcher cannot repair the mutex before the test poisons it
        assert(owner.Get(value)==-ENOTRECOVERABLE);assert(owner.Close()==-ENOTRECOVERABLE);assert(owner.Unlink()==0);
    }
    assert(Fds()==baseline);
    std::cout<<"Parameter recovery PASS: 20 generations, rollback/commit, 30 real Set crashes (1 MiB), robust Wait/StopWait, FD/SHM cleanup\n";
}
