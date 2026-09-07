#pragma once

#include "kcf/ipc/shared_channel.hpp"

namespace kcf
{

template <typename T>
class Publisher
{
public:
    int Create(const std::string& name) { return channel_.Create(name); }
    int Publish(const T& value) { return channel_.Publish(value); }
    int Close() { return channel_.Close(); }
    int Unlink() { return channel_.Unlink(); }

private:
    SharedChannel<T> channel_;
};

} // namespace kcf
