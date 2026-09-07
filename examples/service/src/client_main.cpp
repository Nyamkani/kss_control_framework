#include "kcf/service/service_client.hpp"
#include "service/test_service.hpp"
#include <cerrno>
#include <charconv>
#include <iostream>
#include <string_view>

int main(int argc, char* argv[])
{
    if (argc < 2 || argc > 3) { std::cerr << "Usage: " << argv[0] << " <port> [sequential_calls: 0..10000]" << std::endl; return 1; }
    unsigned port = 0, calls = 1000;
    for (int i = 1; i < argc; ++i)
    {
        unsigned& value = i == 1 ? port : calls;
        const std::string_view input(argv[i]);
        const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size()) return 1;
    }
    if (port == 0 || port > 65535 || calls > 10000) return 1;
    kcf::ServiceClient client;
    if (client.Create(static_cast<std::uint16_t>(port)) != 0) return 1;
    AddResponse add{};
    int result = client.Call(SERVICE_ADD, AddRequest{10, 20}, add);
    if (result != 0 || add.result != 30) { std::cerr << "Add failed: " << result << std::endl; return 1; }
    std::cout << "Add: " << add.result << std::endl;
    SetValueResponse set{};
    if (client.Call(SERVICE_SET_VALUE, SetValueRequest{123}, set) != 0 || set.stored_value != 123) return 1;
    std::cout << "SetValue: " << set.stored_value << " execution_count=" << set.execution_count << std::endl;
    const auto baseline = set.execution_count;
    if (client.Call(65535, AddRequest{}, add) != -ENOENT) return 1;
    if (client.Call(SERVICE_SET_VALUE, AddRequest{}, set) != -EMSGSIZE) return 1;
    for (unsigned i = 0; i < calls; ++i)
    {
        if (client.Call(SERVICE_SET_VALUE, SetValueRequest{static_cast<std::int32_t>(i)}, set) != 0 ||
            set.stored_value != static_cast<std::int32_t>(i) || set.execution_count != baseline + i + 1) return 1;
    }
    client.Close();
    std::cout << "Unknown service / invalid payload PASS; sequential calls=" << calls << " PASS" << std::endl;
    return 0;
}
