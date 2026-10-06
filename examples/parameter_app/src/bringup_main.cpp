#include "bringup/bringup.hpp"
#include <filesystem>
#include <iostream>

int main()
{
    std::error_code error;
    const auto directory=std::filesystem::read_symlink("/proc/self/exe",error).parent_path();
    if(error) { std::cerr<<error.message()<<std::endl; return 1; }
    bringup::Bringup supervisor;
    const int result=supervisor.Setup("parameter_example",{
        {"parameter_owner",(directory/"kcf_parameter_owner").string(),{}}
    });
    if(result) { std::cerr<<"[Parameter] Setup failed: "<<result<<std::endl; return result; }
    return supervisor.Run();
}
