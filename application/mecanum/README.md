# KCF mecanum Motor Element

KCF v5.1 / Linux / C++17 standalone Motor Element. No ROS2/ament dependency.
Only this application and the root CMake entry are added.

## Build

```sh
cmake -S . -B /tmp/kcf-mecanum-build -DMECANUM_BUILD_TESTS=ON
cmake --build /tmp/kcf-mecanum-build --target kcf_mecanum_motor mecanum_motor_tests mecanum_firmware_tests -j2
/tmp/kcf-mecanum-build/application/mecanum/mecanum_motor_tests
/tmp/kcf-mecanum-build/application/mecanum/mecanum_firmware_tests
# Same-RPM generation/write/validated-receive timestamps, 0/20/80 ms replies:
/tmp/kcf-mecanum-build/application/mecanum/mecanum_motor_tests --watchdog-intervals
```

Tests use synthetic encoders and a pseudo-terminal Arduino emulator, never a physical UART.
The interval mode traces writes only in the test executable (`--wrap=write`);
the production driver and its error/stop policies are unchanged. See
[watchdog measurement results](tests/watchdog_measurements.md) for the AVR build,
measurement definitions and the historical expiry stops under 80 ms reply delay.
The cause, correction and current results are in
[continuous-command expiry review](tests/command_expiry_review.md).
The executable requires an explicit device; it does not enumerate or open hardware by default.
A missing device produces a nonzero initialization failure.

```sh
# Usage only, does not open UART:
/tmp/kcf-mecanum-build/application/mecanum/kcf_mecanum_motor --help
```

Actual hardware execution is a separate validation step. Options:
`--serial DEVICE`, `--baud 115200`, `--command-timeout-ms 250`,
`--topic-prefix /mecanum`. Original serial default was /dev/ttyUSB1;
the port must now be selected explicitly. Baud default is unchanged.
The loop target remains 15 ms; UART latency can lengthen it.

## Topics and contracts

| Topic | Payload | Owner / depth |
| --- | --- | --- |
| /mecanum/cmd_vel | CmdVel | External command publisher must Create(name, 1); Element rejects other depths |
| /mecanum/wheel_state | WheelState | Motor Element / 1 |
| /mecanum/wheel_odometry | WheelOdometry | Motor Element / 1 |

Headers in `include/mecanum/messages.hpp` include v1 descriptors for KCF Tool,
with fixed primitive arrays, standard layout and trivial POD payloads.
The prefix is configurable to avoid global Topic name collisions.

- All wheel arrays: FL, FR, RL, RR = Arduino 1, 4, 2, 3.
- Encoder counts and RPM retain the MCU's raw sign. MOTOR_SIGN = -1 is
  applied by kinematics, not silently applied to raw Wheel State.
- Radius 41 mm, wheel base 190 mm, separation 205 mm, CPR 4320, roller angle
  implicit in the original 45-degree equations: unchanged.
- cmd_vel: body vx/vy [m/s], wz [rad/s]. Odometry: x/y [m], yaw [rad] in
  odom; velocity in base_footprint, +x forward, +y left, +yaw CCW.
- All times are CLOCK_MONOTONIC nanoseconds from the same host/boot.
  Command producers must set generated_at_ns at generation, not replay old times.
  No wall-clock or remote-host timestamps.
- Commands created before this lifecycle became ready, future, zero-timestamp,
  expired and nonfinite commands produce zero targets. Age must be strictly
  less than the configured timeout. No velocity clipping is introduced.
  Nonrepresentable RPM or commands exceeding the unchanged 64-byte MCU frame
  capacity are rejected and replaced by zero.
- Callback copies only CmdVel under a mutex. Lifecycle thread serializes all UART
  access. Commands are rechecked around sensor transactions; no command backlog.
- Wheel State encoder/rpm timestamps are separate host receipt times, not MCU
  acquisition times. `encoder_valid` means a parsed frame, not valid odometry.
  `command_acknowledged` means only a protocol ACK for the requested target.
