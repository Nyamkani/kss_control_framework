// Adapted from kss_mecanum_ros v2.0 serial_uart.cpp (Apache-2.0).
// Modified: retain termios 8N1 transport; bound writes/reads and handle partial I/O,
// EINTR and cleanup of partially opened devices. No UART protocol changes.
#include "mecanum/serial_uart.hpp"
#include "mecanum/kinematics.hpp"
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <termios.h>
#include <unistd.h>

namespace mecanum {
int SerialComm::Open(const std::string& device, int baudrate) {
    if (IsOpen()) return -EBUSY;
    speed_t speed;
    switch (baudrate) {
#define BAUD(value) case value: speed = B##value; break
        BAUD(9600); BAUD(19200); BAUD(38400); BAUD(57600); BAUD(115200);
        BAUD(230400); BAUD(460800); BAUD(500000); BAUD(576000); BAUD(921600);
        BAUD(1000000); BAUD(1152000); BAUD(1500000); BAUD(2000000);
        BAUD(2500000); BAUD(3000000); BAUD(3500000); BAUD(4000000);
#undef BAUD
        default: return -EINVAL;
    }
    fd_ = open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) return -errno;
    auto fail = [this](int error) { Close(); return error; };
    if (flock(fd_, LOCK_EX | LOCK_NB)) return fail(-errno);
    termios config{};
    if (tcgetattr(fd_, &config)) return fail(-errno);
    cfmakeraw(&config);
    config.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
    config.c_cflag |= CS8 | CLOCAL | CREAD;
    config.c_cc[VMIN] = 0;
    config.c_cc[VTIME] = 0;
    if (cfsetispeed(&config, speed) || cfsetospeed(&config, speed) ||
        tcsetattr(fd_, TCSANOW, &config) || tcflush(fd_, TCIOFLUSH)) return fail(-errno);
    return 0;
}
int SerialComm::Close() {
    if (fd_ < 0) return 0;
    const int fd = fd_;
    fd_ = -1; // do not retry close on EINTR (descriptor may already be released)
    return close(fd) == 0 ? 0 : -errno;
}
int SerialComm::Discard() {
    return tcflush(fd_, TCIOFLUSH) == 0 ? 0 : -errno;
}
int SerialComm::Wait(short events, std::uint64_t deadline) {
    for (;;) {
        const auto now = MonotonicNs();
        if (now >= deadline) return -ETIMEDOUT;
        const auto remaining_ms = (deadline - now + 999999) / 1000000;
        pollfd p{fd_, events, 0};
        const int result = poll(&p, 1, static_cast<int>(remaining_ms));
        if (result < 0) { if (errno == EINTR) continue; return -errno; }
        if (result == 0) return -ETIMEDOUT;
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) return -EIO;
        if (p.revents & events) return 0;
    }
}
int SerialComm::SendData(const std::string& bytes, std::uint64_t deadline) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const int result = Wait(POLLOUT, deadline);
        if (result) return result;
        if (MonotonicNs() >= deadline) return -ETIMEDOUT;
        const auto count = write(fd_, bytes.data() + sent, bytes.size() - sent);
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return -errno;
        }
        if (!count) return -EIO;
        sent += static_cast<std::size_t>(count);
    }
    return 0;
}
int SerialComm::ReadFrame(std::string& frame, std::uint64_t deadline) {
    frame.clear();
    while (frame.size() < 128) {
        int result = Wait(POLLIN, deadline);
        if (result) return result;
        char byte;
        const auto count = read(fd_, &byte, 1);
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            return -errno;
        }
        if (!count) return -EIO;
        frame.push_back(byte);
        if (byte == '\n') {
            if (frame.size() < 2 || frame[frame.size() - 2] != '\r') return -EPROTO;
            frame.resize(frame.size() - 2);
            return 0;
        }
    }
    return -EMSGSIZE;
}
} // namespace mecanum
