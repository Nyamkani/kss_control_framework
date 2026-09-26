#include <cmath>
#include <iostream>
#include <stdexcept>
#include "Mecanum4WD.ino"

void check(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
void frame(const std::string& bytes) {
    Serial.feed(bytes);
    while (Serial.available()) loop();
}
void stopped() {
    check(!moving, "moving cleared");
    for (auto* p : {&M1PID,&M2PID,&M3PID,&M4PID}) {
        check(p->target_rpm==0 && p->integral==0 && p->output==0 &&
              p->prev_measured_rpm==0, "PID target/integral/output/history cleared");
        check(p->prev_encoder==p->encoder, "encoder baseline reset");
    }
    for (const auto& m : AFMS.motors) check(m.pwm==0 && m.requested==0, "zero output requested");
}
int main() {
  try {
    setup(); stopped();
    check(Serial.tx=="Setup Complete!\r\n", "boot contract");
    Serial.tx.clear(); test_ms=123;
    frame("m 40 -40 20 -20\r");
    check(moving && lastMotorCommand==123 && M2PID.target_rpm==-40, "valid command and watchdog");
    check(Serial.tx=="OK\r\n", "normal ACK unchanged");
    test_ms=150; frame("m 40 -40 20 -20\r");
    check(lastMotorCommand==150, "same target refreshes watchdog");
    for (const auto& bad : {"m nan 0 0 0\r", "m 0 inf 0 0\r", "m 0 0 -inf 0\r",
         "m 0 0 0 121\r", "m -121 0 0 0\r", "m 1e99 0 0 0\r", "m 1 2 3\r",
         "m 1 2 3 4 5\r", "m invalid 0 0 0\r"}) {
        Serial.tx.clear(); test_ms+=1; frame(bad);
        check(lastMotorCommand==150 && M1PID.target_rpm==40 && moving, "invalid cannot renew or replace target");
        check(Serial.tx=="Invalid Command\r\n", "invalid reply contract");
    }
    frame("e\rz\r"); check(lastMotorCommand==150, "queries never refresh watchdog");
    test_ms=200; frame("m 120 -120 120 -120\r");
    check(lastMotorCommand==200 && moving, "inclusive configured limit");
    // Dirty every controller state to prove the actual resetPID clears it.
    for (auto* p : {&M1PID,&M2PID,&M3PID,&M4PID}) {
        p->integral=17; p->output=88; p->prev_measured_rpm=12;
    }
    setMotorSpeeds(88,88,88,88);
    test_ms=10199; checkMotorWatchdog(); check(moving, "before 10 second boundary");
    test_ms=10200; checkMotorWatchdog(); stopped();
    unsigned writes=AFMS.motors[0].writes;
    MotorEnc1.write(10); updatePID(0.02f);
    check(M1PID.measured_rpm!=0 && M1PID.output==0 && M1PID.integral==0 &&
          AFMS.motors[0].writes==writes, "stopped PID measures coast but does not drive");
    test_ms=10201; frame("m 20 20 20 20\r");
    check(moving && M1PID.target_rpm==20 && lastMotorCommand==10201, "valid command resumes after timeout");
    frame("m 0 0 0 0\r"); stopped();
    check(lastMotorCommand==10201, "zero command also refreshes");
    // Wrapped millis subtraction retains a full 10 second lease.
    test_ms=UINT32_MAX-5000U; frame("m 30 30 30 30\r");
    test_ms+=9999U; checkMotorWatchdog(); check(moving, "wrap before expiry");
    ++test_ms; checkMotorWatchdog(); stopped();
    // Flooding cannot monopolize a Loop. Deadline checks occur during draining.
    test_ms=20000; frame("m 30 30 30 30\r");
    test_ms=29990; Serial.feed(std::string(1000,'X')); Serial.read_delay_ms=1;
    const unsigned before=Serial.reads; loop();
    check(Serial.reads-before==64 && Serial.available()>0, "serial byte budget");
    stopped(); Serial.read_delay_ms=0;
    frame("\rm 10 10 10 10\r"); check(moving, "overflow resynchronizes at CR");
    Serial.feed("z\rz\rz\r"); Serial.tx.clear(); processSerial();
    check(Serial.available()==4, "one frame per serial invocation");
    Serial.rx.clear(); test_ms=lastMotorCommand+AUTO_STOP_INTERVAL;
    loop(); stopped();
    std::cout << "PASS: actual sketch/PID with hardware doubles; validation, reset, resume, wrap and serial budget\n";
  } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
