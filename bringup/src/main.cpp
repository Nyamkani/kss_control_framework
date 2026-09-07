#include "bringup/bringup.hpp"

#include <iostream>

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cerr << "Usage: " << argv[0] << " <dummy_executable>" << std::endl;
        return 1;
    }

    bringup::Bringup app;
    if (app.Setup(argv[1]) != 0)
    {
        return 1;
    }

    return app.Run();
}
