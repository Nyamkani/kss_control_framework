#include "mecanum/wt901c_driver.hpp"
#include "wt901c/serial.h"
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <time.h>

namespace mecanum::imu {
std::uint64_t NowNs() {
    timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC,&ts)) throw std::runtime_error("CLOCK_MONOTONIC failed");
    return std::uint64_t(ts.tv_sec)*1000000000ULL+ts.tv_nsec;
}
std::atomic<Wt901cDriver*> Wt901cDriver::active_{nullptr};
int Wt901cDriver::Open(const std::string& device, unsigned baud) {
    if (fd_>=0) return -EBUSY;
    Wt901cDriver* empty=nullptr;
    if (!active_.compare_exchange_strong(empty,this)) return -EBUSY;
    fd_=serial_open(device.c_str(),baud); // c_str is used only during this call
    if (fd_<0) { const int error=fd_; fd_=-1; active_=nullptr; return error; }
    stats_={}; count_=0;
    WitDeInit();
    std::memset(sReg,0,sizeof(sReg));
    if (WitInit(WIT_PROTOCOL_NORMAL,0x50)!=WIT_HAL_OK ||
        WitRegisterCallBack(&Updated)!=WIT_HAL_OK) {
        ++stats_.sdk_errors; Close(); return -EPROTO;
    }
    return 0; // UART ready only: streaming frames must confirm sensor data
}
int Wt901cDriver::Close() {
    if (fd_<0) return 0;
    const int fd=fd_; fd_=-1;
    stats_.parser=WitGetRxStats();
    WitDeInit();
    const int result=serial_close(fd);
    active_=nullptr;
    return result;
}
void Wt901cDriver::Updated(std::uint32_t reg, std::uint32_t count) {
    auto* driver=active_.load();
    if (!driver || count!=3) return; // ignore temperature/version/other registers
    std::uint32_t component=0;
    if (reg==AX) component=ACC;
    else if (reg==GX) component=GYRO;
    else if (reg==Roll) component=ANGLE;
    else if (reg==HX) component=8;
    if (!component) return;
    if (driver->count_==driver->events_.size()) { ++driver->stats_.sdk_errors; return; }
    auto& event=driver->events_[driver->count_++];
    event.timestamp_ns=NowNs(); event.component=component;
    for (unsigned i=0;i<3;++i) event.raw[i]=sReg[reg+i];
}
int Wt901cDriver::Poll() {
    count_=0;
    if (fd_<0) return -ENODEV;
    const auto errors=stats_.sdk_errors;
    unsigned total=0;
    while (total<512) {
        unsigned char buffer[128];
        const int remaining=512-total;
        const int n=serial_read_data(fd_,buffer,remaining<128 ? remaining : 128);
        if (n<0) { ++stats_.uart_errors; stats_.parser=WitGetRxStats(); return n; }
        if (!n) break;
        total+=n; stats_.bytes+=n;
        for (int i=0;i<n;++i) WitSerialDataIn(buffer[i]);
    }
    if (total>=512) ++stats_.budget_hits; // backlog possible, not proof of byte loss
    stats_.parser=WitGetRxStats();
    return stats_.sdk_errors==errors ? 0 : -EPROTO;
}
}
