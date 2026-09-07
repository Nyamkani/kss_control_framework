#include "kcf/service/service_client.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <limits>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

namespace kcf
{
ServiceClient::~ServiceClient() { Close(); }

int ServiceClient::Create(std::uint16_t port, std::uint32_t timeout_ms, std::uint32_t retry_count)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ >= 0) return -EBUSY;
    if (port == 0 || timeout_ms == 0) return -EINVAL;
    std::uint64_t identity = 0;
    do
    {
        std::size_t filled = 0;
        while (filled < sizeof(identity))
        {
            const auto result = getrandom(reinterpret_cast<unsigned char*>(&identity) + filled,
                                          sizeof(identity) - filled, 0);
            if (result < 0) { if (errno == EINTR) continue; return -errno; }
            if (result == 0) return -EIO;
            filled += static_cast<std::size_t>(result);
        }
    } while (identity == 0);
    const int socket_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (socket_fd < 0) return -errno;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(socket_fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
    {
        const int error = errno; close(socket_fd); return -error;
    }
    fd_ = socket_fd;
    target_port_ = port;
    timeout_ms_ = timeout_ms;
    retry_count_ = retry_count;
    client_id_ = identity;
    request_id_ = 0;
    return 0;
}

int ServiceClient::CallRaw(std::uint16_t id, const void* input, std::size_t input_size,
                           void* output, std::size_t output_size)
{
    // One outstanding request, including all its retries and response validation.
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ < 0) return -EBADF;
    if (request_id_ == std::numeric_limits<std::uint64_t>::max()) return -EOVERFLOW;
    detail::ServicePacket request;
    request.header.service_id = id;
    request.header.client_id = client_id_;
    request.header.request_id = ++request_id_;
    request.header.payload_size = static_cast<std::uint32_t>(input_size);
    std::memcpy(request.payload.data(), input, input_size);
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    target.sin_port = htons(target_port_);
    using Clock = std::chrono::steady_clock;
    for (std::uint64_t attempt = 0; attempt <= retry_count_; ++attempt)
    {
        const auto size = sizeof(ServiceHeader) + input_size;
        ssize_t sent;
        do
        {
            sent = sendto(fd_, &request, size, 0, reinterpret_cast<const sockaddr*>(&target), sizeof(target));
        } while (sent < 0 && errno == EINTR);
        if (sent < 0) return -errno;
        if (sent != static_cast<ssize_t>(size)) return -EIO;
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms_);
        while (Clock::now() < deadline)
        {
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (remaining <= 0) break;
            pollfd descriptor{fd_, POLLIN, 0};
            const int ready = poll(&descriptor, 1, static_cast<int>(std::min<std::int64_t>(remaining, INT_MAX)));
            if (ready < 0) { if (errno == EINTR) continue; return -errno; }
            if (ready == 0) break;
            if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) return -EIO;
            if (!(descriptor.revents & POLLIN)) continue;
            detail::ServicePacket response;
            sockaddr_in sender{};
            socklen_t sender_size = sizeof(sender);
            const auto received = recvfrom(fd_, &response, sizeof(response), MSG_TRUNC,
                                          reinterpret_cast<sockaddr*>(&sender), &sender_size);
            if (received < 0)
            {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                return -errno;
            }
            const auto& header = response.header;
            if (received < static_cast<ssize_t>(sizeof(ServiceHeader)) ||
                sender.sin_family != AF_INET || sender.sin_addr.s_addr != target.sin_addr.s_addr ||
                sender.sin_port != target.sin_port || !detail::ValidHeader(header, ServicePacketType::RESPONSE) ||
                header.client_id != client_id_ || header.request_id != request_id_ || header.service_id != id ||
                header.payload_size > SERVICE_MAX_PAYLOAD ||
                received != static_cast<ssize_t>(sizeof(ServiceHeader) + header.payload_size)) continue;
            if (header.framework_status < 0)
            {
                if (header.payload_size == 0) return header.framework_status;
                continue;
            }
            if (header.framework_status != 0) continue;
            if (header.payload_size != output_size) return -EMSGSIZE;
            std::memcpy(output, response.payload.data(), output_size);
            return 0;
        }
    }
    return -ETIMEDOUT;
}

void ServiceClient::Close()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ >= 0) { close(fd_); fd_ = -1; }
}
} // namespace kcf
