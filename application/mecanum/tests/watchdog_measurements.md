# Watchdog build and receive interval measurements — 2026-09-20

## AVR compilation (no upload)

Existing tools: Arduino Builder 1.3.25, AVR GCC 7.3.0, Debian Arduino AVR Boards
1.8.6. Existing IDE preferences select `arduino:avr:uno` (ATmega328P, 16 MHz).
This establishes the software build target, not physical board identification.
No tools, cores or libraries were installed or modified.

The local ROS2 `Mecanum_arduino` sketch, PID/command headers and 16 required
library files were compared with GitHub `Nyamkani/kss_mecanum_ros/v2.0` and match
exactly. Dependencies: QGPMaker MotorShield/Encoder, Adafruit_MS_PWMServoDriver,
PinChangeInterrupt (including numbered units and settings/board/pin headers),
PS2X_lib, AVR Core/Wire/util/atomic. The overlay retains the original PID and
command headers. Dependencies were staged in `/tmp/mecanum-watchdog-avr/`, not
installed or copied into the source repositories. Test doubles were NOT used
for AVR compilation.

| Sketch | Flash / 32256 B | SRAM / 2048 B | Result |
| --- | ---: | ---: | --- |
| v2.0 baseline with the same required dependencies | 12444 | 971 | PASS |
| Porting overlay, compile-only `MECANUM_MAX_ABS_RPM=1.0f` | 12688 | 971 | PASS |
| Porting overlay, no RPM setting | — | — | Expected configuration error |

**1.0f is a compile-only placeholder, not an approved robot RPM limit.** The
production value remains unset. PWM saturation ±255, PI gains Kp=5/Ki=5/Kd=0,
4320 encoder counts/revolution and an 80 Hz PID loop do not establish a safe
maximum RPM. Sketch comments suggest roughly 120 RPM at PWM 250, but do not
specify supply voltage, load, thermal duty, motor/gearbox ratings or all-wheel
repeatability. Neither those comments nor the synthetic test limit 120 justify
an operational maximum.

The installed builder command (using the isolated staged sketch) was:

```sh
arduino-builder -compile \
  -hardware /usr/share/arduino/hardware \
  -tools /usr/share/arduino/hardware/tools \
  -fqbn arduino:avr:uno -core-api-version 10819 \
  -prefs compiler.cpp.extra_flags=-DMECANUM_MAX_ABS_RPM=1.0f \
  -build-path /tmp/mecanum-watchdog-avr/build-ported \
  -build-cache /tmp/mecanum-watchdog-avr/cache \
  /tmp/mecanum-watchdog-avr/ported/Mecanum4WD/Mecanum4WD.ino
```

Omitting the `-prefs` flag in a separate build directory produces the expected
`Define MECANUM_MAX_ABS_RPM` error. Neither executable nor HEX was uploaded.

## Measurement definitions

`mecanum_motor_tests --watchdog-intervals`, nine cases, about 3 seconds of live
input per case. Constant body vx=0.05 m/s, vy=wz=0; only generated timestamp and
sequence change. Latest-value Topic, freshness 250 ms, UART transaction deadline
100 ms, nominal Loop interval 15 ms, alternating encoder/RPM queries retained.
The emulator's synthetic range check is ±120 RPM; all measured nonzero targets
are about 11.65 RPM in magnitude. This synthetic range is not a robot setting.

- GEN: command creation CLOCK_MONOTONIC timestamp, before Topic Publish.
- TX: start of first successful host write contributing to a complete `m` frame;
  trace also records completion of its final write. Test-only linker wrapping
  leaves the production UART code unchanged.
- RX: PTY emulator time after the complete CR-delimited frame is parsed and its
  four finite, in-range RPM arguments are validated, before the ACK delay.
- Delay: added to EVERY motor ACK and encoder/RPM response, not to the creation
  thread and not a simulated baud-rate wire delay.
- Mean/max are consecutive timestamp differences, excluding setup/shutdown and
  artificial start/end-window gaps. Raw `STAMP` lines record GEN and TX/RX
  separately. TX/RX frames are checked one-to-one in FIFO order.
- This Linux/PTY run is not isolated real-time measurement. The regression suite
  overlapped part of the run. Instrumentation and scheduling contribute latency;
  PTY does not simulate physical UART serialization, MCU parsing/PID or I2C time.

All intervals below are milliseconds; pairs are mean / maximum.

