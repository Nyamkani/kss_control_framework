#pragma once

namespace kcf
{

class ProcessElement
{
public:
    virtual ~ProcessElement() = default;

    virtual int Setup() = 0;
    virtual void Loop() = 0;

    virtual void Shutdown()
    {
    }
};

} // namespace kcf
