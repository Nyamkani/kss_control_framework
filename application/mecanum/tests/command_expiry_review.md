# Continuous valid input: stale applied-command expiry fix

## Reproduction and exact cause

The existing PTY matrix was rerun with unchanged 250 ms command freshness,
100 ms UART transaction deadline, nominal 15 ms Loop and constant RPM input.
Each live-input case lasted about 3 seconds. All 0/20 ms reply-delay cases had
zero live stop commands. At 80 ms delay, 10/50/100 Hz each reproduced **9** live
zero commands (the prior run had 9/8/9; counts depend on scheduling).
All 27 zeros were the old `ApplyCommand(stop_only=true)` path:

```cpp
if (applied_expired) { target = {}; fresh = false; }
```

It ran after selecting a new, valid nonzero snapshot, then overwrote that
selection based on the expiry of an older UART-transmitted snapshot. This is
an implementation defect, not evidence that UART throughput requires a stop.

One actual 10 Hz / 80 ms event (CLOCK_MONOTONIC nanoseconds):

| Event | Absolute ns | ms relative to selected generation |
| --- | ---: | ---: |
| Selected/latest command generated | 361477827157639 | 0 |
| Callback copied that command | 361477827251685 | 0.094046 |
| Loop copied snapshot | 361477858787017 | 31.629378 |
| Freshness checked | 361477858794819 | 31.637180 |
| Zero UART write began | 361477858871412 | 31.713773 |
| Older transmitted command expiry | 361477776892883 | -50.264756 |

The selected generation was also the latest callback generation. Its real
age was only 31.637 ms, but the previous applied deadline was 81.902 ms past.
The callback timestamp is when its small snapshot is stored under the existing
mutex; it is diagnostic only, never a freshness deadline.

A second analogous path used the pre-ACK snapshot after a blocking ACK wait.
It did not account for new input arriving during that wait. Dedicated tests
now cover same/changed RPM, explicit zero, invalid input and producer loss
arriving (or not arriving) while the old selected command expires during ACK.

## Minimal behavior change

- stop-only checks decide from the latest valid snapshot. Expiry of the older
  transmitted command may still force a refresh in a normal command slot, but
  cannot turn a newer valid nonzero input into zero.
- After a successful nonzero command ACK, re-enter the stop-only check with a
  newly copied snapshot. A real expiry, invalid input or explicit stop can add
  one zero transaction; another nonzero transaction cannot recurse.
- Generation timestamps, 250/100 ms deadlines and the original wire format are
  unchanged. The latest received timestamp is not used to extend command life.
- Nonzero updates still wait through the reserved sensor slot. Callback UART
  I/O remains absent and all UART access remains on the lifecycle thread.
- P1 classification, protocol_fault and STOP UNCONFIRMED paths are unchanged.

The optional private test observer records selected generation, selected
callback receipt, selection/check times, current latest callback snapshot,
previous applied deadline and decision. Tests independently wrap successful
UART writes and record complete validated m receipt. These timestamps are
not conflated; no synchronous logs or allocations are added to the callback.
The production observer is null.

## Before/after intervals

Pairs are mean / maximum milliseconds, consecutive events only. Command RX
means complete validated m at the emulator; Encoder/RPM RX means query receipt,
not a physical sensor acquisition time. Before command intervals below use
NONZERO m only, so intervening forced zero frames cannot hide motion gaps.

| Delay | Hz | Zero before → after | Nonzero command RX before | Command RX after | Encoder query RX after | RPM query RX after |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 10 | 0 → 0 | 106.631 / 120.939 | 106.625 / 120.714 | 34.966 / 47.820 | 34.810 / 46.873 |
| 0 | 50 | 0 → 0 | 106.687 / 122.829 | 105.531 / 115.709 | 35.118 / 47.512 | 34.820 / 46.054 |
| 0 | 100 | 0 → 0 | 106.068 / 119.609 | 106.081 / 119.825 | 34.943 / 45.594 | 34.816 / 46.788 |
| 20 | 10 | 0 → 0 | 137.360 / 140.868 | 137.807 / 143.247 | 55.277 / 72.997 | 55.010 / 71.742 |
| 20 | 50 | 0 → 0 | 137.004 / 143.624 | 137.426 / 145.567 | 55.142 / 71.947 | 54.716 / 71.855 |
| 20 | 100 | 0 → 0 | 137.320 / 143.580 | 137.292 / 140.933 | 55.035 / 72.836 | 54.619 / 71.593 |
| 80 | 10 | 9 → 0 | 325.025 / 344.046 | 243.322 / 256.439 | 258.457 / 339.217 | 250.810 / 255.324 |
| 80 | 50 | 9 → 0 | 332.844 / 337.031 | 249.600 / 252.001 | 249.573 / 252.109 | 249.494 / 251.466 |
| 80 | 100 | 9 → 0 | 331.961 / 334.585 | 249.397 / 252.066 | 249.371 / 252.563 | 249.488 / 253.159 |