| Reply delay | Input Hz | GEN | TX (all valid m) | RX (all valid m) | Live zero m count |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 10 | 100.099 / 102.538 | 106.074 / 120.225 | 106.050 / 119.951 | 0 |
| 0 | 50 | 20.002 / 22.881 | 106.080 / 120.114 | 106.087 / 119.152 | 0 |
| 0 | 100 | 10.002 / 12.348 | 106.670 / 119.914 | 106.668 / 119.792 | 0 |
| 20 | 10 | 100.005 / 102.433 | 136.445 / 140.121 | 136.435 / 140.183 | 0 |
| 20 | 50 | 20.005 / 20.852 | 136.440 / 141.277 | 136.435 / 141.684 | 0 |
| 20 | 100 | 10.002 / 12.540 | 138.078 / 142.619 | 138.071 / 142.724 | 0 |
| 80 | 10 | 100.016 / 101.116 | 171.567 / 251.213 | 171.539 / 250.870 | 9 |
| 80 | 50 | 20.004 / 21.626 | 171.086 / 250.745 | 171.072 / 250.440 | 8 |
| 80 | 100 | 10.001 / 12.649 | 171.746 / 254.860 | 171.565 / 254.368 | 9 |

All valid m includes zero targets, since zero also renews the MCU timer.
At 80 ms delay the Host expires an already applied command while querying a
sensor, even while newer same-RPM commands arrive. Stop priority causes repeated
zero commands. This is an observed continuity limitation, not sensor starvation
or an MCU watchdog trip. Increasing the MCU timeout does not resolve it.

| Reply delay | Hz | Nonzero RX mean / max | Encoder RX mean / max | RPM RX mean / max | Final expiry to zero RX |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 10 | 106.050 / 119.951 | 34.946 / 45.819 | 34.823 / 47.808 | 5.567 |
| 0 | 50 | 106.087 / 119.152 | 34.946 / 48.205 | 34.827 / 46.442 | 10.140 |
| 0 | 100 | 106.668 / 119.792 | 34.949 / 46.225 | 34.760 / 47.180 | 10.138 |
| 20 | 10 | 136.435 / 140.183 | 54.371 / 71.219 | 54.371 / 70.336 | 8.624 |
| 20 | 50 | 136.435 / 141.684 | 54.734 / 71.222 | 54.297 / 72.113 | 9.444 |
| 20 | 100 | 138.071 / 142.724 | 55.446 / 71.351 | 55.117 / 71.324 | 15.907 |
| 80 | 10 | 333.162 / 335.113 | 324.095 / 338.232 | 333.183 / 334.738 | 33.009 |
| 80 | 50 | 323.136 / 334.825 | 313.986 / 335.826 | 321.968 / 334.294 | 52.117 |
| 80 | 100 | 333.680 / 341.552 | 324.407 / 338.501 | 333.706 / 338.472 | 49.381 |

After stopping each producer, the test repeatedly republishes its exact last
sample, preserving its generation timestamp, for at least 350 ms after expiry.
Every case had zero nonzero TX starts at/after that sample's 250 ms deadline;
the zero command arrived 5.567–52.117 ms after expiry. This is a measured request
latency, not a physical stopping time. Both sensor streams progressed in every
case. No Topic Publish attempts were skipped in this run; this does not promise
every generated command is delivered to the MCU. Nonzero motion renewals are
intentionally coalesced. Maximum observed TX-start to RX-validation delay was
4.657 ms in this PTY environment.

Motor build, existing PTY regression (including post-send uncertainty, delayed
ACK/STOP UNCONFIRMED and timeout cases), and native firmware tests passed.

## Watchdog proposal — no timeout changed

A **500 ms initial hardware-test candidate**, not an approved setting: the
largest observed valid-m gap was 254.368 ms, leaving about 245.6 ms additional
budget. This is a rounded value near twice the observed maximum, not a proven
worst-case bound. Normal 0/20 ms response cases had maxima below 143 ms. The
80 ms condition must also be investigated for intermittent Host expiry stops.

The structural budget must include previous command receive-to-ACK latency,
the Host's 100 ms refresh threshold measured from ACK completion, sensor-slot
blocking, subsequent delivery and Linux/MCU scheduling jitter. A 100 ms
transaction deadline is not an end-to-end receive-interval bound. If measured
hardware maxima plus an agreed margin exceed the candidate, revise the timing
budget/scheduling rather than treating this short PTY run as certification.

With no further accepted command, MCU output-zero is requested after W plus
watchdog check latency, assuming its Loop keeps running. Relative to Host death,
late buffered commands can add delay; mechanical coast/braking adds more.
Validate cable removal, SIGKILL, directional link faults, reboot and queued
commands with UART/PWM/RPM measurements. Confirm actual board, motor/gearbox and
supply/load ratings, RPM limit and I2C/Serial blocking. A stalled MCU Loop is not
protected by this software timer. The source remains at **10,000 ms**.
