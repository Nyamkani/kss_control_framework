#include "kcf/service/service_server.hpp"
#include "service/test_service.hpp"
#include <charconv>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <signal.h>

namespace
{
volatile sig_atomic_t stop_requested = 0;
void HandleStop(int) { stop_requested = 1; }
}

int main(int argc, char* argv[])
{
    if (argc != 2) { std::cerr << "Usage: " << argv[0] << " <port>" << std::endl; return 1; }
    std::uint16_t port = 0;
    const std::string_view input(argv[1]);
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), port);
    if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() || port == 0) return 1;
    struct sigaction action {};
    action.sa_handler = HandleStop;
    if (sigemptyset(&action.sa_mask) != 0 || sigaction(SIGINT, &action, nullptr) != 0 ||
        sigaction(SIGTERM, &action, nullptr) != 0) return 1;

    std::int32_t stored = 0;
    std::uint32_t executions = 0;
    kcf::ServiceServer server;
    const int created = server.Create(port);
    if (created != 0) { std::cerr << "[ServiceServer] Create failed: " << created << std::endl; return 1; }
    if (server.Register<AddRequest, AddResponse>(SERVICE_ADD, [](const auto& request, auto& response)
    {
        const std::int64_t sum = static_cast<std::int64_t>(request.a) + request.b;
        if (sum < std::numeric_limits<std::int32_t>::min() || sum > std::numeric_limits<std::int32_t>::max())
            throw std::overflow_error("Add result overflow");
        response.result = static_cast<std::int32_t>(sum);
    }) != 0) return 1;
    if (server.Register<SetValueRequest, SetValueResponse>(SERVICE_SET_VALUE, [&](const auto& request, auto& response)
    {
        stored = request.value;
        response = {stored, ++executions};
    }) != 0) return 1;
    if (server.Start() != 0) return 1;
    std::cout << "[ServiceServer] listening 127.0.0.1:" << port << " services=1,2" << std::endl;
    while (!stop_requested) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    server.Stop();
    std::cout << "[ServiceServer] stopped executions=" << executions << std::endl;
    return 0;
}
