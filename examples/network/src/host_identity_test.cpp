#include "kcf/network/host_identity.hpp"
#include <cassert>
#include <cerrno>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
namespace n=kcf::network;
namespace fs=std::filesystem;
int main() {
    assert(geteuid()!=0); // Permission tests intentionally require no root privileges.
    char pattern[]="/tmp/kcf-host-identity-XXXXXX";
    char* directory=mkdtemp(pattern);assert(directory);
    const fs::path root=directory;const auto path=(root/"host-id").string();
    n::HostIdentity id{},other{};other.bytes[0]=123;
    auto sentinel=other;
    assert(n::LoadHostIdentity(path,other)==-ENOENT && other==sentinel);
    assert(n::LoadOrCreateHostIdentity(path,id)==0);
    assert((id.bytes[6]&0xf0)==0x40 && (id.bytes[8]&0xc0)==0x80);
    struct stat st{};assert(stat(path.c_str(),&st)==0 && (st.st_mode&0777)==0600 && st.st_size==37);
    std::ifstream input(path);std::string text;std::getline(input,text);
    assert(text.size()==36 && text[8]=='-' && text[13]=='-' && text[18]=='-' && text[23]=='-');
    assert(n::LoadOrCreateHostIdentity(path,other)==0 && other==id);
    pid_t child=fork();assert(child>=0);
    if(!child) {_exit(n::LoadOrCreateHostIdentity(path,other)==0 && other==id?0:1);}
    int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    n::HostIdentity boot,boot2;assert(n::ReadLinuxBootIdentity(boot)==0);
    assert(n::ReadLinuxBootIdentity(boot2)==0 && boot==boot2);
    // Match the kernel string using the same strict file-loading contract.
    std::ifstream kernel("/proc/sys/kernel/random/boot_id");std::string boot_text;std::getline(kernel,boot_text);
    {std::ofstream out(root/"boot");out<<boot_text<<'\n';}
    assert(n::LoadHostIdentity((root/"boot").string(),boot2)==0 && boot==boot2);
    for(const auto& bad: {std::string{},std::string("broken"),std::string("00000000-0000-0000-0000-000000000000"),text+"\nextra",text.substr(1)}) {
        {std::ofstream out(root/"bad");out<<bad;}
        other=sentinel;
        assert(n::LoadOrCreateHostIdentity((root/"bad").string(),other)==-EINVAL && other==sentinel);
        assert(fs::file_size(root/"bad")==bad.size());
    }
    fs::create_symlink(path,root/"link");
    assert(n::LoadOrCreateHostIdentity((root/"link").string(),other)==-ELOOP);
    fs::create_symlink(root/"absent",root/"dangling");
    assert(n::LoadOrCreateHostIdentity((root/"dangling").string(),other)==-ELOOP);
    fs::create_directory_symlink(root,root/"dirlink");
    assert(n::LoadOrCreateHostIdentity((root/"dirlink"/"new").string(),other)==-ELOOP);
    assert(n::LoadOrCreateHostIdentity((root/"missing"/"new").string(),other)==-ENOENT);
    assert(n::LoadOrCreateHostIdentity(root.string(),other)==-EINVAL);
    assert(mkfifo((root/"fifo").c_str(),0600)==0);
    assert(n::LoadOrCreateHostIdentity((root/"fifo").string(),other)==-EINVAL);
    assert(chmod(path.c_str(),0000)==0);
    assert(n::LoadOrCreateHostIdentity(path,other)==-EACCES);
    assert(chmod(path.c_str(),0600)==0);
    fs::create_directory(root/"readonly");assert(chmod((root/"readonly").c_str(),0500)==0);
    assert(n::LoadOrCreateHostIdentity((root/"readonly"/"new").string(),other)==-EACCES);
    assert(chmod((root/"readonly").c_str(),0700)==0);
    child=fork();assert(child>=0);
    if(!child) {
        signal(SIGXFSZ,SIG_IGN);rlimit limit{0,0};assert(setrlimit(RLIMIT_FSIZE,&limit)==0);
        other=sentinel;int result=n::LoadOrCreateHostIdentity((root/"write-failure").string(),other);
        _exit(result==-EFBIG && other==sentinel?0:1);
    }
    assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    assert(!fs::exists(root/"write-failure"));
    // Simultaneous first creators, each persists the ID it received for comparison.
    int gate[2];assert(pipe(gate)==0);pid_t children[16];
    for(int i=0;i<16;++i) {
        children[i]=fork();assert(children[i]>=0);
        if(!children[i]) {
            close(gate[1]);char byte;assert(read(gate[0],&byte,1)==1);close(gate[0]);
            n::HostIdentity result;assert(n::LoadOrCreateHostIdentity((root/"race").string(),result)==0);
            std::ofstream out(root/("result-"+std::to_string(i)),std::ios::binary);
            out.write(reinterpret_cast<const char*>(result.bytes.data()),16);out.close();_exit(0);
        }
    }
    close(gate[0]);for(int i=0;i<16;++i)assert(write(gate[1],"x",1)==1);close(gate[1]);
    for(auto pid:children)assert(waitpid(pid,&status,0)==pid && WIFEXITED(status) && WEXITSTATUS(status)==0);
    assert(n::LoadHostIdentity((root/"race").string(),id)==0);
    for(int i=0;i<16;++i) {
        std::ifstream result(root/("result-"+std::to_string(i)),std::ios::binary);
        result.read(reinterpret_cast<char*>(other.bytes.data()),16);assert(result && other==id);
    }
    for(const auto& entry:fs::directory_iterator(root))assert(entry.path().filename().string().find(".kcf-host-id-")!=0);
    fs::remove_all(root);
    std::cout<<"Host identity: generation/reuse/boot/invalid paths/permissions/write failure/16-process race PASS\n";
}
