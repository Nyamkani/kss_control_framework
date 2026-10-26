#include "bringup/bringup.hpp"
#include <iostream>
#include <charconv>

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cerr << "Usage: " << argv[0] << " [--application-name NAME] <executable> [args...] [--element-name NAME] [--next <executable> [args...] [--element-name NAME]]" << std::endl;
        return 1;
    }
    int first = 1;
    std::string application_name;
    // Only a leading option is Supervisor metadata; child args keep old syntax.
    if (std::string(argv[first]) == "--application-name")
    {
        if (argc < 4) return 1;
        application_name = argv[first + 1]; first += 2;
    }
    std::vector<bringup::ElementSpec> elements;
    for (int i = first; i < argc;)
    {
        if (std::string(argv[i]) == "--next") return 1;
        bringup::ElementSpec spec{"element" + std::to_string(elements.size() + 1), argv[i++], {}};
        while (i < argc && std::string(argv[i]) != "--next") spec.arguments.emplace_back(argv[i++]);
        bool named=false;
        for (std::size_t j=0; j<spec.arguments.size();)
        {
            auto& option=spec.arguments[j];
            if(option=="--element-name") {
                if(named || j+1>=spec.arguments.size() || spec.arguments[j+1].empty() || spec.arguments[j+1].rfind("--",0)==0)return 1;
                named=true;spec.name=spec.arguments[j+1];
                spec.arguments.erase(spec.arguments.begin()+j,spec.arguments.begin()+j+2);
                continue;
            }
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
        elements.push_back(std::move(spec));
        if (i < argc && ++i == argc) return 1;
    }
    bringup::Bringup app;
    if (app.Setup(application_name, std::move(elements)) != 0) return 1;
    return app.Run();
}
