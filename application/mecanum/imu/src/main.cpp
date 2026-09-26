#include "mecanum/imu_element.hpp"
#include "kcf/process/process_runtime.hpp"
#include <charconv>
#include <iostream>

int main(int argc, char** argv) {
    mecanum::imu::ImuOptions options;
    for (int i=1;i<argc;++i) {
        const std::string key=argv[i];
        if (key=="--help") {
            std::cout << "Usage: kcf_mecanum_imu [--serial /dev/ttyUSB0] [--baud 115200] "
                         "[--topic-prefix /mecanum] [--invert-x] [--invert-y] [--invert-z]\n";
            return 0;
        }
        if (key=="--invert-x") options.inverted[0]=true;
        else if (key=="--invert-y") options.inverted[1]=true;
        else if (key=="--invert-z") options.inverted[2]=true;
        else {
            if (++i>=argc) { std::cerr << "Missing value\n"; return 2; }
            const std::string value=argv[i];
            if (key=="--serial") options.device=value;
            else if (key=="--topic-prefix") options.topic_prefix=value;
            else if (key=="--baud") {
                unsigned baud=0;
                const auto result=std::from_chars(value.data(),value.data()+value.size(),baud);
                if (result.ec!=std::errc{} || result.ptr!=value.data()+value.size() || !baud) return 2;
                options.baudrate=baud;
            } else { std::cerr << "Unknown option: " << key << '\n'; return 2; }
        }
    }
    mecanum::imu::ImuElement element(options);
    kcf::ProcessRuntime runtime;
    runtime.SetLoopFrequency(100.0); // initial 10 ms target, not hard real-time
    const int result=runtime.Run(element);
    if (result || element.ShutdownError()) {
        std::cerr << "IMU failed: runtime=" << result << " cleanup=" << element.ShutdownError() << '\n';
        return 1;
    }
    return 0;
}
