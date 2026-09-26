#pragma once
#include <cstdint>
struct QGPMaker_Encoder {
    int32_t count=0;
    QGPMaker_Encoder(int, int) {}
    int32_t read() { return count; }
    void write(int32_t v) { count=v; }
};