- Encoder wrap-around, 200 RPM x 1.5 plausibility gate, three-invalid-frame
  baseline resync and original Euler integration are retained.
  Rejected deltas publish invalid Odometry with retained pose and zero twist;
  pose_timestamp_ns retains the last accepted pose time. No covariance is claimed.

## Lifecycle and stop behavior

Setup opens UART, waits the original combined 2.5-second boot allowance, requests
zero RPM and requires ACK, resets encoders with r, reads the baseline, creates
output Topics and attaches the command Topic if available. With no producer,
it remains at zero and retries attachment in Loop. Output Topics are unlinked
only by their owner. A producer recreated after attachment requires restarting
the Motor Element: KCF Subscriber does not automatically reconnect; the old
command still expires and produces zero.

Before the first command, Setup consumes only complete CRLF `Setup Complete!`
boot lines (and empty lines), requiring 20 ms quiet within a 100 ms total budget.
Unexpected `OK`, unknown lines, incomplete frames and startup floods fail Setup.
It does not flush after this boundary or skip boot lines during a transaction:
a reboot banner after transmission remains a SendAttempted protocol failure.
The quiet window is not proof that the MCU cannot reboot later.

Each Loop sends the latest valid target (or zero), reads e/z and updates
odometry. Reads, writes and ACK waits have a 100 ms transaction deadline.
Writes are nonblocking and handle partial writes/EINTR; command send deadlines
also respect expiration. A blocking ACK/sensor transaction can delay the next
zero request by up to a transaction plus scheduling latency; this is not a
hard real-time stop guarantee.

UART failure is fatal to this lifecycle. Shutdown requests zero before closing
UART/Topics. After ambiguous transport errors, it terminates any partial frame
and attempts a zero command, but reports STOP UNCONFIRMED and failure, even if
an old ACK appears. The ASCII protocol has no transaction identifiers. Normal
zero ACK also does not prove mechanical stop. SIGKILL/host failure cannot run
Shutdown. The wire protocol and 10-second MCU timeout are unchanged; the firmware
overlay in [firmware/README.md](firmware/README.md) adds common stop handling,
validated RPM commands and bounded Serial input work. It still needs MCU build
and hardware validation, and a verified maximum RPM supplied at firmware build.

Command results carry both an errno and a send phase. Only an explicit
NotStarted result for size/expiry rejection permits the normal zero fallback.
Once SendData is entered, any failure is conservatively SendAttempted, including
partial writes, oversized replies, malformed ACKs and timeouts. It latches the
Element's protocol fault and ends the lifecycle. A delayed previous ACK arriving
after the best-effort stop flush still cannot turn STOP UNCONFIRMED into success.
PTY tests cover these cases without changing the wire protocol.

## Source provenance

Adapted from Nyamkani/kss_mecanum_ros, branch v2.0:
`mecanum_hardwares/packages/arduino_motor_driver/arduino_motor_driver_ros2_jazzy/`
(src/arduino_motor_driver.cpp, serial_uart.cpp, mecanum_motor_controller.cpp
and associated headers). See LICENSE (upstream Apache-2.0).
Active UART framing/parsing and kinematic/odometry equations are preserved;
ROS interfaces and unused queue paths are replaced with this Element. Transport
deadline/error/cleanup changes support explicit stop and freshness requirements.
Mecanum_arduino/Mecanum4WD.ino, commands.h, pid_controller.h and
QGPMaker_MotorShield.cpp/.h were additionally consulted for the wire protocol
and actual stop/PID behavior. The firmware overlay retains unchanged PID code.

Pending hardware verification: device permissions/identity, 115200 UART and ACK
latency, physical wheel mapping/sign, encoder scale, straight/lateral/rotation
odometry, actual loop jitter, disconnect/reboot behavior and measured stop latency.
