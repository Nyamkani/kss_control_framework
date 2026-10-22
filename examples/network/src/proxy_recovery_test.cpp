#include "kcf/network/detail/topic_proxy.hpp"
#include "kcf/network/detail/proxy_ownership.hpp"
#include "kcf/network/parameter_codec.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include "kcf/ipc/publisher.hpp"
#include "parameter_demo_type.hpp"
#include <cassert>
#include <csignal>
#include <iostream>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
namespace n=kcf::network;namespace d=n::detail;
n::HostIdentity Id(int x){n::HostIdentity h;h.bytes[0]=x;return h;}
auto Desc(){return kcf::TypeDescriptorTraits<Value>::Get();}
d::ProxyOwnership Owner(){d::ProxyOwnership o;o.proxy={{Id(2),Id(3),getpid(),0},(1ull<<61)|1};assert(kcf::detail::ReadProcessIdentity(getpid(),o.proxy.runtime.process_start_ticks));o.source={{Id(1),Id(4),42,100},1};o.gateway_session=10;o.source_session=20;return o;}
std::string Name(const char* suffix){return "/kcf_proxy_recovery_"+std::to_string(getpid())+suffix;}
std::uint64_t Inode(const std::string& name){int fd=shm_open(name.c_str(),O_RDONLY,0);if(fd<0)return 0;struct stat st{};assert(!fstat(fd,&st));close(fd);return st.st_ino;}
std::vector<std::uint8_t> Read(const std::string& name){int fd=shm_open(name.c_str(),O_RDONLY,0);assert(fd>=0);struct stat st{};assert(!fstat(fd,&st));std::vector<std::uint8_t> b(st.st_size);assert(pread(fd,b.data(),b.size(),0)==static_cast<ssize_t>(b.size()));close(fd);return b;}
void Write(const std::string& name,const std::vector<std::uint8_t>& b){int fd=shm_open(name.c_str(),O_RDWR,0);assert(fd>=0);assert(!ftruncate(fd,b.size()));assert(pwrite(fd,b.data(),b.size(),0)==static_cast<ssize_t>(b.size()));close(fd);}
void Kill(pid_t pid){assert(!kill(pid,SIGKILL));int status;assert(waitpid(pid,&status,0)==pid&&WIFSIGNALED(status));}
pid_t Child(const std::string& name,bool local=false){int pipefd[2];assert(!pipe(pipefd));auto pid=fork();assert(pid>=0);if(!pid){close(pipefd[0]);d::TopicProxy proxy;kcf::Publisher<Value> publisher;int code=local?publisher.Create(name):proxy.Create(name,Desc(),Owner());assert(!code);char ready=1;assert(write(pipefd[1],&ready,1)==1);close(pipefd[1]);for(;;)pause();}close(pipefd[1]);char ready;assert(read(pipefd[0],&ready,1)==1);close(pipefd[0]);return pid;}
void Remove(const std::string& name){shm_unlink(name.c_str());shm_unlink(d::ProxySidecarName(name).c_str());}
void Lifecycle(){auto name=Name("_life");{
    d::TopicProxy proxy;assert(!proxy.Create(name,Desc(),Owner()));assert(proxy.RecoveryState()==n::ProxyRecoveryState::CREATED);
    auto ino=Inode(name);d::TopicProxy conflict;assert(conflict.Create(name,Desc(),Owner())==-EEXIST);assert(conflict.RecoveryState()==n::ProxyRecoveryState::LIVE_NETWORK_OWNER&&Inode(name)==ino);
}assert(!Inode(name)&&!Inode(d::ProxySidecarName(name)));
    auto child=Child(name);auto ino=Inode(name);Kill(child);assert(Inode(name)==ino);
    auto invalid=Owner();invalid.source={};d::TopicProxy refused;assert(refused.Create(name,Desc(),invalid)==-EINVAL&&Inode(name)==ino);
    d::TopicProxy proxy;assert(!proxy.Create(name,Desc(),Owner()));assert(proxy.StaleDetected()&&proxy.RecoveryState()==n::ProxyRecoveryState::RECOVERED&&Inode(name)!=ino);
    kcf::SharedChannel<Value> reader;assert(!reader.Open(name));kcf::DynamicPayload value;Value input{123,false,1.5};value.type_id=Desc().type_id;value.bytes.resize(sizeof(input));std::memcpy(value.bytes.data(),&input,sizeof(input));assert(!proxy.Publish(value));Value output{};std::uint64_t seq=0;assert(!reader.ReadLatestSnapshot(output,seq)&&output.count==123&&seq==1);reader.Close();assert(!proxy.Cleanup());
    // A cleanup crash after Topic unlink leaves only a complete old sidecar.
    child=Child(name);Kill(child);assert(!shm_unlink(name.c_str()));d::TopicProxy orphan;assert(!orphan.Create(name,Desc(),Owner())&&orphan.StaleDetected());
    std::cout<<"normal release, live-owner refusal, SIGKILL recovery, orphan-sidecar cleanup PASS\n";
}
void IdentityAndCodec(){auto o=Owner();assert(d::ProbeProxyOwner(getpid(),o.proxy.runtime.process_start_ticks)==d::OwnerLiveness::LIVE);
    assert(d::ProbeProxyOwner(getpid(),o.proxy.runtime.process_start_ticks+1)==d::OwnerLiveness::DEAD);assert(d::ProbeProxyOwner(0,1)==d::OwnerLiveness::UNKNOWN);assert(d::ProbeProxyOwner(getpid(),0)==d::OwnerLiveness::UNKNOWN);
    auto name=Name("_codec");d::TopicProxy proxy;assert(!proxy.Create(name,Desc(),o));auto bytes=Read(d::ProxySidecarName(name));d::ProxyRecord record;assert(!d::DecodeProxyRecord(bytes,record));assert(record.topic==name&&record.owner.proxy==o.proxy);std::vector<std::uint8_t> encoded;assert(!d::EncodeProxyRecord(record,encoded)&&encoded==bytes);
    for(std::size_t i=0;i<bytes.size();++i){auto truncated=bytes;truncated.resize(i);assert(d::DecodeProxyRecord(truncated,record));}for(std::size_t i=0;i<bytes.size();++i){auto bad=bytes;bad[i]^=1;assert(d::DecodeProxyRecord(bad,record));}
    std::cout<<"PID reuse/liveness uncertainty and bounded BE sidecar validation PASS\n";
}
void Protection(){auto name=Name("_protect");
    auto child=Child(name,true);auto ino=Inode(name);d::TopicProxy live;assert(live.Create(name,Desc(),Owner())==-EEXIST&&Inode(name)==ino);Kill(child);d::TopicProxy dead;assert(dead.Create(name,Desc(),Owner())==-EEXIST&&Inode(name)==ino);Remove(name);
    int fd=shm_open(name.c_str(),O_CREAT|O_EXCL|O_RDWR,0600);assert(fd>=0);char garbage[]="unknown";assert(write(fd,garbage,sizeof(garbage))==sizeof(garbage));close(fd);ino=Inode(name);d::TopicProxy unknown;assert(unknown.Create(name,Desc(),Owner())==-EEXIST&&Inode(name)==ino);Remove(name);
    child=Child(name);Kill(child);ino=Inode(name);auto side=d::ProxySidecarName(name);auto bytes=Read(side);auto bad=bytes;bad[5]^=1;Write(side,bad);d::TopicProxy corrupt;assert(corrupt.Create(name,Desc(),Owner())==-EEXIST&&Inode(name)==ino);Write(side,bytes);
    auto other=Desc();std::strcpy(other.type_name,"kcf.network.other.v1");other.type_id=kcf::StableTypeId(other.type_name);d::TopicProxy type;assert(type.Create(name,other,Owner())==-EEXIST&&Inode(name)==ino);
    d::ProxyRecord record;assert(!d::DecodeProxyRecord(bytes,record));record.inode++;assert(!d::EncodeProxyRecord(record,bad));Write(side,bad);d::TopicProxy mismatch;assert(mismatch.Create(name,Desc(),Owner())==-EEXIST&&Inode(name)==ino);Write(side,bytes);
    // Keep valid old sidecar but replace the name with a real Local publisher.
    assert(!shm_unlink(name.c_str()));kcf::Publisher<Value> publisher;assert(!publisher.Create(name));ino=Inode(name);d::TopicProxy replaced;assert(replaced.Create(name,Desc(),Owner())==-EEXIST&&Inode(name)==ino);assert(!publisher.Publish({9,false,1}));publisher.Unlink();publisher.Close();Remove(name);
    // Correct catalog, corrupt Topic header: still never delete.
    child=Child(name);Kill(child);ino=Inode(name);fd=shm_open(name.c_str(),O_RDWR,0);assert(fd>=0);std::uint64_t zero=0;assert(pwrite(fd,&zero,8,0)==8);close(fd);d::TopicProxy broken;assert(broken.Create(name,Desc(),Owner())==-EEXIST&&Inode(name)==ino);Remove(name);
    std::cout<<"live/dead Local publisher, unknown/corrupt/type/catalog/inode mismatch protection PASS\n";
}
void Concurrent(){auto name=Name("_race");auto stale=Child(name);Kill(stale);auto old=Inode(name);int start[2],results[2];assert(!pipe(start)&&!pipe(results));pid_t children[2];
    for(int i=0;i<2;++i){children[i]=fork();assert(children[i]>=0);if(!children[i]){close(start[1]);close(results[0]);char go;assert(read(start[0],&go,1)==1);d::TopicProxy proxy;int code=-EAGAIN;
        for(int n=0;n<100&&code==-EAGAIN;++n){code=proxy.Create(name,Desc(),Owner());if(code==-EAGAIN)usleep(1000);}assert(write(results[1],&code,sizeof(code))==sizeof(code));for(;;)pause();}}
    close(start[0]);close(results[1]);assert(write(start[1],"xx",2)==2);int codes[2];for(auto& c:codes)assert(read(results[0],&c,sizeof(c))==sizeof(c));assert((codes[0]==0&&codes[1]==-EEXIST)||(codes[1]==0&&codes[0]==-EEXIST));assert(Inode(name)!=old);close(start[1]);close(results[0]);for(auto c:children)Kill(c);d::TopicProxy final;assert(!final.Create(name,Desc(),Owner()));std::cout<<"concurrent recovery single winner/no double unlink PASS\n";
}
int main(){alarm(90);IdentityAndCodec();Lifecycle();Protection();Concurrent();std::cout<<"Proxy recovery PASS\n";}
