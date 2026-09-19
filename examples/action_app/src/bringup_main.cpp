#include "action_app/count_action.hpp"
#include "bringup/bringup.hpp"
#include <filesystem>
int main(int argc, char** argv)
{
    std::uint16_t port;
    if (!action_app::ParsePort(argc, argv, port)) return 1;
    std::error_code error;
    const auto directory = std::filesystem::read_symlink("/proc/self/exe", error).parent_path();
    if (error) return 1;
    const auto argument = std::to_string(port);
    bringup::Bringup app;
    const int result = app.Setup("action_example", {
        {"action_server", (directory/"kcf_action_server").string(), {argument}},
        {"action_client", (directory/"kcf_action_client").string(), {argument}}
    });
    return result ? result : app.Run();
}
