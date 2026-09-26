# Mecanum 2D EKF

Standalone KCF v5.1 application; no ROS2, serial access or actuator commands.
`kcf_mecanum_ekf --help` lists Topic overrides and `--timeout-ms` (1–1000).
The initial Loop rate is 40 Hz, not a real-time guarantee.

## Model and initial reference

State order is `[x, y, yaw, vx, vy, wz]`. Position and heading use a local
planar frame; velocities use the robot frame (x forward, y left, z/yaw CCW).
Units are metres, seconds and radians. Input sensors must already use compatible
axes, mounting and angular signs; this application does not calibrate them.
Initial position is zero. In the default relative mode, initial heading is zero
and the first accepted IMU yaw becomes the heading offset, not a verified
north/world direction. An IMU source/axis change in relative mode reanchors to
the retained local heading; it cannot recover motion during a gap.
Restarting the EKF creates a new `session_id` and a new local origin.

In relative mode, the first yaw frame of each anchor is used only to set the offset. Its timestamp
is accepted for freshness and duplicate rejection, but it does **not** perform a
yaw Kalman update: the constructed relative yaw equals the current estimate and
adds no independent information. Anchoring leaves the state and covariance
unchanged; subsequent new yaw frames use the normal measurement update. The
existing source-change policy still resets twist and its cross covariance before
reanchoring, while retaining local position and yaw.

The initial yaw reference error can correlate subsequent relative yaw errors.
For example, independent raw errors `e_k` become `e_k - e_0` after subtraction of
the first sample, sharing the same reference error. The current fixed yaw R model
does not explicitly represent this temporal correlation or reference uncertainty.
Skipping the anchor update fixes the artificial covariance reduction at anchor
creation; it does not establish statistical consistency of all later yaw updates.

In absolute mode (`imu_yaw_relative=false`), the first valid yaw directly sets
`yaw=wrap(measured_yaw)` and `P[yaw,yaw]=yaw_r`, with no Kalman update using that
same sample. Other initial covariance entries stay unchanged. Before that first
yaw, Wheel and gyro samples are ignored (not buffered), with no accepted state
update, prediction or timestamp advancement. This gate prevents changing the
coordinate frame or resetting an already updated covariance after late yaw
arrival. Fresh enabled Wheel/gyro inputs must arrive after initialization.
The gate also applies to initially queued samples sorted before the first yaw.

After absolute initialization, IMU disconnect/recreation/sequence restart does
not change the heading reference or reinitialize yaw covariance. Recovery yaw
uses the ordinary measurement update in the existing frame. This can correct
yaw normally; it never forces yaw to zero or creates a new offset. If the sensor
itself resets its heading reference, this implementation cannot identify the
correct physical frame transform. Verify that convention before use.
Absolute mode consumes the IMU Element's yaw, not raw magnetometer counts. It
assumes neither magnetic calibration nor verified mounting, north alignment or
coordinate transformation. If IMU yaw is disabled, the relative flag has no
effect; heading starts at zero and evolves through the selected measurements.

## Measurement selection and execution

`Options` exposes these booleans through its `Selection` base. Each defaults to
`true`, preserving the prior configuration. CLI values must be `true` or `false`.
Selection is fixed for the lifetime of an Element; there is no new configuration
service or runtime switching API.

| C++ setting | Command-line option |
| --- | --- |
| `use_wheel_vx` | `--use-wheel-vx` |
| `use_wheel_vy` | `--use-wheel-vy` |
| `use_wheel_wz` | `--use-wheel-wz` |
| `use_imu_yaw` | `--use-imu-yaw` |
| `use_imu_gyro_z` | `--use-imu-gyro-z` |
| `imu_yaw_relative` | `--imu-yaw-relative` |

```
# Absolute IMU yaw, excluding Wheel angular velocity
/tmp/kcf-mecanum-ekf-build/application/mecanum/ekf/kcf_mecanum_ekf --imu-yaw-relative false --use-wheel-wz false
# Wheel-only input: no IMU Topic required
/tmp/kcf-mecanum-ekf-build/application/mecanum/ekf/kcf_mecanum_ekf --use-imu-yaw false --use-imu-gyro-z false
```

