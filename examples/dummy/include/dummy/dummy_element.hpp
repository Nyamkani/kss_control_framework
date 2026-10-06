#pragma once

#include "kcf/process/process_element.hpp"

class DummyElement : public kcf::ProcessElement
{
public:
    int Setup() override;
    int Loop() override;
    void Shutdown() override;
};
