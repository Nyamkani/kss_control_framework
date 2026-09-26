#pragma once
#include <cstdint>
#include <deque>
#include <sstream>
#include <string>
inline uint32_t test_ms = 0;
inline uint32_t millis() { return test_ms; }
inline uint32_t micros() { return test_ms * 1000U; }
struct TestSerial {
    std::deque<char> rx;
    std::string tx;
    unsigned reads = 0;
    uint32_t read_delay_ms = 0;
    void begin(unsigned) {}
    int available() { return static_cast<int>(rx.size()); }
    int read() { ++reads; test_ms += read_delay_ms; char c = rx.front(); rx.pop_front(); return c; }
    void feed(const std::string& s) { for (char c : s) rx.push_back(c); }
    template<class T> void print(T value) { std::ostringstream out; out << value; tx += out.str(); }
    void print(float value, int) { print(value); }
    template<class T> void println(T value) { print(value); tx += "\r\n"; }
    void println(float value, int) { println(value); }
};
inline TestSerial Serial;