A disabled measurement performs no measurement update and is not required for
freshness or recovery. Disabled Wheel component values are not checked for
finiteness. If any Wheel component is enabled, its packet validity and shared
measurement timestamp remain required. If a whole source is disabled, no
connection is opened and its absence/expiry/restart does not invalidate output.
Its connected flag remains false; consumers must use the configured selection
when interpreting it. Topic names, depths and message layout are unchanged.

All five measurements disabled is rejected during construction before publishing
or running. Other nonempty subsets are accepted as partial estimates: `valid`
means the enabled inputs are fresh and the filter is healthy, not that all six
states are independently observable. Disabling an observation does not pin its
state to zero: prediction and cross covariance can still change that state.
Unobserved motion and drift cannot be recovered by selection alone.

For measurement-time interval h, the constant body-velocity prediction is:

```
x'   = x + h (cos(yaw) vx - sin(yaw) vy)
y'   = y + h (sin(yaw) vx + cos(yaw) vy)
yaw' = wrap(yaw + h wz)
vx', vy', wz' = vx, vy, wz
```

F is identity with these additional entries (zero-based indices):
`F02=h(-sin(yaw)vx-cos(yaw)vy)`, `F03=h cos(yaw)`,
`F04=-h sin(yaw)`, `F12=h(cos(yaw)vx-sin(yaw)vy)`,
`F13=h sin(yaw)`, `F14=h cos(yaw)`, `F25=h`.
Prediction uses `P'=F P F^T + diag(q) h`, subdividing intervals into at most
50 ms steps. This is an Euler-discretized planar model, not exact arc integration.

Wheel updates select state indices 3,4,5; gyro z selects 5; relative yaw selects
2. Wheel position is never fused. Diagonal R permits sequential scalar updates:
`innovation=z-Hx`, `S=HPH^T+R`, `K=PH^T/S`, `x'=x+K innovation`,
`P'=(I-KH)P(I-KH)^T+KRK^T` (Joseph form). Yaw innovation and resulting heading
are wrapped at ±pi. Matrices are symmetrized and checked for finite values and
positive definiteness with Cholesky. Failed calculations do not commit their
candidate state; numerical failure invalidates the output and ends the Element.

All noise settings are **untuned trial variances**, configurable through
`Options::noise` in C++; they are not measured sensor accuracy:

| Setting | Diagonal values, in state/measurement order |
| --- | --- |
| q, variance rate per second | .05, .05, .06, .025, .025, .02 |
| Initial P | 1e-6, 1e-6, .04, 1, 1, 1 |
| Wheel R, vx/vy/wz | .04, .04, .02 |
| Gyro z R | .01 |
| Relative yaw R | .04 |

Position variances use m², angle rad², linear velocity (m/s)², angular velocity
(rad/s)². The six q values refer to those units per second. Initial near-zero
position variance defines the chosen origin, not absolute positioning accuracy.
The velocity-only Wheel selection, relative IMU heading, 40 Hz starting rate
and six process-noise values reference the original
[ROS2 v2.0 ekf.yaml](https://github.com/Nyamkani/kss_mecanum_ros/blob/v2.0/mecanum_hardwares/packages/ekf/ekf/config/ekf.yaml).
The original 15-state filter, 100 ms timeout and tiny initial covariance are not
copied as an already validated model. Wheel velocity components and IMU yaw/gyro
may be correlated; the initial diagonal measurement-noise approximation needs
hardware evaluation.

## Topics and time policy

| Topic | Read/write policy | Data used |
| --- | --- | --- |
| `/mecanum/wheel_odometry` | depth 1, ReadLatest | valid new body vx, vy, wz only |
| `/mecanum/imu/raw_data` | depth 16, ReadNext | independently updated gyro z and yaw |
| `/mecanum/ekf/odometry` | depth 1 Snapshot | `EstimateMessage`, Tool descriptor `kcf.mecanum.Ekf2D.v1` |

All timestamps use the same host CLOCK_MONOTONIC epoch. Existing input timestamps
are host receipt/validation times, not synchronized hardware acquisition times.
The output contains the six states, row-major 6x6 covariance, latest accepted
measurement time, separate publication time, per-input accepted timestamps,
validity, connections/generations and diagnostic counters. Repeated status
publications increase `sequence` but do not invent a new measurement timestamp
or re-integrate previous measurements. Output consumers must inspect timestamps
and `valid`, not publication sequence alone.

IMU queue reads are bounded to 32 per Loop and `TopicReadInfo::missed` is counted.
Only GYRO/ANGLE bits in both updated/valid masks produce measurements; the new
component timestamp must equal that packet's timestamp, matching the existing
IMU producer contract. Cached components are not fused again. Within each Loop,
staged measurements are sorted by timestamp. Duplicate component timestamps,
zero/future/expired timestamps and measurements older than the filter time are
rejected. Cross-Loop late measurements are dropped rather than rewinding the
filter; no synchronization buffer or delayed-state smoothing is implemented.
Wheel snapshot skips and application sequence gaps have separate counters;
these can overlap and must not be summed as unique lost measurements.

The initial freshness timeout is **500 ms, provisional**, independently
configurable; it is not the Motor command timeout or MCU watchdog. Only enabled measurements must be fresh (all three source groups by default). Missing/invalid/resynchronized enabled Wheel data, required connection
changes or expiry suspend prediction and zero the estimate's twist (this does
not send a motor stop). Pose is held and no unobserved gap is integrated.
Recovery requires fresh enabled inputs. `valid` means currently fresh, connected and
numerically healthy, not continuous global localization: `discontinuities`
records interruptions. Pose covariance does not account for arbitrary actual
motion during a gap; downstream consumers must handle that discontinuity.
No wall-clock extrapolation is performed on empty Loops.

