#include "mecanum/ekf/filter.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace mecanum::ekf {
namespace {
constexpr double pi=3.14159265358979323846;
Matrix Identity() { Matrix a{}; for (unsigned i=0;i<6;++i) a[i*6+i]=1; return a; }
Matrix Multiply(const Matrix& a,const Matrix& b,bool transpose_b=false) {
    Matrix c{};
    for (unsigned i=0;i<6;++i) for (unsigned j=0;j<6;++j) for (unsigned k=0;k<6;++k)
        c[i*6+j]+=a[i*6+k]*b[transpose_b ? j*6+k : k*6+j];
    return c;
}
void Symmetrize(Matrix& p) {
    for (unsigned i=0;i<6;++i) for (unsigned j=0;j<i;++j)
        p[i*6+j]=p[j*6+i]=0.5*p[i*6+j]+0.5*p[j*6+i];
}
bool Healthy(const State& x,const Matrix& p) {
    for (double v:x) if (!std::isfinite(v)) return false;
    for (double v:p) if (!std::isfinite(v)) return false;
    Matrix l{}; // Cholesky: fail closed on loss of positive definiteness
    for (unsigned i=0;i<6;++i) for (unsigned j=0;j<=i;++j) {
        if (std::abs(p[i*6+j]-p[j*6+i])>1e-9*(1+std::abs(p[i*6+j]))) return false;
        double v=p[i*6+j];
        for (unsigned k=0;k<j;++k) v-=l[i*6+k]*l[j*6+k];
        if (i==j) { if (!(v>1e-14) || !std::isfinite(v)) return false; l[i*6+j]=std::sqrt(v); }
        else l[i*6+j]=v/l[j*6+j];
    }
    return true;
}
}
double Wrap(double angle) { return std::remainder(angle,2*pi); }
Filter::Filter(Noise noise):noise_(noise) {
    for (unsigned i=0;i<6;++i) {
        if (!std::isfinite(noise.q[i]) || noise.q[i]<=0 || !std::isfinite(noise.p0[i]) || noise.p0[i]<=0)
            throw std::invalid_argument("Q/P must be finite positive diagonal values");
        p_[i*6+i]=noise.p0[i];
    }
    for (double r:{noise.wheel_r[0],noise.wheel_r[1],noise.wheel_r[2],noise.gyro_r,noise.yaw_r})
        if (!std::isfinite(r) || r<=0) throw std::invalid_argument("R must be finite and positive");
    if (!Healthy()) throw std::invalid_argument("Initial covariance not positive definite");
}
State Filter::Transition(const State& x,double dt) {
    State next=x;
    const double c=std::cos(x[2]),s=std::sin(x[2]);
    next[0]+=dt*(c*x[3]-s*x[4]); next[1]+=dt*(s*x[3]+c*x[4]); next[2]=Wrap(x[2]+dt*x[5]);
    return next;
}
Matrix Filter::Jacobian(const State& x,double dt) {
    Matrix f=Identity(); const double c=std::cos(x[2]),s=std::sin(x[2]);
    f[2]=dt*(-s*x[3]-c*x[4]); f[3]=dt*c; f[4]=-dt*s;
    f[8]=dt*(c*x[3]-s*x[4]); f[9]=dt*s; f[10]=dt*c; f[17]=dt;
    return f;
}
bool Filter::Predict(double dt) {
    if (!std::isfinite(dt) || dt<0 || dt>1) return false;
    auto x=x_; auto p=p_;
    const unsigned steps=std::max(1u,static_cast<unsigned>(std::ceil(dt/0.05)));
    const double h=dt/steps;
    for (unsigned n=0;n<steps;++n) {
        const auto f=Jacobian(x,h);
        p=Multiply(Multiply(f,p),f,true);
        for (unsigned i=0;i<6;++i) p[i*6+i]+=noise_.q[i]*h;
        x=Transition(x,h); Symmetrize(p);
        if (!ekf::Healthy(x,p)) return false;
    }
    x_=x; p_=p; return true;
}
bool Filter::Update(unsigned component,double value,double variance,bool angular) {
    if (component>=6 || !std::isfinite(value) || !std::isfinite(variance) || variance<=0) return false;
    const double innovation=angular ? Wrap(value-x_[component]) : value-x_[component];
    const double s=p_[component*6+component]+variance;
    if (!std::isfinite(innovation) || !std::isfinite(s) || s<=1e-14) return false;
    State k{},x=x_; Matrix a=Identity();
    for (unsigned i=0;i<6;++i) {
        k[i]=p_[i*6+component]/s; x[i]+=k[i]*innovation; a[i*6+component]-=k[i];
    }
    x[2]=Wrap(x[2]);
    auto p=Multiply(Multiply(a,p_),a,true); // Joseph covariance update
    for (unsigned i=0;i<6;++i) for (unsigned j=0;j<6;++j) p[i*6+j]+=k[i]*variance*k[j];
    Symmetrize(p);
    if (!ekf::Healthy(x,p)) return false;
    x_=x; p_=p; return true;
}
bool Filter::InitializeYaw(double yaw) {
    auto x=x_; auto p=p_;
    x[2]=Wrap(yaw); p[14]=noise_.yaw_r;
    if (!ekf::Healthy(x,p)) return false;
    x_=x; p_=p; return true;
}
void Filter::StopMotion() {
    for (unsigned i=3;i<6;++i) {
        x_[i]=0;
        const double variance=std::max(p_[i*6+i],noise_.p0[i]);
        for (unsigned j=0;j<6;++j) p_[i*6+j]=p_[j*6+i]=0;
        p_[i*6+i]=variance;
    }
}
bool Filter::Healthy() const { return ekf::Healthy(x_,p_); }
Fusion::Fusion(std::uint64_t timeout_ns,Noise noise,Selection selection):filter_(noise),selection_(selection),timeout_(timeout_ns) {
    if (!selection_.Wheel() && !selection_.Imu()) throw std::invalid_argument("At least one measurement must be enabled");
    if (!timeout_ns || timeout_ns>1000000000ULL) throw std::invalid_argument("timeout must be (0,1s]");
}
bool Fusion::Fresh(std::uint64_t now) const {
    if (selection_.use_imu_yaw && !anchored_) return false;
    if ((selection_.Wheel() && !velocity_acquired_[0]) ||
        (selection_.use_imu_gyro_z && !velocity_acquired_[1])) return false;
    const bool required[]{selection_.Wheel(),selection_.use_imu_gyro_z,selection_.use_imu_yaw};
    for (unsigned i=0;i<3;++i) if (required[i]) {
        const auto t=last_[i];
        if (!t || t>now || now-t>=timeout_) return false;
    }
    return true;
}
bool Fusion::Valid(std::uint64_t now) const { return !suspended_ && Fresh(now) && !stats_.numerical_errors; }
void Fusion::Suspend(bool new_epoch) {
    if (suspended_ && !new_epoch) return;
    if (!suspended_) ++stats_.gaps;
    velocity_acquired_.fill(false);
    suspended_=true; filter_.StopMotion();
}
void Fusion::Expire(std::uint64_t now) { if (!Fresh(now)) Suspend(); }
void Fusion::SourceChanged(bool imu) {
    if (imu ? !selection_.Imu() : !selection_.Wheel()) return;
    Suspend(true);
    if (imu) { last_[1]=last_[2]=0; if (selection_.imu_yaw_relative) anchored_=false; }
    else last_[0]=0;
}
void Fusion::InvalidateWheel() { if (selection_.Wheel()) { last_[0]=0; Suspend(true); } }
bool Fusion::Process(const Measurement& m,std::uint64_t now) {
    const unsigned index=m.kind==Kind::Wheel ? 0 : m.kind==Kind::Gyro ? 1 : 2;
    const bool enabled[]{selection_.Wheel(),selection_.use_imu_gyro_z,selection_.use_imu_yaw};
    if (!enabled[index]) return false;
    // Absolute heading defines the frame before any accepted update/prediction.
    // Earlier non-yaw samples are not buffered; their producers must send new ones.
    if (selection_.use_imu_yaw && !selection_.imu_yaw_relative && !anchored_ && m.kind!=Kind::Yaw) return false;
    const unsigned count=m.kind==Kind::Wheel ? 3 : 1;
    const bool wheel_enabled[]{selection_.use_wheel_vx,selection_.use_wheel_vy,selection_.use_wheel_wz};
    if (!m.timestamp_ns || m.timestamp_ns>now || now-m.timestamp_ns>=timeout_) { ++stats_.invalid_time; return false; }
    for (unsigned i=0;i<count;++i) if ((m.kind!=Kind::Wheel || wheel_enabled[i]) && !std::isfinite(m.value[i])) { ++stats_.invalid; return false; }
    if (m.timestamp_ns<=last_[index]) { ++stats_.duplicates; return false; }
    if (m.timestamp_ns<time_) { ++stats_.late; return false; }
    const bool advance=time_ && m.timestamp_ns>time_;
    if (advance && !suspended_ && (!Fresh(m.timestamp_ns) || m.timestamp_ns-time_>1000000000ULL)) Suspend();
    Filter candidate=filter_;
    if (advance && !suspended_ && !candidate.Predict((m.timestamp_ns-time_)/1e9)) {
        ++stats_.numerical_errors; Suspend(); return false;
    }
    bool ok=true;
    if (m.kind==Kind::Wheel) {
        for (unsigned i=0;i<3;++i) if (wheel_enabled[i]) ok=ok && candidate.Update(3+i,m.value[i],candidate.Parameters().wheel_r[i]);
    } else if (m.kind==Kind::Gyro) ok=candidate.Update(5,m.value[0],candidate.Parameters().gyro_r);
    else {
        // First yaw defines local heading, NOT north. Reconnect anchors to the
        // retained local heading; it cannot reconstruct motion during the gap.
        // The anchor is derived from this estimate, not independent evidence.
        // Consume its timestamp, but do not shrink covariance with a zero residual.
        if (!selection_.imu_yaw_relative) {
            if (!anchored_) ok=candidate.InitializeYaw(m.value[0]);
            else ok=candidate.Update(2,Wrap(m.value[0]),candidate.Parameters().yaw_r,true);
        } else if (!anchored_) yaw_offset_=Wrap(m.value[0]-candidate.X()[2]);
        else ok=candidate.Update(2,Wrap(m.value[0]-yaw_offset_),candidate.Parameters().yaw_r,true);
    }
    if (!ok) { ++stats_.numerical_errors; Suspend(); return false; }
    filter_=candidate; time_=m.timestamp_ns; last_[index]=m.timestamp_ns;
    if (index<2) velocity_acquired_[index]=true;
    if (m.kind==Kind::Yaw) anchored_=true;
    if (Fresh(now)) suspended_=false;
    else Suspend();
    ++stats_.accepted; return true;
}
}
