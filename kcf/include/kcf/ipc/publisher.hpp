#pragma once

#include "kcf/ipc/shared_channel.hpp"
#include "kcf/introspection/detail/endpoint_registry.hpp"

namespace kcf
{

template <typename T>
class Publisher
{
public:
    ~Publisher() { Close(); }
    int Create(const std::string& name, std::uint32_t depth = 1)
    {
        const int result = channel_.Create(name, depth);
        if (result == 0) registration_id_ = detail::RegisterEndpoint(EndpointKind::TOPIC,
            EndpointRole::PUBLISHER, name, sizeof(T), detail::DiagnosticTypeName<T>(), detail::RegisterEndpointType<T>());
        return result;
    }
    int Publish(const T& value) { return channel_.Publish(value); }
    int Close()
    {
        const int result = channel_.Close();
        detail::UnregisterEndpoint(registration_id_); registration_id_ = 0;
        return result;
    }
    int Unlink() { return channel_.Unlink(); }

private:
    SharedChannel<T> channel_;
    std::uint64_t registration_id_{0};
};

} // namespace kcf
