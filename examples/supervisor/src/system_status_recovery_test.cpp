#include "recovery/test_support.hpp"
#include "kcf/system/system_status_channel.hpp"
#include <sys/stat.h>
using namespace recovery_test;
using namespace std::chrono_literals;
// Test-only mirror of SharedParameter<SystemStatus>; no production crash hook.
struct Storage
{
    kcf::detail::OwnerHeader header;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    std::uint64_t version,previous_version;
    std::uint32_t active_index,previous_active;
    std::atomic<std::uint32_t> transaction_active;
    alignas(kcf::SystemStatus) unsigned char slots[2][sizeof(kcf::SystemStatus)];
};
static const std::string object="/kcf%2Fsystem%2Fstatus%2Fstate";
static kcf::SystemStatus Value(std::uint64_t seq)
{
    kcf::SystemStatus s{};s.sequence=seq;
    s.state=static_cast<kcf::ApplicationState>(seq%5);
    s.error_valid=1;s.failure_kind=kcf::ElementFailureKind::RUNTIME_ERROR;
    s.failed_pid=123;s.runtime_error=-EFAULT;
    std::strcpy(s.error_element,"reader-crash-origin");return s;
}
int main()
{
    alarm(45);const auto baseline=Fds();
    kcf::SystemStatusPublisher writer;
    auto expected=Value(1);expected.state=kcf::ApplicationState::INITIALIZING;expected.error_valid=0;
    assert(writer.Create(expected)==0);
    kcf::SystemStatusPublisher duplicate;assert(duplicate.Create({})==-EEXIST);
    kcf::SystemStatusSubscriber reader;assert(reader.Open()==0);
    kcf::SystemStatus current{};assert(reader.ReadCurrent(current)==0);
    assert(std::memcmp(&expected,&current,sizeof(current))==0);
    std::atomic<bool> initial_seen{false};
    const auto main_thread=std::this_thread::get_id();
    kcf::SystemStatusSubscriber initial_reader;
    assert(initial_reader.Open([&](const auto& s){
        assert(std::this_thread::get_id()!=main_thread);
        assert(s.sequence==1 && s.state==kcf::ApplicationState::INITIALIZING && !s.error_valid);
        kcf::SystemStatus value{};assert(initial_reader.ReadCurrent(value)==0);
        initial_seen=true;
    })==0);
    while(!initial_seen)std::this_thread::sleep_for(1ms);
    assert(initial_reader.Close()==0);
    struct stat inode{};assert(stat(("/dev/shm"+object).c_str(),&inode)==0);
    const auto stable=Fds();
    {
        Mapping<Storage> raw(object);
        for(int i=0;i<20;++i)
        {
            KillLocked([&]{
                kcf::SystemStatusSubscriber crashed;assert(crashed.Open()==0);
                assert(pthread_mutex_lock(&raw.p->mutex)==0);
            });
            if(i%2) { assert(reader.ReadCurrent(current)==0);assert(current.sequence==expected.sequence); }
            expected=Value(i+2);
            assert(writer.Publish(expected)==0);
            kcf::SystemStatusSubscriber fresh;assert(fresh.Open()==0);
            assert(fresh.ReadCurrent(current)==0 && std::memcmp(&expected,&current,sizeof(current))==0);
            assert(reader.ReadCurrent(current)==0 && current.sequence==expected.sequence);
            assert(fresh.Close()==0);
            struct stat now{};assert(stat(("/dev/shm"+object).c_str(),&now)==0 && now.st_ino==inode.st_ino);
            assert(Fds()==stable);
        }
    }
    // N independent readers, slow latest-value callbacks and callback re-entry.
    std::atomic<unsigned> callbacks{0};
    kcf::SystemStatusSubscriber fast,slow;
    assert(fast.Open([&](const auto& s){assert(s.error_valid==1 && s.failed_pid==123);++callbacks;})==0);
    assert(slow.Open([&](const auto& s){
        kcf::SystemStatus local{};assert(slow.ReadCurrent(local)==0 && local.sequence>=s.sequence);
        assert(std::strcmp(s.error_element,"reader-crash-origin")==0);
        std::this_thread::sleep_for(100ms);++callbacks;
    })==0);
    for(int i=22;i<42;++i){assert(writer.Publish(Value(i))==0);std::this_thread::sleep_for(10ms);}
    assert(slow.Close()==0 && fast.Close()==0 && callbacks>0);
    assert(reader.ReadCurrent(current)==0 && current.sequence==41);
    assert(reader.Close()==0);
    // Unrecoverable synchronization is reported, never an infinite retry.
    {
        std::atomic<bool> entered{false},release{false};
        kcf::SystemStatusSubscriber waiting;
        assert(waiting.Open([&](const auto&){entered=true;while(!release)std::this_thread::sleep_for(1ms);})==0);
        while(!entered)std::this_thread::sleep_for(1ms);
        Mapping<Storage> raw(object);
        KillLocked([&]{assert(pthread_mutex_lock(&raw.p->mutex)==0);});
        assert(pthread_mutex_lock(&raw.p->mutex)==EOWNERDEAD);
        assert(pthread_mutex_unlock(&raw.p->mutex)==0); // deliberately not consistent
        assert(writer.Publish(Value(42))==-ENOTRECOVERABLE);
        release=true;assert(waiting.Close()==-ENOTRECOVERABLE);
        kcf::SystemStatusSubscriber poisoned;assert(poisoned.Open()==0);
        assert(poisoned.ReadCurrent(current)==-ENOTRECOVERABLE);assert(poisoned.Close()==0);
    }
    assert(writer.Close()==0 && writer.Unlink()==0);NoObject(object);
    // Dead writer is reclaimed only after its child is reaped; not a migration.
    int ready[2];assert(pipe(ready)==0);pid_t owner=fork();assert(owner>=0);
    if(!owner){close(ready[0]);kcf::SystemStatusPublisher p;assert(p.Create(Value(90))==0);assert(write(ready[1],"x",1)==1);for(;;)pause();}
    close(ready[1]);char c;assert(read(ready[0],&c,1)==1);close(ready[0]);
    assert(kill(owner,SIGKILL)==0);Reap(owner);
    assert(writer.Create(Value(91))==0);assert(reader.Open()==0);
    assert(reader.ReadCurrent(current)==0 && current.sequence==91);
    assert(reader.Close()==0 && writer.Close()==0 && writer.Unlink()==0);
    NoObject(object);assert(Fds()==baseline);
    int status;assert(waitpid(-1,&status,WNOHANG)==-1 && errno==ECHILD);
    std::cout<<"SystemStatus PASS: initial, 20 dead-reader cycles same inode, N readers/callback re-entry, ENOTRECOVERABLE, dead-writer recovery, FD/reap/SHM cleanup\n";
}
