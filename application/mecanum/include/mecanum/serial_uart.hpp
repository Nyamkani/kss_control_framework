#pragma once
#include <cstdint>
#include <string>

namespace mecanum {
// Single caller; bounded nonblocking UART I/O, 8N1, no flow control.
class SerialComm {
public:
    ~SerialComm() { Close(); }
    SerialComm() = default;
    SerialComm(const SerialComm&) = delete;
    SerialComm& operator=(const SerialComm&) = delete;
    int Open(const std::string& device, int baudrate);
    int Close();
    bool IsOpen() const { return fd_ >= 0; }
    int SendData(const std::string&, std::uint64_t deadline_ns);
    int ReadFrame(std::string&, std::uint64_t deadline_ns);
    int Discard();
private:
    int Wait(short events, std::uint64_t deadline_ns);
    int fd_{-1};
};
} // namespace mecanum
