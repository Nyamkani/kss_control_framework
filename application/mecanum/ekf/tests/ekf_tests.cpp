#include "mecanum/ekf/element.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <sys/wait.h>

using namespace mecanum::ekf;
using namespace std::chrono_literals;
namespace mecanum::ekf {
struct EkfTestAccess {
    static int ClosePublisher(Element& e) { return e.publisher_.Close(); }
    static bool Suspended(const Element& e) { return e.fusion_.Suspended(); }
};
}
void Check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
bool Near(double a,double b,double tolerance=1e-8) { return std::abs(a-b)<tolerance; }
void Triplet(Fusion& f,std::uint64_t t,double vx,double vy,double wz,double yaw) {
    Check(f.Process({Kind::Wheel,t,{vx,vy,wz}},t),"wheel update");
    Check(f.Process({Kind::Gyro,t,{wz,0,0}},t),"gyro update");
    Check(f.Process({Kind::Yaw,t,{yaw,0,0}},t),"yaw update");
    Check(f.Valid(t) && f.Estimate().Healthy(),"valid positive definite estimate");
}
void Mathematics() {
    Filter f;
    Check(f.Update(3,2,0.25) && Near(f.X()[3],1.6) && Near(f.P()[21],0.2),"scalar Kalman gain/Joseph covariance");
    State x{0.1,-0.2,0.4,0.7,-0.3,0.2};
    const auto jac=Filter::Jacobian(x,0.03);
    for (unsigned j=0;j<6;++j) {
        auto a=x,b=x; a[j]+=1e-6; b[j]-=1e-6;
        const auto fa=Filter::Transition(a,0.03),fb=Filter::Transition(b,0.03);
        for (unsigned i=0;i<6;++i)
            Check(Near(jac[i*6+j],(fa[i]-fb[i])/2e-6,1e-6),"Jacobian finite difference");
    }
    const auto before=f.X(); const auto covariance=f.P();
    Check(!f.Update(0,std::numeric_limits<double>::quiet_NaN(),1) && !f.Update(0,1,0) &&
          !f.Predict(-1) && !f.Predict(2) && !f.Predict(std::numeric_limits<double>::infinity()),"invalid numeric input rejected");
    Check(f.X()==before && f.P()==covariance,"failed math is transactional");
    Check(!f.Update(3,2,1e-30) && f.Healthy() && f.P()==covariance,"near singular posterior rejected without corruption");
    bool rejected=false;
    try { Noise n; n.q[0]=-1; Filter bad(n); } catch (const std::invalid_argument&) { rejected=true; }
    Check(rejected,"invalid Q configuration rejected");
}
void YawAnchorCovariance() {
    constexpr double pi=3.14159265358979323846;
    constexpr std::uint64_t start=1000000000ULL, step=20000000ULL;
    Fusion f;
    auto velocity=[&](std::uint64_t t) {
        Check(f.Process({Kind::Wheel,t,{1,0.2,0}},t),"anchor test wheel update");
        Check(f.Process({Kind::Gyro,t,{0,0,0}},t),"anchor test gyro update");
    };
    auto anchor=[&](std::uint64_t t,double raw,const char* label) {
        const auto x=f.Estimate().X(); const auto p=f.Estimate().P();
        Check(f.Process({Kind::Yaw,t,{raw,0,0}},t),"accept yaw anchor");
        Check(f.Estimate().X()==x && f.Estimate().P()==p,"anchor leaves entire state and covariance unchanged");
        Check(f.InputTimes()[2]==t && f.Valid(t),"anchor consumed for timestamp and freshness");
        Check(!f.Process({Kind::Yaw,t,{raw,0,0}},t) && f.Estimate().P()==p,"anchor timestamp cannot be fused again");
        std::cout << label << " P_yaw: " << p[14] << " -> " << f.Estimate().P()[14] << '\n';
    };
    auto update=[&](std::uint64_t t,double raw,double relative) {
        const auto x=f.Estimate().X(); const double p=f.Estimate().P()[14];
        const double r=f.Estimate().Parameters().yaw_r, k=p/(p+r);
        Check(f.Process({Kind::Yaw,t,{raw,0,0}},t),"next new yaw is updated");
        Check(Near(f.Estimate().X()[2],Wrap(x[2]+k*Wrap(relative-x[2]))) &&
              Near(f.Estimate().P()[14],p*r/(p+r)) && f.Estimate().P()[14]<p,
              "subsequent yaw uses normal Kalman gain and covariance update");
        std::cout << "New yaw P_yaw: " << p << " -> " << f.Estimate().P()[14] << '\n';
    };
    velocity(start);
    Check(Near(f.Estimate().P()[14],0.04),"initial yaw variance");
    anchor(start,179*pi/180,"Initial anchor");
    velocity(start+step);
    update(start+step,-179*pi/180,2*pi/180);
    const auto pose=f.Estimate().X();
    Check(std::abs(pose[0])>1e-3 && std::abs(pose[1])>1e-3 && std::abs(pose[2])>1e-3,
          "reanchor exercises nonzero local position and yaw");
    const double yaw_variance=f.Estimate().P()[14];
    f.SourceChanged(true);
    velocity(start+2*step);
    anchor(start+2*step,-0.5,"Reconnect anchor");
    for (unsigned i=0;i<3;++i) Check(f.Estimate().X()[i]==pose[i],"reanchor preserves local pose continuity");
    Check(f.Estimate().P()[14]==yaw_variance,"reconnect and anchor do not shrink yaw variance");
    velocity(start+3*step);
    update(start+3*step,-0.48,Wrap(pose[2]+0.02));
}
Selection Mask(unsigned mask) {
    Selection s;
    s.use_wheel_vx=mask&1; s.use_wheel_vy=mask&2; s.use_wheel_wz=mask&4;
    s.use_imu_gyro_z=mask&8; s.use_imu_yaw=mask&16;
    return s;
}
void SelectionAndInitialization() {
    constexpr std::uint64_t t=1000000000ULL, step=20000000ULL;
    constexpr double pi=3.14159265358979323846, angle=70*pi/180;
    bool rejected=false;
    try { Fusion f(500000000ULL,{},Mask(0)); } catch (const std::invalid_argument&) { rejected=true; }
    Check(rejected,"all disabled rejected");
    for (unsigned mask=1;mask<32;++mask) {
        auto selection=Mask(mask); Fusion f(500000000ULL,{},selection);
        const double nan=std::numeric_limits<double>::quiet_NaN();
        const Measurement wheel{Kind::Wheel,t,{selection.use_wheel_vx ? 1 : nan,
                                               selection.use_wheel_vy ? 2 : nan,
                                               selection.use_wheel_wz ? 0.3 : nan}};
        Check(f.Process(wheel,t)==selection.Wheel(),"selected wheel components only");
        Check(f.Process({Kind::Gyro,t,{0.3,0,0}},t)==selection.use_imu_gyro_z,"selected gyro only");
        Check(f.Process({Kind::Yaw,t,{angle,0,0}},t)==selection.use_imu_yaw,"selected yaw only");
        Check(f.Valid(t),"all enabled subsets become valid");
        for (unsigned i=0;i<3;++i) {
            const bool observed=i==0 ? selection.use_wheel_vx : i==1 ? selection.use_wheel_vy : selection.use_wheel_wz || selection.use_imu_gyro_z;
            Check(observed ? f.Estimate().P()[(i+3)*6+i+3]<1 : f.Estimate().X()[i+3]==0 && f.Estimate().P()[(i+3)*6+i+3]==1,
                  "disabled velocity component not updated");
        }
        // Keep only selected inputs alive beyond the default freshness interval.
        for (unsigned n=1;n<=30;++n) {
            auto w=wheel; w.timestamp_ns=t+n*step;
            if (selection.Wheel()) Check(f.Process(w,w.timestamp_ns),"selected wheel refresh");
            if (selection.use_imu_gyro_z) Check(f.Process({Kind::Gyro,w.timestamp_ns,{0.3,0,0}},w.timestamp_ns),"selected gyro refresh");
            if (selection.use_imu_yaw) Check(f.Process({Kind::Yaw,w.timestamp_ns,{angle,0,0}},w.timestamp_ns),"selected yaw refresh");
            f.Expire(w.timestamp_ns); Check(f.Valid(w.timestamp_ns),"disabled timestamps do not expire fusion");
        }
        const auto state=f.Estimate().X(); const auto covariance=f.Estimate().P();
        if (!selection.Wheel()) { f.SourceChanged(false); f.InvalidateWheel(); }
        if (!selection.Imu()) f.SourceChanged(true);
        Check(f.Estimate().X()==state && f.Estimate().P()==covariance,"disabled source changes ignored");
    }
    auto relative=Mask(16); Fusion rel(500000000ULL,{},relative);
    const auto initial=rel.Estimate().P();
    Check(rel.Process({Kind::Yaw,t,{angle,0,0}},t) && rel.Estimate().X()[2]==0 && rel.Estimate().P()==initial,
          "70 degree relative anchor is zero without covariance update");
    auto absolute=relative; absolute.imu_yaw_relative=false;
    Noise noise; noise.yaw_r=0.09; // test initialization uses measurement variance, not a tuned default
    Fusion abs(500000000ULL,noise,absolute);
    Check(abs.Process({Kind::Yaw,t,{angle,0,0}},t) && Near(abs.Estimate().X()[2],angle) && Near(abs.Estimate().P()[14],0.09),
          "absolute yaw initializes to measurement and R exactly once");
    const auto absolute_p=abs.Estimate().P();
    Check(!abs.Process({Kind::Yaw,t,{angle,0,0}},t) && abs.Estimate().P()==absolute_p,"absolute initial duplicate rejected");
    Check(abs.Process({Kind::Yaw,t+step,{angle+0.1,0,0}},t+step) && abs.Estimate().X()[2]>angle && abs.Estimate().P()[14]<0.09,
          "absolute next yaw normal update");
    const auto heading=abs.Estimate().X()[2]; const auto yaw_p=abs.Estimate().P()[14];
    abs.SourceChanged(true);
    Check(abs.Estimate().X()[2]==heading && abs.Estimate().P()[14]==yaw_p,"absolute source change preserves heading and yaw variance");
    Check(abs.Process({Kind::Yaw,t+2*step,{heading+0.1,0,0}},t+2*step) && abs.Estimate().X()[2]>heading && abs.Estimate().P()[14]<yaw_p,
          "absolute reconnect uses sensor heading without reanchor");
    Fusion boundary(500000000ULL,{},absolute);
    Check(boundary.Process({Kind::Yaw,t,{179*pi/180,0,0}},t),"absolute boundary initialize");
    Check(boundary.Process({Kind::Yaw,t+step,{-179*pi/180,0,0}},t+step) &&
          std::abs(Wrap(boundary.Estimate().X()[2]-179*pi/180))<3*pi/180,"absolute pi boundary residual");
    Selection gated; gated.imu_yaw_relative=false;
    Fusion gate(500000000ULL,{},gated); const auto gate_p=gate.Estimate().P();
    Check(!gate.Process({Kind::Wheel,t,{1,2,3}},t) && !gate.Process({Kind::Gyro,t,{1,0,0}},t) &&
          gate.Time()==0 && gate.Estimate().P()==gate_p,"no pre-heading state updates or prediction in absolute mode");
    Check(gate.Process({Kind::Yaw,t+step,{angle,0,0}},t+step) && !gate.Valid(t+step),"absolute initialization waits for fresh remaining inputs");
    Check(gate.Process({Kind::Wheel,t+2*step,{0,0,0}},t+2*step) && gate.Process({Kind::Gyro,t+2*step,{0,0,0}},t+2*step) &&
          gate.Valid(t+2*step) && Near(gate.Estimate().X()[2],angle),"absolute initialization then normal fusion");
}
void PartialRecovery() {
    constexpr std::uint64_t t=1000000000ULL, timeout=500000000ULL;
    for (unsigned mask:{31u,25u,15u,23u}) { // default, vx only Wheel, no yaw, no gyro
        for (bool wheel_first:{true,false}) {
            const auto selection=Mask(mask); Fusion f(timeout,{},selection);
            auto wheel=[&](std::uint64_t at) {
                Check(f.Process({Kind::Wheel,at,{1,0.2,0.3}},at),"recovery wheel accepted");
                Check(f.InputTimes()[0]==at && f.Estimate().X()[3]>0,"wheel state and accepted timestamp agree");
            };
            auto imu=[&](std::uint64_t at) {
                if (selection.use_imu_gyro_z) {
                    Check(f.Process({Kind::Gyro,at,{0.3,0,0}},at),"recovery gyro accepted");
                    Check(f.InputTimes()[1]==at && f.Estimate().X()[5]>0,"gyro state and timestamp agree");
                }
                if (selection.use_imu_yaw) {
                    Check(f.Process({Kind::Yaw,at,{0,0,0}},at),"recovery yaw accepted");
                    Check(f.InputTimes()[2]==at,"yaw timestamp recorded");
                }
            };
            wheel(t); imu(t); Check(f.Valid(t),"recovery initial valid");
            const auto pose=f.Estimate().X(); const auto times=f.InputTimes();
            f.Expire(t+timeout);
            Check(f.Suspended() && !f.Valid(t+timeout) && f.Estimate().X()[3]==0 && f.Estimate().X()[5]==0 &&
                  f.InputTimes()==times,"entry resets twist once and retains historical times");
            auto at=t+timeout+10000000ULL;
            if (wheel_first) wheel(at); else imu(at);
            const auto state=f.Estimate().X(); const auto covariance=f.Estimate().P(); const auto partial_times=f.InputTimes();
            f.Expire(at);
            Check(f.Suspended() && !f.Valid(at) && f.Estimate().X()==state && f.Estimate().P()==covariance &&
                  f.InputTimes()==partial_times,"partial recovery measurements survive Expire");
            // Only one source continues; no prediction is allowed, even over many Loops.
            for (unsigned i=0;i<4;++i) {
                at+=10000000ULL;
                if (wheel_first) wheel(at); else imu(at);
                f.Expire(at);
                Check(f.Suspended() && !f.Valid(at) && Near(f.Estimate().X()[0],pose[0]) && Near(f.Estimate().X()[1],pose[1]),
                      "one-sided input cannot resume position prediction");
            }
            // The partial sample itself expires: preserve state but deny recovery on that old time.
            at+=timeout;
            const auto stale=f.Estimate().X(); f.Expire(at);
            Check(f.Estimate().X()==stale && f.Suspended(),"partial re-expiry does not erase acquired twist");
            if (wheel_first) imu(at); else wheel(at);
            f.Expire(at); Check(f.Suspended() && !f.Valid(at),"expired partial input cannot complete recovery");
            at+=10000000ULL;
            if (wheel_first) wheel(at); else imu(at);
            f.Expire(at);
            Check(!f.Suspended() && f.Valid(at) && f.Estimate().X()[3]>0 &&
                  Near(f.Estimate().X()[0],pose[0]) && Near(f.Estimate().X()[1],pose[1]),"recovery completes without gap integration");
            at+=10000000ULL; wheel(at);
            Check(f.Estimate().X()[0]>pose[0],"prediction resumes only after recovery");
        }
    }
    Fusion f; Triplet(f,t,1,0,0,0);
    // Only yaw expires; recent pre-reset velocity timestamps cannot restore validity.
    Check(f.Process({Kind::Wheel,t+400000000ULL,{1,0,0}},t+400000000ULL),"fresh pre-gap wheel");
    Check(f.Process({Kind::Gyro,t+400000000ULL,{0,0,0}},t+400000000ULL),"fresh pre-gap gyro");
    f.Expire(t+timeout);
    Check(f.Process({Kind::Yaw,t+timeout,{0,0,0}},t+timeout) && f.Suspended() && !f.Valid(t+timeout),"old fresh velocity times cannot satisfy reset epoch");
    Check(f.Process({Kind::Wheel,t+timeout+1,{1,0,0}},t+timeout+1) && f.Suspended(),"new wheel alone insufficient");
    f.SourceChanged(true); // new epoch while partially recovered
    Check(f.Estimate().X()[3]==0 && f.Suspended(),"source change restarts acquisition epoch");
    Check(f.Process({Kind::Gyro,t+timeout+2,{0,0,0}},t+timeout+2) &&
          f.Process({Kind::Yaw,t+timeout+2,{0,0,0}},t+timeout+2) && f.Suspended(),"prior epoch wheel cannot recover after reconnect");
    Check(f.Process({Kind::Wheel,t+timeout+3,{1,0,0}},t+timeout+3) && f.Valid(t+timeout+3),"new epoch complete");
    f.InvalidateWheel();
    Check(f.Process({Kind::Wheel,t+timeout+4,{1,0,0}},t+timeout+4) && f.Suspended(),"resynchronization requires new gyro too");
    Check(f.Process({Kind::Gyro,t+timeout+5,{0,0,0}},t+timeout+5) && f.Valid(t+timeout+5),"fresh unchanged yaw suffices after velocity reset");
}
void MotionAndTime() {
    constexpr double pi=3.14159265358979323846;
    for (const auto velocity:{std::array<double,3>{0,0,0},{1,0,0},{0,1,0},{0,0,0.5},{0.5,0.2,0.3}}) {
        Fusion f;
        State truth{}; truth[3]=velocity[0]; truth[4]=velocity[1]; truth[5]=velocity[2];
        const std::uint64_t start=1000000000ULL;
        for (unsigned i=0;i<=250;++i) {
            if (i) truth=Filter::Transition(truth,0.02);
            Triplet(f,start+i*20000000ULL,velocity[0],velocity[1],velocity[2],Wrap(1.2+truth[2]));
        }
        // Constant-velocity analytic reference, independent of EKF transition.
        const double w=velocity[2],t=5;
        const double tx=w==0 ? velocity[0]*t : (velocity[0]*std::sin(w*t)+velocity[1]*(std::cos(w*t)-1))/w;
        const double ty=w==0 ? velocity[1]*t : (velocity[0]*(1-std::cos(w*t))+velocity[1]*std::sin(w*t))/w;
        Check(Near(f.Estimate().X()[0],tx,0.08) && Near(f.Estimate().X()[1],ty,0.08) &&
              Near(Wrap(f.Estimate().X()[2]-w*t),0,0.025),"stationary/straight/lateral/rotation trajectory");
        const auto x=f.Estimate().X(); const auto p=f.Estimate().P(); const auto ts=f.Time();
        Check(!f.Process({Kind::Wheel,ts,{99,99,99}},ts) && f.Estimate().X()==x && f.Estimate().P()==p,"duplicate timestamp is not fused twice");
        Check(!f.Process({Kind::Yaw,ts-1,{0,0,0}},ts),"reverse component timestamp rejected");
        Check(!f.Process({Kind::Wheel,ts+1,{0,0,0}},ts),"future timestamp rejected");
        Check(!f.Process({Kind::Gyro,0,{0,0,0}},ts),"zero timestamp rejected");
        Check(!f.Process({Kind::Wheel,ts+1,{std::numeric_limits<double>::infinity(),0,0}},ts+1),"nonfinite measurement rejected");
        f.Expire(ts+500000000ULL);
        Check(!f.Valid(ts+500000000ULL) && Near(f.Estimate().X()[3],0) && Near(f.Estimate().X()[5],0),"expiry stops stale twist");
        Check(Near(f.Estimate().X()[0],x[0]) && f.Time()==ts,"expiry never invents measurement time or integrates silence");
        Triplet(f,ts+5000000000ULL,0,0,0,Wrap(1.2+x[2]));
        Check(Near(f.Estimate().X()[0],x[0],1e-5) && Near(f.Estimate().X()[1],x[1],1e-5),"long gap is not integrated with old velocity");
    }
    Fusion wrap;
    Triplet(wrap,1000000000ULL,0,0,0,179*pi/180);
    Triplet(wrap,1020000000ULL,0,0,0,-179*pi/180);
    Check(wrap.Estimate().X()[2]>0 && wrap.Estimate().X()[2]<3*pi/180,"yaw innovation wraps through pi");
    const auto yaw=wrap.Estimate().X()[2];
    wrap.SourceChanged(true);
    Triplet(wrap,1040000000ULL,0,0,0,-0.5);
    Check(Near(wrap.Estimate().X()[2],yaw,1e-8),"new IMU generation reanchors to retained local heading");
    Fusion late;
    Triplet(late,1000000000ULL,0,0,0,0);
    Check(late.Process({Kind::Gyro,1100000000ULL,{0,0,0}},1100000000ULL),"new gyro");
    Check(!late.Process({Kind::Wheel,1050000000ULL,{1,0,0}},1100000000ULL) && late.Stats().late==1,"cross-source out-of-sequence rejected");
    Check(kcf::ValidateTypeDescriptor(kcf::TypeDescriptorTraits<EstimateMessage>::Get()),"Tool descriptor");
}
std::string Prefix() { static unsigned n=0; return "/ekf_test_"+std::to_string(getpid())+"_"+std::to_string(++n); }
struct Fixture {
    std::string prefix=Prefix();
    kcf::Publisher<mecanum::WheelOdometry> wheel;
    kcf::Publisher<mecanum::imu::ImuSample> imu;
    std::uint64_t ws{},is{};
    Fixture() { Check(wheel.Create(prefix+"/wheel",1)==0 && imu.Create(prefix+"/imu",16)==0,"input publishers"); }
    ~Fixture() { wheel.Close(); wheel.Unlink(); imu.Close(); imu.Unlink(); }
    Options Config(std::uint64_t timeout=500000000ULL) {
        Options o; o.wheel_topic=prefix+"/wheel"; o.imu_topic=prefix+"/imu"; o.output_topic=prefix+"/out"; o.timeout_ns=timeout; return o;
    }
    void Wheel(std::uint64_t t,double vx=0,bool valid=true) {
        mecanum::WheelOdometry m{}; m.sequence=++ws; m.timestamp_ns=m.pose_timestamp_ns=t;
        m.x_m=10000; m.y_m=-20000; m.yaw_rad=2; m.vx_m_s=vx; m.valid=valid;
        Check(wheel.Publish(m)==0,"wheel publication");
    }
    void Imu(std::uint64_t t,bool yaw,double value=0,std::uint32_t axes=0) {
        mecanum::imu::ImuSample m{}; m.sequence=++is; m.timestamp_ns=t;
        m.updated_mask=yaw ? mecanum::imu::ANGLE : mecanum::imu::GYRO;
        m.valid_mask=7; m.axis_inversion_mask=axes;
        m.component_timestamp_ns[yaw ? 2 : 1]=t;
        m.component_timestamp_ns[yaw ? 1 : 2]=t-1;
        m.rpy_rad[2]=yaw ? value : 2.7; // cached yaw must NOT be fused on gyro frame
        m.angular_velocity_rad_s[2]=yaw ? 123 : value;
        Check(imu.Publish(m)==0,"IMU publication");
    }
    void Inputs(std::uint64_t t,double vx=0,double yaw=1.1) { Wheel(t,vx); Imu(t,false); Imu(t,true,yaw); }
};
void TopicsAndGeneration() {
    Fixture f; Element e(f.Config()); Check(e.Setup()==0,"EKF setup");
    kcf::Subscriber<EstimateMessage> reader;
    Check(reader.Create(f.prefix+"/out")==0 && reader.GetDepth()==1,"snapshot output");
    const auto base=NowNs()-100000000ULL;
    f.Inputs(base); Check(e.Loop()==0 && e.LastOutput().valid,"first fused estimate");
    Check(Near(e.LastOutput().x_m,0) && Near(e.LastOutput().y_m,0) && Near(e.LastOutput().yaw_rad,0),"wheel position never fused; relative IMU origin");
    const auto stamp=e.LastOutput().timestamp_ns; const double covariance=e.LastOutput().covariance[0];
    Check(e.Loop()==0 && e.LastOutput().timestamp_ns==stamp && Near(e.LastOutput().covariance[0],covariance),"idle snapshot not re-updated");
    f.Imu(base,false); Check(e.Loop()==0 && e.LastOutput().duplicates>0,"component duplicate despite new app sequence");
    for (unsigned i=1;i<=20;++i) f.Imu(base+i*1000000ULL,false);
    Check(e.Loop()==0 && e.LastOutput().imu_missed==4,"ReadNext reports bounded queue loss");
    Check(e.LastOutput().input_timestamp_ns[2]==base && Near(e.LastOutput().yaw_rad,0),"cached angle not fused with gyro");
    f.Wheel(base+5000000ULL,5);
    Check(e.Loop()==0 && e.LastOutput().rejected_time>0,"late Wheel rejected rather than rewinding filter");
    const auto oldgen=e.LastOutput().imu_generation;
    f.imu.Close(); f.imu.Unlink();
    Check(e.Loop()==0 && !e.LastOutput().imu_connected && !e.LastOutput().valid,"unlink invalidates old live mapping");
    Check(f.imu.Create(f.prefix+"/imu",16)==0,"recreate IMU Topic"); f.is=0;
    const auto restart=NowNs()-1000000ULL;
    f.Inputs(restart,0,-2.0);
    Check(e.Loop()==0 && e.LastOutput().valid && e.LastOutput().imu_generation>oldgen &&
          e.LastOutput().reconnects>0 && Near(e.LastOutput().yaw_rad,0),"replacement generation reconnect and relative reanchor");
    // Also detect replacement between Loops, without observing an ENOENT gap.
    const auto wg=e.LastOutput().wheel_generation;
    f.wheel.Close(); f.wheel.Unlink(); Check(f.wheel.Create(f.prefix+"/wheel",1)==0,"instant wheel replacement");
    f.ws=0; f.Wheel(NowNs());
    Check(e.Loop()==0 && e.LastOutput().wheel_generation>wg,"new POSIX identity with identical owner PID");
    const auto ig=e.LastOutput().imu_generation;
    f.is=0; std::this_thread::sleep_for(1ms); const auto apprestart=NowNs();
    f.Imu(apprestart,false); f.Imu(apprestart,true,1.0);
    Check(e.Loop()==0 && e.LastOutput().imu_generation>ig,"application sequence restart without mapping replacement");
    f.Wheel(NowNs(),0,false); Check(e.Loop()==0 && !e.LastOutput().valid,"invalid wheel sample suspends fusion");
    EstimateMessage result{}; kcf::TopicReadInfo info{};
    Check(reader.ReadLatest(result,info)==0 && result.session_id!=0 && result.published_at_ns>=result.timestamp_ns,"output times and session");
    e.Shutdown(); e.Shutdown();
}
void SelectedTopics() {
    for (auto mask:{7u,8u,16u}) {
        Fixture f; auto options=f.Config(); static_cast<Selection&>(options)=Mask(mask);
        if (mask==7) { f.imu.Close(); f.imu.Unlink(); }
        else { f.wheel.Close(); f.wheel.Unlink(); }
        Element e(options); Check(e.Setup()==0,"selected Topic setup");
        const auto t=NowNs();
        if (mask==7) f.Wheel(t,1);
        else f.Imu(t,mask==16,0.1);
        Check(e.Loop()==0 && e.LastOutput().valid,"unused missing Topic does not invalidate output");
        Check(mask==7 ? !e.LastOutput().imu_connected : !e.LastOutput().wheel_connected,"disabled source not connected");
        e.Shutdown();
    }
    Fixture f; auto options=f.Config(); options.imu_yaw_relative=false;
    Element e(options); Check(e.Setup()==0,"absolute Topic setup");
    auto t=NowNs(); f.Imu(t,true,1.2); Check(e.Loop()==0,"initialize absolute Topic yaw");
    t=NowNs(); f.Inputs(t,0,1.2); Check(e.Loop()==0 && e.LastOutput().valid,"absolute sources fresh");
    const auto generation=e.LastOutput().imu_generation;
    f.imu.Close(); f.imu.Unlink(); Check(e.Loop()==0 && !e.LastOutput().valid,"absolute disconnect");
    Check(f.imu.Create(f.prefix+"/imu",16)==0,"absolute recreate"); f.is=0;
    t=NowNs(); f.Inputs(t,0,1.3);
    Check(e.Loop()==0 && e.LastOutput().valid && e.LastOutput().imu_generation>generation &&
          e.LastOutput().yaw_rad>1.2 && e.LastOutput().yaw_rad<1.3,"absolute generation retains frame, normal correction");
    e.Shutdown();
}
void PartialRecoveryOutput() {
    for (bool wheel_first:{true,false}) {
        Fixture f; Element e(f.Config(30000000ULL)); Check(e.Setup()==0,"partial output setup");
        f.Inputs(NowNs(),1); Check(e.Loop()==0 && e.LastOutput().valid,"partial output initially valid");
        std::this_thread::sleep_for(40ms);
        Check(e.Loop()==0 && EkfTestAccess::Suspended(e) && !e.LastOutput().valid && e.LastOutput().vx_m_s==0,
              "output expiry resets velocity and invalidates");
        const auto pose=e.LastOutput().x_m;
        auto t=NowNs();
        if (wheel_first) f.Wheel(t,1);
        else { f.Imu(t,false,0.2); f.Imu(t,true,1.1); }
        Check(e.Loop()==0 && EkfTestAccess::Suspended(e) && !e.LastOutput().valid,"partial output remains suspended");
        const auto partial=e.LastOutput();
        Check(wheel_first ? partial.vx_m_s>0 && partial.input_timestamp_ns[0]==t :
              partial.wz_rad_s>0 && partial.input_timestamp_ns[1]==t,"partial state and timestamp published consistently");
        Check(e.Loop()==0 && e.LastOutput().vx_m_s==partial.vx_m_s && e.LastOutput().wz_rad_s==partial.wz_rad_s &&
              e.LastOutput().timestamp_ns==partial.timestamp_ns && !e.LastOutput().valid,"idle Loop preserves partial measurement");
        t=NowNs();
        if (wheel_first) { f.Imu(t,false); f.Imu(t,true,1.1); }
        else f.Wheel(t,1);
        Check(e.Loop()==0 && !EkfTestAccess::Suspended(e) && e.LastOutput().valid && e.LastOutput().vx_m_s>0 &&
              Near(e.LastOutput().x_m,pose),"output recovery retains wheel velocity and skips gap");
        e.Shutdown();
    }
}
void ExpiryConnectionAndErrors() {
    Fixture f; Element e(f.Config(30000000ULL)); Check(e.Setup()==0,"expiry setup");
    f.Inputs(NowNs(),1); Check(e.Loop()==0 && e.LastOutput().valid,"moving estimate");
    const auto pose=e.LastOutput().x_m; const auto time=e.LastOutput().timestamp_ns;
    std::this_thread::sleep_for(40ms);
    Check(e.Loop()==0 && !e.LastOutput().valid && e.LastOutput().wheel_connected && e.LastOutput().imu_connected &&
          e.LastOutput().vx_m_s==0 && e.LastOutput().timestamp_ns==time && e.LastOutput().x_m==pose,"stale data differs from disconnected Topic, no blind integration");
    f.Inputs(NowNs(),0); Check(e.Loop()==0 && e.LastOutput().valid && Near(e.LastOutput().x_m,pose),"resume without gap integration");
    Check(EkfTestAccess::ClosePublisher(e)==0,"close output for failure injection");
    Check(e.Loop()!=0 && e.LastOutput().publish_failures==1,"Publish failure not success"); e.Shutdown();
    Options absent; const auto p=Prefix(); absent.wheel_topic=p+"/missingwheel"; absent.imu_topic=p+"/missingimu"; absent.output_topic=p+"/out";
    Element waiting(absent); Check(waiting.Setup()==0 && waiting.Loop()==0 && !waiting.LastOutput().valid &&
                                 !waiting.LastOutput().wheel_connected && waiting.LastOutput().timestamp_ns==0,"missing inputs wait invalid");
    waiting.Shutdown();
    Element restarted(absent); Check(restarted.Setup()==0 && restarted.LastOutput().session_id!=waiting.LastOutput().session_id,"EKF restart has separate local session"); restarted.Shutdown();
}
int main() {
    try {
        Mathematics(); SelectionAndInitialization(); PartialRecovery(); YawAnchorCovariance(); MotionAndTime(); TopicsAndGeneration(); SelectedTopics(); PartialRecoveryOutput(); ExpiryConnectionAndErrors();
        std::cout << "PASS: EKF equations/Jacobian/Joseph, planar trajectories/wrap, timestamps/expiry, queue loss and generations\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