Known pre-existing limitation, intentionally unchanged: partial recovery across
Loops can accept a Wheel update, then erase its twist via `Expire/StopMotion`
while retaining its accepted timestamp. Later IMU recovery can mark the output
valid before another Wheel update. Measurement selection does not fix this
state/timestamp inconsistency. Also, pose covariance still does not represent
unobserved motion during an input gap. Neither limitation is a new yaw-mode
initialization policy.

KCF v5.1 lacks a public generation/reconnect accessor. `Connection` therefore
read-only checks the actual POSIX object's device/inode and owner header, retains
the previous identity FD, and reopens the typed Subscriber when necessary.
This application adapter is coupled to v5.1 naming/header layout; Core is not
modified. Source application sequence regression with a newer timestamp also
starts a logical generation. A sequence reset indistinguishable from a duplicate
is conservatively dropped. PID reuse/zombie processes and a publisher closed
without unlinking while its owner remains alive can delay disconnect detection;
timestamp expiry remains independent. Name replacement races cannot provide an
atomic multi-Topic snapshot. Publish failures are counted, EAGAIN is retried on
later Loops, and other publication errors end the Element.

## Build and checks

```
cmake -S . -B /tmp/kcf-mecanum-ekf-build -DMECANUM_BUILD_EKF_TESTS=ON
cmake --build /tmp/kcf-mecanum-ekf-build --target kcf_mecanum_ekf mecanum_ekf_tests -j2
/tmp/kcf-mecanum-ekf-build/application/mecanum/ekf/mecanum_ekf_tests
```

Tests exercise analytic planar trajectories, finite-difference Jacobian, scalar
Kalman/Joseph results, covariance failure handling, yaw wrap, timestamps,
duplication/expiry, bounded queue overwrite, cached IMU components, publisher
replacement/reconnection, application sequence restart, missing inputs and
publication failures, using synthetic data and isolated local shared memory.
Anchor-specific checks compare the entire state and covariance before/after
initial anchoring and reconnection, verify nonzero local pose continuity, reject
duplicate anchor timestamps and check the next yaw update against scalar Kalman
mean/variance formulas, including a raw yaw crossing of ±pi.
Selection tests cover all 31 nonempty measurement subsets, disabled components
with nonfinite placeholders, disabled-source absence/expiry, all-disabled
rejection, relative/absolute 70-degree initialization, measurement-variance
initialization without duplicate update, startup gating, subsequent yaw updates,
angle wrapping and absolute Topic generation replacement without reanchoring.
They do not establish physical accuracy. Hardware work remains: axes/mounting,
heading convention and reset behaviour, actual latency/rates/queue loss,
freshness selection, encoder slip, yaw/gyro correlation, Q/R tuning and measured
trajectory error. Acceleration integration, magnetometer fusion and 3D estimation
are deliberately outside this implementation.
