#include "kcf/network/host_identity.hpp"
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kcf::network {
namespace {
struct Fd {
    int value;
    explicit Fd(int fd = -1): value(fd) {}
    ~Fd() { if (value >= 0) close(value); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
};
int Random(HostIdentity& id) {
    std::size_t offset = 0;
    while (offset < id.bytes.size()) {
        const auto n = getrandom(id.bytes.data()+offset,id.bytes.size()-offset,0);
        if (n < 0) { if (errno == EINTR) continue; return -errno; }
        if (!n) return -EIO;
        offset += static_cast<std::size_t>(n);
    }
    id.bytes[6] = (id.bytes[6] & 0x0f) | 0x40;
    id.bytes[8] = (id.bytes[8] & 0x3f) | 0x80;
    return 0;
}
std::string Format(const HostIdentity& id) {
    std::string text;
    for (std::size_t i=0;i<id.bytes.size();++i) {
        if (i==4 || i==6 || i==8 || i==10) text+='-';
        text+="0123456789abcdef"[id.bytes[i]>>4];
        text+="0123456789abcdef"[id.bytes[i]&15];
    }
    return text;
}
int Read(int fd, HostIdentity& output) {
    char text[38]; std::size_t size=0;
    while (size<sizeof(text)) {
        const auto n=read(fd,text+size,sizeof(text)-size);
        if(n<0) {if(errno==EINTR)continue;return -errno;}
        if(!n)break;
        size+=static_cast<std::size_t>(n);
    }
    if(size==37 && text[36]=='\n')--size;
    if(size!=36)return -EINVAL;
    const auto hex=[](char c)->int {
        if(c>='0'&&c<='9')return c-'0';
        if(c>='a'&&c<='f')return c-'a'+10;
        if(c>='A'&&c<='F')return c-'A'+10;
        return -1;
    };
    HostIdentity id;std::size_t pos=0;bool nonzero=false;
    for(std::size_t i=0;i<id.bytes.size();++i) {
        if(i==4||i==6||i==8||i==10) {if(text[pos++]!='-')return -EINVAL;}
        int a=hex(text[pos++]),b=hex(text[pos++]);
        if(a<0||b<0)return -EINVAL;
        id.bytes[i]=static_cast<std::uint8_t>((a<<4)|b);nonzero|=id.bytes[i]!=0;
    }
    if(!nonzero)return -EINVAL;
    output=id;return 0;
}
int Parent(const std::string& path,Fd& dir,std::string& name) {
    if(path.empty() || path.find('\0')!=std::string::npos || path.back()=='/')return -EINVAL;
    dir.value=open(path.front()=='/'?"/":".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(dir.value<0)return -errno;
    std::size_t start=path.front()=='/'?1:0;
    while(start<path.size()) {
        auto end=path.find('/',start);
        auto part=path.substr(start,end==std::string::npos?end:end-start);
        if(part=="..")return -EINVAL;
        if(end==std::string::npos) {if(part.empty()||part==".")return -EINVAL;name=part;return 0;}
        if(!part.empty()&&part!=".") {
            // O_PATH permits identifying a symlink without following it.
            int next=openat(dir.value,part.c_str(),O_PATH|O_NOFOLLOW|O_CLOEXEC);
            if(next<0)return -errno;
            Fd component(next);struct stat st{};
            if(fstat(next,&st))return -errno;
            if(S_ISLNK(st.st_mode))return -ELOOP;
            if(!S_ISDIR(st.st_mode))return -ENOTDIR;
            int readable=openat(next,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
            if(readable<0)return -errno;
            close(dir.value);dir.value=readable;
        }
        start=end+1;
    }
    return -EINVAL;
}
int Load(int dir,const std::string& name,HostIdentity& output) {
    Fd fd(openat(dir,name.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC));
    if(fd.value<0)return -errno;
    struct stat st{};if(fstat(fd.value,&st))return -errno;
    if(!S_ISREG(st.st_mode))return -EINVAL;
    return Read(fd.value,output);
}
int Identity(const std::string& path,HostIdentity& output,bool create) {
    Fd dir;std::string name;int code=Parent(path,dir,name);if(code)return code;
    while(flock(dir.value,create?LOCK_EX:LOCK_SH)) {if(errno!=EINTR)return -errno;}
    HostIdentity id;code=Load(dir.value,name,id);
    if(!code) {
        // Also complete durability after an earlier caller failed post-publication.
        if(create && fsync(dir.value))return -errno;
        output=id;return 0;
    }
    if(code!=-ENOENT || !create)return code;
    if((code=Random(id)))return code;
    std::string temporary=".kcf-host-id-"+Format(id);
    Fd fd(openat(dir.value,temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));
    if(fd.value<0)return -errno;
    const auto cleanup=[&](int error) {unlinkat(dir.value,temporary.c_str(),0);return error;};
    if(fchmod(fd.value,0600))return cleanup(-errno);
    const auto text=Format(id)+"\n";std::size_t offset=0;
    while(offset<text.size()) {
        auto n=write(fd.value,text.data()+offset,text.size()-offset);
        if(n<0) {if(errno==EINTR)continue;return cleanup(-errno);}
        if(!n)return cleanup(-EIO);
        offset+=static_cast<std::size_t>(n);
    }
    if(fsync(fd.value))return cleanup(-errno);
    if(linkat(dir.value,temporary.c_str(),dir.value,name.c_str(),0)) {
        code=-errno;cleanup(code);
        if(code!=-EEXIST)return code;
        code=Load(dir.value,name,id);if(code)return code;
    } else if(unlinkat(dir.value,temporary.c_str(),0))return -errno;
    if(fsync(dir.value))return -errno;
    output=id;return 0;
}
}
int LoadHostIdentity(const std::string& path,HostIdentity& out) {return Identity(path,out,false);}
int LoadOrCreateHostIdentity(const std::string& path,HostIdentity& out) {return Identity(path,out,true);}
int ReadLinuxBootIdentity(HostIdentity& out) {
    Fd fd(open("/proc/sys/kernel/random/boot_id",O_RDONLY|O_NOFOLLOW|O_CLOEXEC));
    if(fd.value<0)return -errno;
    return Read(fd.value,out);
}
}
