#pragma once

namespace kcf
{

class ProcessElement
{
public:
    virtual ~ProcessElement() = default;

    // Once Setup is entered, Shutdown runs exactly once, even on failure.
    virtual int Setup() = 0;
    // 0 completes a normal cycle; non-zero is fatal and ends this lifecycle.
    // Prefer negative errno values. Transient recovery belongs to the Element.
    virtual int Loop() = 0;

    virtual void Shutdown()
    {
    }
};

} // namespace kcf
