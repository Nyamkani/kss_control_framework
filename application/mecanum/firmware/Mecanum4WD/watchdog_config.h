#pragma once
#include <float.h>

// No RPM limit exists in upstream v2.0. Supply a hardware-approved limit via
// the compiler definition, e.g. -DMECANUM_MAX_ABS_RPM=<approved value>.
// PWM's 255 limit is NOT an RPM limit. No unverified default is provided.
#ifndef MECANUM_MAX_ABS_RPM
#error "Define MECANUM_MAX_ABS_RPM using the robot's approved wheel RPM limit"
#endif
static_assert(MECANUM_MAX_ABS_RPM > 0 && MECANUM_MAX_ABS_RPM <= FLT_MAX,
              "RPM limit must be positive and finite");
