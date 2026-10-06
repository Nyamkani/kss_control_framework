#include "bringup/bringup.hpp"
#include <filesystem>
#include <iostream>

int main()
{
    std::error_code error;
    const auto directory=std::filesystem::read_symlink("/proc/self/exe",error).parent_path();
    if(error) { std::cerr<<error.message()<<std::endl; return 1; }
    bringup::Bringup supervisor;
    const int result=supervisor.Setup("pubsub_example",{
        {"publisher",(directory/"kcf_pubsub_publisher").string(),{}},
        {"subscriber",(directory/"kcf_pubsub_subscriber").string(),{}}
    });
    if(result) { std::cerr<<"[PubSub] Setup failed: "<<result<<std::endl; return result; }
    return supervisor.Run();
}
