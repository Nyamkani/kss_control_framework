#pragma once
#include <algorithm>
#include <cstdint>
constexpr int FORWARD=1, BACKWARD=2, RELEASE=4;
// Hardware API double; physical PWM/I2C delivery is not tested here.
struct QGPMaker_DCMotor {
    int requested=0, pwm=0;
    unsigned writes=0;
    void setMotorSpeed(int16_t v) { requested=std::clamp<int>(v,-255,255); }
    void motorRun() { pwm=requested; ++writes; }
    void setSpeed(uint8_t v) { requested=v; motorRun(); }
    void run(int) {}
};
struct QGPMaker_MotorShield {
    QGPMaker_DCMotor motors[4];
    void begin(int) {}
    QGPMaker_DCMotor* getMotor(int n) { return &motors[n-1]; }
};
