#include "dummy/dummy_element.hpp"

#include <iostream>

int DummyElement::Setup()
{
    std::cout << "[Dummy] Setup" << std::endl;
    return 0;
}

void DummyElement::Loop()
{
    std::cout << "[Dummy] Loop" << std::endl;
}

void DummyElement::Shutdown()
{
    std::cout << "[Dummy] Shutdown" << std::endl;
}
