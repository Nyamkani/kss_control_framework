#include "mecanum/imu_element.hpp"
#include "kcf/ipc/subscriber.hpp"
#include "wt901c/serial.h"
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace mecanum::imu;
using namespace std::chrono_literals;
namespace mecanum::imu {
struct ImuTestAccess {
    static int ClosePublisher(ImuElement& element) { return element.imu_.Close(); }
};
}
void Check(bool value, const char* what) { if (!value) throw std::runtime_error(what); }
bool Near(double a,double b) { return std::abs(a-b)<1e-9; }
struct Pty {
    int master{-1};
    std::string path;
    Pty() {
        master=posix_openpt(O_RDWR|O_NOCTTY|O_NONBLOCK|O_CLOEXEC);
        Check(master>=0 && grantpt(master)==0 && unlockpt(master)==0,"PTY setup");
        path=ptsname(master);
    }
    ~Pty() { if (master>=0) close(master); }
    void Disconnect() { close(master); master=-1; }
    void Send(const std::vector<unsigned char>& bytes) {
        Check(write(master,bytes.data(),bytes.size())==static_cast<ssize_t>(bytes.size()),"PTY injection");
    }
};
std::vector<unsigned char> Frame(unsigned char type, std::array<std::int16_t,3> data) {
    std::vector<unsigned char> bytes{0x55,type};
    for (auto value:data) { const auto u=static_cast<std::uint16_t>(value); bytes.push_back(u&255); bytes.push_back(u>>8); }
    bytes.push_back(0); bytes.push_back(0);
    unsigned char sum=0; for (auto b:bytes) sum+=b;
    bytes.push_back(sum); return bytes;
}
std::string Prefix() { static unsigned n=0; return "/imu_test_"+std::to_string(getpid())+"_"+std::to_string(++n); }
ImuOptions Options(const Pty& p) { ImuOptions o; o.device=p.path; o.topic_prefix=Prefix(); return o; }
void Drive(ImuElement& e, unsigned samples) {
    for (unsigned i=0;i<50;++i) {
        Check(e.Loop()==0,"IMU Loop");
        if (e.Stats().imu_samples+e.Stats().mag_samples>=samples) return;
        std::this_thread::sleep_for(1ms);
    }
    throw std::runtime_error("missing expected frame");
}
template<class T> T Next(kcf::Subscriber<T>& reader, std::uint64_t missed=0) {
    T value{}; kcf::TopicReadInfo info{};
    Check(reader.ReadNext(value,info)==0 && info.missed==missed,"queue read/missed count");
    return value;
}
void ConversionAndUpdates() {
    constexpr double pi=3.14159265358979323846;
    for (unsigned mask=0;mask<8;++mask) {
        Pty p; auto options=Options(p);
        for (unsigned i=0;i<3;++i) options.inverted[i]=(mask&(1u<<i))!=0;
        ImuElement e(options); Check(e.Setup()==0,"conversion setup");
        kcf::Subscriber<ImuSample> reader;
        kcf::Subscriber<MagSample> mag;
        Check(reader.Create(options.topic_prefix+"/imu/raw_data",kcf::TopicStartPosition::OLDEST)==0 &&
              mag.Create(options.topic_prefix+"/imu/mag_raw",kcf::TopicStartPosition::OLDEST)==0,"topic subscribe");
        Check(reader.GetDepth()==16 && mag.GetDepth()==16,"depth 16 bounded queue");
        auto bytes=Frame(0x51,{2048,-4096,8192});
        p.Send({bytes.begin(),bytes.begin()+5});
        Check(e.Loop()==0 && e.Stats().imu_samples==0,"partial frame never published");
        const auto before=NowNs();
        p.Send({bytes.begin()+5,bytes.end()}); Drive(e,1);
        const auto a=Next(reader);
        Check(a.timestamp_ns>=before && a.timestamp_ns<=NowNs() && a.component_timestamp_ns[0]==a.timestamp_ns,
              "frame receipt time, not Loop replay time");
        Check(a.sequence==1 && a.updated_mask==ACC && a.valid_mask==ACC &&
              a.component_timestamp_ns[1]==0 && a.component_timestamp_ns[2]==0,"partial validity");
        const double expected_acc[3]={9.80665,-19.6133,39.2266};
        for (unsigned i=0;i<3;++i) Check(Near(a.acceleration_m_s2[i],(options.inverted[i]?-1:1)*expected_acc[i]),"g to SI and XYZ indices");
        std::this_thread::sleep_for(1ms);
        p.Send(Frame(0x52,{16384,-8192,4096})); Drive(e,2);
        const auto g=Next(reader);
        Check(g.updated_mask==GYRO && g.valid_mask==(ACC|GYRO) &&
              g.component_timestamp_ns[0]==a.timestamp_ns && g.timestamp_ns>a.timestamp_ns,"gyro doesn't retimestamp acceleration");
        const double expected_gyro[3]={1000*pi/180,-500*pi/180,250*pi/180};
        for (unsigned i=0;i<3;++i) Check(Near(g.angular_velocity_rad_s[i],(options.inverted[i]?-1:1)*expected_gyro[i]),"deg/s to rad/s axes");
        p.Send(Frame(0x53,{4096,8192,16384})); Drive(e,3);
        const auto angle=Next(reader);
        const double expected_angle[3]={pi/8,pi/4,pi/2};
        for (unsigned i=0;i<3;++i) Check(Near(angle.rpy_rad[i],(options.inverted[i]?-1:1)*expected_angle[i]),"Euler degrees to radians axes");
        Check(angle.updated_mask==ANGLE && angle.valid_mask==7 && angle.axis_inversion_mask==mask &&
              angle.component_timestamp_ns[0]==a.timestamp_ns && angle.component_timestamp_ns[1]==g.timestamp_ns,"independent angle timestamp");
        double norm=0; for (double v:angle.orientation_xyzw) norm+=v*v;
        Check(Near(norm,1),"quaternion unit length");
        p.Send(Frame(0x53,{0,0,16384})); Drive(e,4);
        const auto yaw=Next(reader);
        Check(Near(yaw.orientation_xyzw[0],0) && Near(yaw.orientation_xyzw[1],0) &&
              Near(yaw.orientation_xyzw[2],(options.inverted[2]?-1:1)*std::sqrt(0.5)) &&
              Near(yaw.orientation_xyzw[3],std::sqrt(0.5)),"known 90 degree yaw XYZW");
        p.Send(Frame(0x54,{-32768,123,-456})); Drive(e,5);
        const auto m=Next(mag);
        const int expected_mag[3]={-32768,123,-456};
        for (unsigned i=0;i<3;++i) Check(m.magnetic_raw_counts[i]==(options.inverted[i]?-1:1)*expected_mag[i],"raw magnetic XYZ no int16 negation overflow");
        Check(m.sequence==1 && m.valid==1 && m.axis_inversion_mask==mask,"separate magnetic sequence/validity");
        for (unsigned i=0;i<10;++i) Check(e.Loop()==0,"idle Loop");
        ImuSample empty{}; kcf::TopicReadInfo info{};
        Check(reader.ReadNext(empty,info)==-EAGAIN && e.Stats().imu_samples==4 && e.Stats().mag_samples==1,"idle never republishes");
        p.Send(Frame(0x53,{0,0,16384})); Drive(e,6);
        Check(Next(reader).sequence==5,"identical newly received frame is a new measurement");
        e.Shutdown(); e.Shutdown(); // idempotent close/unlink
    }
    Check(kcf::ValidateTypeDescriptor(kcf::TypeDescriptorTraits<ImuSample>::Get()) &&
          kcf::ValidateTypeDescriptor(kcf::TypeDescriptorTraits<MagSample>::Get()),"Tool descriptors");
}
void FramingQueueAndBudget() {
    Pty p; auto o=Options(p); ImuElement e(o); Check(e.Setup()==0,"framing setup");
    kcf::Subscriber<ImuSample> reader;
    Check(reader.Create(o.topic_prefix+"/imu/raw_data",kcf::TopicStartPosition::OLDEST)==0,"queue open");
    p.Send(Frame(0x50,{1,2,3})); // SDK time registers are not an IMU measurement
    Check(e.Loop()==0 && e.Stats().imu_samples==0,"non-IMU registers never republish cached data");
    p.Send(Frame(0x60,{1,2,3})); // checksum-valid, unsupported SDK frame type
    Check(e.Loop()==0 && e.Stats().imu_samples==0 && e.DriverStats().parser.unsupported_frames==1,
          "unsupported frame counted separately without measurement");
    auto bad=Frame(0x51,{1,2,3}); bad.back()^=1;
    p.Send(bad); Check(e.Loop()==0 && e.Stats().imu_samples==0,"bad checksum rejected");
    p.Send(Frame(0x51,{1,2,3})); Drive(e,1); Next(reader);
    Check(e.DriverStats().parser.checksum_errors==1 && e.DriverStats().sdk_errors==0,"packet corruption separate from SDK processing errors");
    std::vector<unsigned char> burst;
    for (int i=0;i<20;++i) { const auto f=Frame(0x52,{static_cast<std::int16_t>(i),0,0}); burst.insert(burst.end(),f.begin(),f.end()); }
    p.Send(burst); Drive(e,21);
    Check(Next(reader,4).sequence==6,"bounded overwrite reports missed frames");
    for (unsigned i=0;i<15;++i) Next(reader);
    Check(e.Stats().imu_publish_failures==0 && e.Stats().imu_published==21,"reader lag is not Publish or SDK failure");
    burst.clear();
    for (unsigned i=0;i<100;++i) { const auto f=Frame(0x51,{1,0,0}); burst.insert(burst.end(),f.begin(),f.end()); }
    const auto bytes=e.DriverStats().bytes;
    p.Send(burst); Check(e.Loop()==0,"bounded flood Loop");
    Check(e.DriverStats().bytes-bytes<=512 && e.DriverStats().budget_hits>0 && e.Stats().imu_samples<121,"finite read budget");
    Drive(e,121); Check(e.Stats().imu_published==121,"backlog resumes next Loop without parser reset");
    const auto start=NowNs(); p.Disconnect();
    Check(e.Loop()<0 && NowNs()-start<100000000ULL && e.DriverStats().uart_errors==1,"negative read exits without infinite loop");
    Check(e.DriverStats().sdk_errors==0,"UART error not SDK error"); e.Shutdown();
}
std::size_t FdCount() {
    std::size_t n=0; for (const auto& entry:std::filesystem::directory_iterator("/proc/self/fd")) { (void)entry; ++n; }
    return n;
}
void ResourcesAndPublishFailure() {
    {
        const auto before=FdCount();
        Wt901cDriver d;
        for (unsigned i=0;i<20;++i) {
            Check(d.Open("/dev/null",115200)<0,"termios init failure");
            Check(d.Open("/definitely/missing/wt901c",115200)<0,"missing device");
            Check(d.Close()==0 && d.Close()==0,"failed init close is idempotent");
        }
        Check(FdCount()==before,"failed initialization does not leak FD");
        Pty a,b;
        Check(d.Open(std::string(a.path),115200)==0,"temporary string path owned by call");
        Wt901cDriver other;
        Check(other.Open(b.path,115200)==-EBUSY && d.Open(a.path,115200)==-EBUSY,"SDK single instance and duplicate open");
        a.Send(Frame(0x51,{1,2,3}));
        for (int i=0;i<20 && !d.EventCount();++i) { Check(d.Poll()==0,"direct SDK Poll"); std::this_thread::sleep_for(1ms); }
        Check(d.EventCount()==1,"FD assignment and SDK callback work");
        d.Close();
        Check(other.Open(b.path,12345)==-EINVAL && other.Open(b.path,115200)==0,"release SDK after close/unsupported baud");
    }
    {
        Pty p; auto o=Options(p);
        kcf::Publisher<MagSample> blocker;
        Check(blocker.Create(o.topic_prefix+"/imu/mag_raw",16)==0,"collision fixture");
        ImuElement e(o); Check(e.Setup()!=0,"partial topic setup fails");
        kcf::Publisher<ImuSample> probe;
        Check(probe.Create(o.topic_prefix+"/imu/raw_data",16)==0,"partial setup cleans owned topic");
        Check(blocker.Publish({})==0,"foreign topic not unlinked");
        Wt901cDriver d; Check(d.Open(p.path,115200)==0,"partial setup releases UART/SDK"); d.Close();
        probe.Close(); probe.Unlink(); blocker.Close(); blocker.Unlink(); e.Shutdown();
    }
    {
        Pty p; auto o=Options(p); ImuElement e(o); Check(e.Setup()==0,"publish failure setup");
        Check(ImuTestAccess::ClosePublisher(e)==0,"close actual publisher");
        p.Send(Frame(0x51,{2048,0,0}));
        int result=0;
        for (int i=0;i<50 && !result;++i) { result=e.Loop(); std::this_thread::sleep_for(1ms); }
        Check(result<0 && e.Stats().imu_samples==1 && e.Stats().imu_published==0 &&
              e.Stats().imu_publish_failures==1 && e.DriverStats().sdk_errors==0 && e.DriverStats().uart_errors==0,
              "failed Publish never counted as success or SDK error");
        Check(e.Loop()==0 && e.Stats().imu_samples==1,"failed frame not retried as a new measurement");
        e.Shutdown();
    }
    unsigned char c=0; Check(serial_read_data(-1,&c,1)<0,"negative serial return contract");
}
int main() {
    try {
        ConversionAndUpdates(); FramingQueueAndBudget(); ResourcesAndPublishFailure();
        std::cout << "PASS: WT901C SDK/PTY frames, SI/8 axis combinations, timestamps/queue, budget, publish failures and cleanup\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
