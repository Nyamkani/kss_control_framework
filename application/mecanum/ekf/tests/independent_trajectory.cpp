#include "mecanum/ekf/filter.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
constexpr double pi=3.14159265358979323846;
using Six=std::array<double,6>;
double Angle(double x) { return std::atan2(std::sin(x),std::cos(x)); }
void Require(bool ok,const char* reason) { if (!ok) throw std::runtime_error(reason); }
struct Scenario {
    const char* name; double vx,vy,w,initial;
    int wheel_ms=20,gyro_ms=20,yaw_ms=20;
    int gap_order=0; // 1: Wheel first, 2: IMU first
};
// Exact constant body-twist trajectory. No EKF Transition/Predict/Wrap reuse.
Six Truth(const Scenario& s,double t,bool relative) {
    const double heading=relative ? 0 : s.initial;
    double x=s.vx*t,y=s.vy*t;
    if (s.w!=0) {
        x=(s.vx*std::sin(s.w*t)+s.vy*(std::cos(s.w*t)-1))/s.w;
        y=(s.vx*(1-std::cos(s.w*t))+s.vy*std::sin(s.w*t))/s.w;
    }
    return {std::cos(heading)*x-std::sin(heading)*y,
            std::sin(heading)*x+std::cos(heading)*y,
            Angle(heading+s.w*t),s.vx,s.vy,s.w};
}
void Covariance(const mecanum::ekf::Fusion& f) {
    const auto& p=f.Estimate().P(); long double l[6][6]{};
    for (double x:f.Estimate().X()) Require(std::isfinite(x),"nonfinite state");
    for (double x:p) Require(std::isfinite(x),"nonfinite covariance");
    for (int i=0;i<6;++i) for (int j=0;j<6;++j)
        Require(std::abs(p[i*6+j]-p[j*6+i])<1e-10,"asymmetric covariance");
    // Independent long-double Cholesky, without the production Healthy helper.
    for (int i=0;i<6;++i) for (int j=0;j<=i;++j) {
        long double value=p[i*6+j];
        for (int k=0;k<j;++k) value-=l[i][k]*l[j][k];
        if (i==j) { Require(value>0 && std::isfinite(value),"not positive definite"); l[i][j]=std::sqrt(value); }
        else l[i][j]=value/l[j][j];
    }
}
bool Available(const Scenario& s,int ms,int source) {
    if (!s.gap_order || ms<2000) return true;
    const int resume=s.gap_order==1 ? (source==0 ? 4000 : source==1 ? 4200 : 4400)
                                  : (source==0 ? 4400 : 4000);
    return ms>=resume;
}
std::string Vector(const Six& v) {
    std::string out="(";
    for (unsigned i=0;i<6;++i) { if (i) out+=", "; out+=std::to_string(v[i]); }
    return out+")";
}
void Run(const Scenario& s,bool relative,std::ofstream& trace) {
    mecanum::ekf::Selection selection; selection.imu_yaw_relative=relative;
    mecanum::ekf::Fusion f(500000000ULL,{},selection);
    Six maximum{},final{}; bool invalid_seen=false,recovered=false;
    std::uint64_t recovery_at=0; double held_x=0,held_y=0;
    unsigned valid_count=0,invalid_count=0;
    for (int ms=0;ms<=8000;ms+=10) {
        const std::uint64_t stamp=1000000000ULL+std::uint64_t(ms)*1000000ULL;
        // Deliver yaw first on equal timestamps so absolute startup is defined.
        for (int source:{2,0,1}) {
            const int period=source==0 ? s.wheel_ms : source==1 ? s.gyro_ms : s.yaw_ms;
            if (ms%period || !Available(s,ms,source)) continue;
            const auto kind=source==0 ? mecanum::ekf::Kind::Wheel : source==1 ? mecanum::ekf::Kind::Gyro : mecanum::ekf::Kind::Yaw;
            const Six truth=Truth(s,ms/1000.0,relative);
            std::array<double,3> z=source==0 ? std::array<double,3>{truth[3],truth[4],truth[5]} :
                std::array<double,3>{source==1 ? truth[5] : Angle(s.initial+s.w*ms/1000.0),0,0};
            Require(f.Process({kind,stamp,z},stamp),"unexpected measurement rejection");
            Covariance(f);
        }
        f.Expire(stamp); Covariance(f);
        const auto truth=Truth(s,ms/1000.0,relative); const auto& estimate=f.Estimate().X();
        for (unsigned i=0;i<6;++i) {
            final[i]=std::abs(i==2 ? Angle(estimate[i]-truth[i]) : estimate[i]-truth[i]);
            maximum[i]=std::max(maximum[i],final[i]);
        }
        if (f.Valid(stamp)) ++valid_count; else ++invalid_count;
        if (!s.gap_order) Require(f.Valid(stamp),"continuous scenario invalid");
        else {
            if (!f.Valid(stamp) && !invalid_seen) { invalid_seen=true; held_x=estimate[0]; held_y=estimate[1]; }
            if (invalid_seen && !recovered) {
                Require(std::abs(estimate[0]-held_x)<1e-10 && std::abs(estimate[1]-held_y)<1e-10,"gap integrated before recovery");
                if (f.Valid(stamp)) { recovered=true; recovery_at=ms; }
            }
            if (ms>=2500 && ms<4400) Require(!f.Valid(stamp) && f.Suspended(),"premature recovery");
        }
        trace<<s.name<<','<<(relative ? "relative" : "absolute")<<','<<ms<<','<<f.Time()<<','<<f.Valid(stamp)<<','<<f.Suspended();
        for (double x:truth) trace<<','<<x;
        for (double x:estimate) trace<<','<<x;
        trace<<'\n';
    }
    if (s.gap_order) {
        Require(invalid_seen && recovered && recovery_at==4400,"missing ordered recovery");
        if (s.vx!=0) Require(std::hypot(final[0],final[1])>2.3 && std::hypot(final[0],final[1])<2.6,"moving-gap residual outside expected missing displacement");
    } else {
        Require(maximum[0]<0.1 && maximum[1]<0.1 && maximum[2]<0.08 &&
                maximum[3]<0.05 && maximum[4]<0.05 && maximum[5]<0.03,"continuous trajectory tolerance exceeded");
    }
    Require(f.Valid(9000000000ULL) && f.Stats().numerical_errors==0,"final estimate invalid");
    if (s.vx==0 && s.vy==0 && s.w==0) for (double e:maximum) Require(e<1e-10,"stationary drift");
    std::cout<<"| "<<s.name<<" | "<<(relative ? "상대" : "절대")<<" | "<<Vector(maximum)<<" | "<<Vector(final)
             <<" | "<<valid_count<<" / "<<invalid_count<<" | 통과 |\n";
}
}
int main(int argc,char** argv) {
    try {
        Require(argc==2,"trace CSV path required");
        std::ofstream trace(argv[1]); Require(bool(trace),"cannot open trace"); trace<<std::setprecision(17);
        trace<<"scenario,mode,time_ms,estimate_timestamp_ns,valid,suspended,reference_x,reference_y,reference_yaw,reference_vx,reference_vy,reference_wz,estimate_x,estimate_y,estimate_yaw,estimate_vx,estimate_vy,estimate_wz\n";
        const Scenario scenarios[]{
            {"정지",0,0,0,70*pi/180}, {"직진",1,0,0,70*pi/180},
            {"횡이동",0,0.7,0,70*pi/180}, {"제자리 회전",0,0,0.6,70*pi/180},
            {"복합 운동",0.6,0.2,0.5,70*pi/180}, {"각도 경계",0.3,0,0.8,179*pi/180},
            {"서로 다른 주기",0.6,0.2,0.5,70*pi/180,50,10,40},
            {"정지 공백",0,0,0,70*pi/180,20,20,20,1},
            {"이동 공백·바퀴 우선",1,0,0,70*pi/180,20,20,20,1},
            {"이동 공백·관성센서 우선",1,0,0,70*pi/180,20,20,20,2}
        };
        std::cout<<"# 독립 궤적 검증 결과\n\n"
                 <<"각 벡터 순서: x, y, yaw, vx, vy, wz. 단위: m, m, rad, m/s, m/s, rad/s.\n\n"
                 <<"최대 오차는 무효 구간을 포함한 현재 시각 기준 절댓값이며 최종 시각은 8초입니다.\n\n"
                 <<"| 시나리오 | 모드 | 최대 절대 오차 | 최종 절대 오차 | 유효 / 무효 표본 수 | 판정 |\n"
                 <<"|---|---|---|---|---|---|\n";
        for (const auto& s:scenarios) for (bool relative:{true,false}) Run(s,relative,trace);
        trace.flush(); Require(bool(trace),"trace write failed");
        std::cout<<"\n20개 시나리오·모드 조합 모두 통과. 모든 측정 갱신 후와 10밀리초 검사 시점에서 공분산의 유한성·대칭성·양의 정부호성을 독립 검사했습니다.\n";
    } catch (const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1; }
}
