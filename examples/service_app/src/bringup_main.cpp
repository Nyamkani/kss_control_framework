#include "bringup/bringup.hpp"
#include <filesystem>
#include <iostream>

int main()
{
    std::error_code error;
    const auto directory=std::filesystem::read_symlink("/proc/self/exe",error).parent_path();
    if(error) { std::cerr<<error.message()<<std::endl; return 1; }
    bringup::Bringup supervisor;
    const int result=supervisor.Setup("service_example",{
        {"service_server",(directory/"kcf_service_server").string(),{}}
    });
    if(result) { std::cerr<<"[Service] Setup failed: "<<result<<std::endl; return result; }
    return supervisor.Run();
}
