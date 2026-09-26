# WT901C IMU Element (KCF v5.1)

Source: `Nyamkani/kss_mecanum_ros`, branch `v2.0`,
`mecanum_hardwares/packages/wt901c/wt90c1c/`.
The fetched branch sources, rather than the modified local ROS2 worktree, were
used. Original license is in `vendor/LICENSE` (Apache-2.0).

The original WIT C SDK, register definitions and NORMAL UART frame parser are
retained. SDK modifications are limited to NORMAL framing diagnostics and
replacing overlapping resynchronization memcpy with memmove. UART serial.c is
adapted from the original Linux termios 8N1 implementation: explicit baud errors,
nonblocking reads, bounded polling, errno propagation, setup cleanup, CLOEXEC and
exclusive FD ownership. No calibration commands or sensor settings are sent.
The unused SDK Modbus/CAN paths retain upstream compiler warnings; they are not
used or covered by this port.

## Build / run

```sh
cmake -S . -B /tmp/kcf-mecanum-imu-build -DMECANUM_BUILD_IMU_TESTS=ON
cmake --build /tmp/kcf-mecanum-imu-build --target kcf_mecanum_imu mecanum_imu_tests -j2
/tmp/kcf-mecanum-imu-build/application/mecanum/imu/mecanum_imu_tests
/tmp/kcf-mecanum-imu-build/application/mecanum/imu/kcf_mecanum_imu --help
```

Hardware usage (not executed during this port):

```sh
kcf_mecanum_imu --serial /dev/ttyUSB0 --baud 115200 --topic-prefix /mecanum
```

Device `/dev/ttyUSB0` and 115200 baud are the original defaults. Axis flags
`--invert-x`, `--invert-y`, `--invert-z` default off. KCF ProcessRuntime executes
Setup/Loop/Shutdown with an initial 100 Hz (10 ms) Loop target, not a hard
real-time guarantee. Root CMake adds this subdirectory separately from Motor;
the IMU target does not link Motor support.

## Data contract

Both Topics use fixed-size trivially-copyable standard-layout messages, v1 Tool
descriptors and **Depth=16**. This is a temporary bounded retention queue: a slow
reader can lose overwritten items. Use pull `Subscriber::ReadNext` and inspect
`TopicReadInfo.missed`; callback subscriptions still receive latest snapshots
only. ReadLatest/Tool echo does not prove that all queued samples were consumed.
Each publisher's lifetime has its own application sequence starting at 1.

| Topic suffix (prefix `/mecanum`) | Payload |
| --- | --- |
| `/imu/raw_data` | ImuSample: sequence, timestamp_ns, component_timestamp_ns[3], updated_mask, valid_mask, axis_inversion_mask, acceleration_m_s2[3], angular_velocity_rad_s[3], rpy_rad[3], orientation_xyzw[4] |
| `/imu/mag_raw` | MagSample: sequence, timestamp_ns, valid, axis_inversion_mask, magnetic_raw_counts[3] |

Axes are X/Y/Z and roll/pitch/yaw in the sensor's `imu_link` convention.
Acceleration uses `raw/32768 * 16 * 9.80665` m/s², gyro uses
`raw/32768 * 2000 * pi/180` rad/s, angle uses `raw/32768 * pi` rad.
Gravity is retained; no bias subtraction, gravity removal, covariance estimate,
frame mounting transform or EKF is introduced. The original offsets were zero.

Each inversion flag uses the correct index 0/1/2 and negates the corresponding
acceleration, gyro, Euler angle and magnetic component consistently. Quaternion
XYZW is recomputed from the signed Euler angles, replacing the original TF
setRPY. These are component-sign settings, not a general physically correct
mounting rotation/reflection model; real mounting/frame conventions must be
verified before state estimation.

Magnetic data retain signed **raw register counts**, using int32 so negating
-32768 is representable. No verified Tesla scale or calibration is assumed.

## Update and time policy

