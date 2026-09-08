#include "bringup/bringup.hpp"
#include <iostream>
#include <charconv>

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cerr << "Usage: " << argv[0] << " <executable> [args...] [--next <executable> [args...]]" << std::endl;
        return 1;
    }
    std::vector<bringup::ElementSpec> elements;
    for (int i = 1; i < argc;)
    {
        if (std::string(argv[i]) == "--next") return 1;
        bringup::ElementSpec spec{"element" + std::to_string(elements.size() + 1), argv[i++], {}};
        while (i < argc && std::string(argv[i]) != "--next") spec.arguments.emplace_back(argv[i++]);
        for (std::size_t j=0; j<spec.arguments.size();)
        {
            auto& option=spec.arguments[j];
            if (option!="--startup-timeout-ms" && option!="--health-timeout-ms" && option!="--shutdown-timeout-ms") { ++j; continue; }
            if (j+1>=spec.arguments.size()) return 1;
            std::uint32_t value=0;
            const auto& text=spec.arguments[j+1];
            const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
            if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() || !value) return 1;
            if (option=="--startup-timeout-ms") spec.startup_timeout_ms=value;
            else if (option=="--health-timeout-ms") spec.health_timeout_ms=value;
            else spec.shutdown_timeout_ms=value;
            spec.arguments.erase(spec.arguments.begin()+j,spec.arguments.begin()+j+2);
        }
        for (std::size_t j = 0; j + 1 < spec.arguments.size(); ++j)
            if (spec.arguments[j] == "--element-name") spec.name = spec.arguments[j + 1];
        elements.push_back(std::move(spec));
        if (i < argc && ++i == argc) return 1;
    }
    bringup::Bringup app;
    if (app.Setup(std::move(elements)) != 0) return 1;
    return app.Run();
}