Generation, Host TX and emulator RX remain separate observations. Send age is
Host write start minus the ORIGINAL selected generation timestamp.

| Delay | Hz | Generation interval mean/max | Host TX interval mean/max | Max live send age | Max send age including producer-loss tail | Expiry → zero RX |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 10 | 100.014 / 102.451 | 106.667 / 119.796 | 100.506 | 210.337 | 14.731 |
| 0 | 50 | 20.004 / 22.020 | 105.525 / 118.611 | 19.944 | 200.951 | 6.029 |
| 0 | 100 | 10.001 / 12.737 | 106.078 / 119.799 | 10.795 | 201.377 | 5.257 |
| 20 | 10 | 100.013 / 101.889 | 137.796 / 143.367 | 95.819 | 100.597 | 4.116 |
| 20 | 50 | 20.002 / 22.018 | 137.418 / 145.533 | 19.411 | 174.407 | 3.496 |
| 20 | 100 | 10.001 / 11.652 | 137.292 / 141.822 | 10.288 | 201.566 | 15.040 |
| 80 | 10 | 100.025 / 101.147 | 243.286 / 254.750 | 82.967 | 99.972 | 32.701 |
| 80 | 50 | 20.002 / 21.833 | 249.569 / 252.072 | 21.281 | 222.073 | 57.648 |
| 80 | 100 | 10.004 / 12.861 | 249.415 / 252.241 | 9.647 | 207.456 | 42.022 |

All nine cases assert zero unnecessary live stop commands, both sensor streams
progress, every nonzero TX starts before its selected generation + 250 ms, and
one observed selection matches each UART command. Maximum live send age was
100.506 ms; including the producer-loss tail it was 222.073 ms.

After the live phase, the producer actually stops publishing through the last
generation's expiry + 150 ms. Each case requests zero before that interval ends.
Then the exact expired sample is replayed until expiry + 350 ms: none causes
nonzero transmission. Measured expiry-to-zero receipt was 3.496–57.648 ms;
nonzero TX count after final expiry was zero in every case.

## Verification and scope

Motor executable and tests built successfully. The complete Motor regression,
including P1 post-send faults, delayed stale ACK/STOP UNCONFIRMED, explicit stop
priority, encoder/RPM expiry, continuous changed commands, sensor fairness and
no fabricated sensor/odometry timestamps, passed. New ACK-reselection scenarios
passed. No firmware, KCF Core, IMU or EKF code changed; no physical device was
opened, firmware uploaded, commit made or push performed.

```sh
cmake --build /tmp/kcf-mecanum-motor-build --target kcf_mecanum_motor mecanum_motor_tests -j2
/tmp/kcf-mecanum-motor-build/application/mecanum/mecanum_motor_tests --watchdog-intervals
/tmp/kcf-mecanum-motor-build/application/mecanum/mecanum_motor_tests
```

Raw records from this run: `/tmp/mecanum-expiry-before.log`,
`/tmp/mecanum-expiry-after.log`, `/tmp/mecanum-expiry-regression.log`.
DECISION columns: Hz, delay, phase, selection time, check time, selected generation,
selected callback receipt, latest generation, latest callback receipt, previous
applied expiry, fresh, stop_only, zero. STAMP records generation separately, or
m write start/completion, emulator validation time and zero flag.

This is a short Linux/PTY observation, not a hard real-time bound. The 80 ms
load still yields roughly 243–250 ms mean nonzero command spacing and coalesces
intermediate input. Valid newly generated snapshots authorize continued motion;
250 ms is a generation-age limit, not a UART period guarantee. Physical UART,
MCU execution and mechanical stopping remain unverified. No timeout was enlarged.
