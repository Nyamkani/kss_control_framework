#include "bringup/bringup.hpp"
#include <filesystem>
#include <iostream>

int main()
{
    std::error_code error;
    const auto directory=std::filesystem::read_symlink("/proc/self/exe",error).parent_path();
    if(error) { std::cerr<<error.message()<<std::endl; return 1; }
    bringup::Bringup supervisor;
    const int result=supervisor.Setup("timer_example",{
        {"timer_element",(directory/"kcf_timer_element").string(),{}}
    });
    if(result) { std::cerr<<"[Timer] Setup failed: "<<result<<std::endl; return result; }
    return supervisor.Run();
}