Only SDK callbacks for complete, checksum-valid ACC/GYRO/ANGLE/MAG frames produce
events. Each frame produces one message; equal numeric values in a NEW frame
are still a new sample. Idle Loops, partial/bad frames, temperature/version
updates, shutdown and read errors do not fabricate measurements.

An IMU message is an **asynchronously updated state snapshot**. `updated_mask`
contains exactly the new group (ACC=1, GYRO=2, ANGLE=4); `valid_mask` indicates
which groups have actually been received this lifecycle, NOT freshness.
Unseen groups remain zero/invalid. The three component timestamps identify each
group's last update. Other groups retain their previous value AND timestamp.
Orientation shares the angle timestamp and validity. No interpolation or
claim of synchronized acceleration/gyro/orientation is made.

All timestamps use host CLOCK_MONOTONIC at frame validation. `timestamp_ns`
identifies the newly updated group; these are reception-time proxies because
the used SDK frames expose no acquisition timestamp. Buffered bytes and Linux
scheduling can delay them. Consumers must age-check the relevant component
timestamp; silence never refreshes values or publishes a fake invalid sample.

Application sequence advances for each new attempted measurement publication,
including failed attempts. Published counters advance ONLY when Publish returns
0. EAGAIN drops the sample without retry/retimestamp; later sequences show a gap.
Other Publish errors return failure from Loop. Fatal UART/SDK failures also end
the lifecycle; valid frames received before the failure may still be published
with their original times. Queue overwrite (`ReadInfo.missed`), publish failure,
checksum/discarded bytes, UART failure and SDK processing errors are distinct.
Shutdown logs these counters; they are also exposed by Stats/DriverStats.
Checksum-error count measures rejected candidate frames, not an exact count of
lost sensor measurements. Unknown byte loss cannot be quantified without a
device sequence. A read-budget hit indicates possible backlog, not proven loss.

## Ownership and bounds

The original SDK is process-global, so only one active Wt901cDriver is permitted
per process (a second Open returns EBUSY). Its synchronous callback only copies
the register group into a fixed event buffer; no UART or Topic I/O happens there.
The lifecycle thread owns all reads, parsing and publishing. Each Loop consumes
at most 512 UART bytes (at most 47 complete frames including a carried partial
frame), into 64 event slots. Partial parsing state survives the next Loop.
Negative serial_read_data results exit immediately; idle reads return 0.

Setup opens the selected UART once and creates the two Topics. It reports only
UART readiness and waits for passive streaming frames. The original AutoScan
closed/reopened the FD and ignored the return from WitReadReg despite having no
serial-write callback registered. That ineffective probe is not carried over.
An open but silent device is not reported as a detected or measured sensor.
Missing/inaccessible devices fail Setup. Partial Setup cleans owned resources;
Shutdown and destructor close idempotently and do not unlink another publisher's
Topic. Device strings have owned lifetime; FDs use -1 as the closed sentinel.

## Verification / remaining hardware work

Only kcf_mecanum_imu and mecanum_imu_tests (plus their library dependencies) were
built. Native tests passed: fragmented/checksum-corrupt/resynchronized frames,
all 8 inversion combinations, SI conversions and a known yaw quaternion,
asynchronous timestamps/masks, idle deduplication, identical new frames, raw
magnetometer, Depth=16 overwrite/missed=4, bounded receive backlog, negative
reads/disconnect, init failure FD leaks, duplicate opens/closes, temporary path
strings, partial Topic setup and actual Publisher-close failure accounting.
The executable's nonexistent UART test returned status 1; --help returned 0.

No physical sensor was opened and no real measurements were verified. Pending:
device identity/baud and streaming setup, actual sensor frame/output rates,
queue capacity versus consumer rate, mounted axes and quaternion convention,
gravity/gyro scale and drift, magnetic scale, UART unplug/reconnect and receive
timestamp latency. No Motor, Arduino firmware, KCF Core or EKF code was changed.
