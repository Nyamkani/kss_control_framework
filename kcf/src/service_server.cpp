#include "kcf/service/service_server.hpp"
#include <new>
#include <system_error>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace kcf
{
ServiceServer::~ServiceServer() { Stop(); }

int ServiceServer::Create(std::uint16_t port)
{
    if (fd_ >= 0 || worker_.joinable()) return -EBUSY;
    if (port == 0) return -EINVAL;
    const int socket_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (socket_fd < 0) return -errno;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    // No SO_REUSEADDR/PORT: another server must fail on this endpoint.
    if (bind(socket_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
    {
        const int error = errno;
        close(socket_fd);
        return -error;
    }
    fd_ = socket_fd;
    handlers_.clear();
    for (auto& entry : cache_) entry.valid = false;
    next_cache_ = 0;
    return 0;
}

int ServiceServer::RegisterHandler(std::uint16_t id, Handler handler)
{
    if (fd_ < 0) return -EBADF;
    if (worker_.joinable()) return -EBUSY;
    try
    {
        return handlers_.emplace(id, std::move(handler)).second ? 0 : -EEXIST;
    }
    catch (const std::bad_alloc&) { return -ENOMEM; }
}

int ServiceServer::Start()
{
    if (fd_ < 0) return -EBADF;
    if (worker_.joinable()) return -EBUSY;
    running_.store(true);
    try { worker_ = std::thread(&ServiceServer::Run, this); }
    catch (const std::system_error& error)
    {
        running_.store(false);
        return -error.code().value();
    }
    catch (const std::bad_alloc&) { running_.store(false); return -ENOMEM; }
    return 0;
}

void ServiceServer::Stop()
{
    running_.store(false);
    if (worker_.joinable())
    {
        if (worker_.get_id() == std::this_thread::get_id()) return;
        worker_.join();
    }
    if (fd_ >= 0) { close(fd_); fd_ = -1; }
}

void ServiceServer::Run()
{
    while (running_.load())
    {
        pollfd descriptor{fd_, POLLIN, 0};
        const int ready = poll(&descriptor, 1, 50);
        if (ready < 0) { if (errno == EINTR) continue; break; }
        if (!running_.load()) break;
        if (ready == 0) continue;
        if (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (!(descriptor.revents & POLLIN)) continue;
        detail::ServicePacket request;
        sockaddr_in source{};
        socklen_t length = sizeof(source);
        const auto received = recvfrom(fd_, &request, sizeof(request), MSG_TRUNC,
                                      reinterpret_cast<sockaddr*>(&source), &length);
        if (received < 0)
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
        if (received < static_cast<ssize_t>(sizeof(ServiceHeader)) || source.sin_family != AF_INET ||
            source.sin_addr.s_addr != htonl(INADDR_LOOPBACK) ||
            !detail::ValidHeader(request.header, ServicePacketType::REQUEST) ||
            request.header.framework_status != 0) continue;

        detail::ServicePacket response;
        response.header = request.header;
        response.header.packet_type = ServicePacketType::RESPONSE;
        response.header.payload_size = 0;
        const auto send_response = [&](const detail::ServicePacket& packet)
        {
            // Cache exists before send, including if a send fails or is lost.
            ssize_t sent;
            do
            {
                sent = sendto(fd_, &packet, sizeof(ServiceHeader) + packet.header.payload_size, 0,
                              reinterpret_cast<const sockaddr*>(&source), sizeof(source));
            } while (sent < 0 && errno == EINTR);
        };
        if (request.header.payload_size > SERVICE_MAX_PAYLOAD ||
            received != static_cast<ssize_t>(sizeof(ServiceHeader) + request.header.payload_size))
        {
            response.header.framework_status = -EMSGSIZE;
            send_response(response);
            continue;
        }

        const Cached* duplicate = nullptr;
        for (const auto& entry : cache_)
        {
            if (entry.valid && entry.address == source.sin_addr.s_addr && entry.port == source.sin_port &&
                entry.request.header.client_id == request.header.client_id &&
                entry.request.header.request_id == request.header.request_id &&
                entry.request.header.service_id == request.header.service_id)
            { duplicate = &entry; break; }
        }
        if (duplicate)
        {
            if (duplicate->request.header.payload_size == request.header.payload_size &&
                std::memcmp(duplicate->request.payload.data(), request.payload.data(), request.header.payload_size) == 0)
                send_response(duplicate->response);
            else
            {
                response.header.framework_status = -EINVAL; // ID reused with different data
                send_response(response);
            }
            continue;
        }

        const auto handler = handlers_.find(request.header.service_id);
        if (handler == handlers_.end()) response.header.framework_status = -ENOENT;
        else if (handler->second.request_size != request.header.payload_size)
            response.header.framework_status = -EMSGSIZE;
        else
        {
            try
            {
                handler->second.invoke(request.payload.data(), response.payload.data());
                response.header.payload_size = static_cast<std::uint32_t>(handler->second.response_size);
            }
            catch (...) { response.header.framework_status = -EFAULT; }
        }
        auto& entry = cache_[next_cache_];
        entry.request = request;
        entry.response = response;
        entry.address = source.sin_addr.s_addr;
        entry.port = source.sin_port;
        entry.valid = true;
        next_cache_ = (next_cache_ + 1) % cache_.size();
        send_response(response);
    }
    running_.store(false);
}
} // namespace kcf
