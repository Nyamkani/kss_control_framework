#include "mecanum/motor/motor_element.hpp"
#include "kcf/process/process_runtime.hpp"
#include <charconv>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    std::string serial_port="/dev/ttyUSB1";
    std::int32_t baud_rate=115200;
    for(int i=1;i<argc;i+=2)
    {
        if(i+1>=argc) return 2;
        const std::string key=argv[i], value=argv[i+1];
        if(key=="--serial-port") { if(value.empty()) return 2; serial_port=value; }
        else if(key=="--baud")
        {
            const auto parsed=std::from_chars(value.data(),value.data()+value.size(),baud_rate);
            if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() ||
               mecanum::motor::get_baud(baud_rate)<0) return 2;
        }
        else return 2;
    }
    const auto mode=kcf::DetectLaunchExecutionMode();
    mecanum::motor::MotorElement element(mode,serial_port,baud_rate);
    kcf::ProcessRuntime runtime;
    runtime.SetLoopFrequency(1000.0);
    const int result=runtime.Run(element);
    if(result) std::cerr<<"[Motor] Runtime failed error="<<result<<std::endl;
    return result;
}
