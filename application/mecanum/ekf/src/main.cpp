#include "mecanum/ekf/element.hpp"
#include "kcf/process/process_runtime.hpp"
#include <charconv>
#include <iostream>

int main(int argc,char** argv) {
    try {
        mecanum::ekf::Options options;
        for (int i=1;i<argc;++i) {
            const std::string key=argv[i];
            if (key=="--help") {
                std::cout << "Usage: kcf_mecanum_ekf [--wheel-topic NAME] [--imu-topic NAME] "
                             "[--output-topic NAME] [--timeout-ms 500]\n"
                             "  --use-wheel-vx|--use-wheel-vy|--use-wheel-wz true|false\n"
                             "  --use-imu-yaw|--use-imu-gyro-z|--imu-yaw-relative true|false\n";
                return 0;
            }
            if (++i>=argc) return 2;
            const std::string value=argv[i];
            if (key=="--wheel-topic") options.wheel_topic=value;
            else if (key=="--imu-topic") options.imu_topic=value;
            else if (key=="--output-topic") options.output_topic=value;
            else if (key=="--timeout-ms") {
                unsigned ms=0; const auto result=std::from_chars(value.data(),value.data()+value.size(),ms);
                if (result.ec!=std::errc{} || result.ptr!=value.data()+value.size() || !ms || ms>1000) return 2;
                options.timeout_ns=std::uint64_t(ms)*1000000ULL;
            } else {
                bool* flag=nullptr;
                if (key=="--use-wheel-vx") flag=&options.use_wheel_vx;
                else if (key=="--use-wheel-vy") flag=&options.use_wheel_vy;
                else if (key=="--use-wheel-wz") flag=&options.use_wheel_wz;
                else if (key=="--use-imu-yaw") flag=&options.use_imu_yaw;
                else if (key=="--use-imu-gyro-z") flag=&options.use_imu_gyro_z;
                else if (key=="--imu-yaw-relative") flag=&options.imu_yaw_relative;
                if (!flag || (value!="true" && value!="false")) return 2;
                *flag=value=="true";
            }
        }
        mecanum::ekf::Element element(options);
        kcf::ProcessRuntime runtime; runtime.SetLoopFrequency(40.0);
        const int result=runtime.Run(element);
        if (result) std::cerr << "EKF failed: " << result << '\n';
        return result ? 1 : 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
