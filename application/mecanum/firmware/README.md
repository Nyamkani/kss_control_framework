# Mecanum watchdog firmware overlay

Base: `Nyamkani/kss_mecanum_ros`, branch `v2.0`, directory `Mecanum_arduino/`.
Source retrieved for this change on 2026-09-20. `pid_controller.h` and
`commands.h` are unchanged copies; `Mecanum4WD.ino` contains the watchdog changes.
The source repository/worktree was not modified.

This is an overlay, not a standalone Arduino library bundle. Copy the upstream
sketch directory and its bundled dependencies to a separate porting sketch named
`Mecanum4WD`, then overlay these four files. Retain upstream licenses. The motor
shield, encoder, pin-change interrupt, PWM and PS2 libraries are the original
v2.0 dependencies; no dependency implementation is replaced here.

Define `MECANUM_MAX_ABS_RPM` in the firmware compiler flags using a verified wheel
RPM limit. There is deliberately no default: the original has no RPM limit,
and the PWM limit 255 is not an RPM limit. The host test's 120 is synthetic and
must not be treated as a validated robot setting. Missing, nonpositive or
nonfinite limits must fail compilation.

The ASCII m/e/z/r requests, CR delimiters, OK/Invalid Command responses and
10,000 ms timeout are preserved. Valid finite in-range motor commands (including
zero and unchanged RPM) renew the timer; invalid commands and sensor queries do
not. Timeout, startup and zero commands share stopMotors: disable PID actuation,
request zero motor output, then call the existing resetPID once. After a timeout,
a new valid motor command may resume motion, as before; this is not a latched
emergency stop and the protocol cannot identify delayed stale commands.

Serial processing handles at most 64 bytes and one frame per invocation. The
watchdog is checked before/after Serial processing, within byte processing, and
after PID processing. These limits do not bound a stalled Serial write, I2C
operation, disabled interrupts or a stopped MCU Loop.

Software tests compile the actual sketch and unchanged PID code against hardware
API doubles. They verify parsing, output requests, PID reset, expiry/resumption,
millis wrap and bounded Serial draining. They do not compile for an AVR board,
verify physical PWM/I2C delivery, or establish mechanical stopping time.

With `MECANUM_BUILD_TESTS=ON`, build/run the `mecanum_firmware_tests` target.
Its 120 RPM definition is confined to that native test target. Production builds
must provide their own limit; no board was flashed or physically exercised.
