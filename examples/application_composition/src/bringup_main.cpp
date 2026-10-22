#include "composition/common.hpp"
#include "bringup/bringup.hpp"
#include <filesystem>

int main(int argc, char** argv) {
    try {
        const auto instance = composition::Instance(argc, argv);
        const auto directory = std::filesystem::read_symlink("/proc/self/exe").parent_path();
        bringup::Bringup supervisor;
        const int code = supervisor.Setup("composition_" + instance, {
            {"source", (directory / "kcf_composition_source").string(), {instance}},
            {"processor", (directory / "kcf_composition_processor").string(), {instance}}
        }); // ExecutionProfile and all supervisor policies retain existing defaults.
        if (code) { std::cerr << "[Composition] Setup error=" << code << std::endl; return 1; }
        return supervisor.Run();
    } catch (const std::exception& e) { std::cerr << e.what() << std::endl; return 2; }
}
