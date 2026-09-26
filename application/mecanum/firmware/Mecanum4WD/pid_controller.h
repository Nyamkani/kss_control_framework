#ifndef PID_CONTROLLER_H
#define PID_CONTROLLER_H

#include "QGPMaker_MotorShield.h"
#include "QGPMaker_Encoder.h"


extern QGPMaker_DCMotor *DCMotor_1;
extern QGPMaker_DCMotor *DCMotor_2;
extern QGPMaker_DCMotor *DCMotor_3;
extern QGPMaker_DCMotor *DCMotor_4;

extern QGPMaker_Encoder MotorEnc1;
extern QGPMaker_Encoder MotorEnc2;
extern QGPMaker_Encoder MotorEnc3;
extern QGPMaker_Encoder MotorEnc4;


constexpr float ENCODER_COUNTS_PER_REV = 4320.0f;
constexpr float MAX_PWM = 255.0f;

constexpr float RPM_LPF_ALPHA = 0.5f;
/*
 * PID gain
 *
 * v2.0 initial test value.
 * 우선 P 제어부터 확인하고 I/D는 이후 조정한다.
 */
constexpr float PID_KP = 5.0f;
constexpr float PID_KI = 5.0f;
constexpr float PID_KD = 0.0f;


typedef struct
{
  float target_rpm;

  float raw_rpm;
  float measured_rpm;
  float prev_measured_rpm;

  int32_t encoder;
  int32_t prev_encoder;
  int32_t delta_ticks;      // debug: encoder delta / PID frame

  float integral;
  float output;

} SetPointInfo;


SetPointInfo M1PID = {};
SetPointInfo M2PID = {};
SetPointInfo M3PID = {};
SetPointInfo M4PID = {};


unsigned char moving = 0;


void setMotorSpeeds(
    int m1speed,
    int m2speed,
    int m3speed,
    int m4speed)
{
  DCMotor_1->setMotorSpeed(m1speed);
  DCMotor_2->setMotorSpeed(m2speed);
  DCMotor_3->setMotorSpeed(m3speed);
  DCMotor_4->setMotorSpeed(m4speed);

  DCMotor_1->motorRun();
  DCMotor_2->motorRun();
  DCMotor_3->motorRun();
  DCMotor_4->motorRun();
}


float clampPWM(float value)
{
  if (value > MAX_PWM)
    return MAX_PWM;

  if (value < -MAX_PWM)
    return -MAX_PWM;

  return value;
}


void resetMotorPID(
    SetPointInfo* pid,
    int32_t encoder)
{
  pid->target_rpm = 0.0f;

  pid->encoder = encoder;
  pid->prev_encoder = encoder;
  pid->delta_ticks = 0;

  pid->raw_rpm = 0.0f;
  pid->measured_rpm = 0.0f;
  pid->prev_measured_rpm = 0.0f;

  pid->integral = 0.0f;
  pid->output = 0.0f;
}


void resetPID()
{
  resetMotorPID(&M1PID, MotorEnc1.read());
  resetMotorPID(&M2PID, MotorEnc2.read());
  resetMotorPID(&M3PID, MotorEnc3.read());
  resetMotorPID(&M4PID, MotorEnc4.read());
}


/*
 * Encoder delta -> measured RPM
 */
void updateMeasuredRPM(
    SetPointInfo* pid,
    int32_t encoder,
    float dt_sec)
{
  pid->encoder = encoder;

  const int32_t delta_ticks =
      pid->encoder - pid->prev_encoder;

  pid->delta_ticks = delta_ticks;

  if (dt_sec > 0.0f)
  {
    pid->raw_rpm =
        ((float)delta_ticks /
         ENCODER_COUNTS_PER_REV)
        * (60.0f / dt_sec);

    // 1st-order LPF
    pid->measured_rpm +=
        RPM_LPF_ALPHA *
        (pid->raw_rpm - pid->measured_rpm);
  }
  else
  {
    pid->raw_rpm = 0.0f;
  }

  pid->prev_encoder = pid->encoder;
}


/*
 * RPM based positional PID
 */
void doPID(
    SetPointInfo* pid,
    float dt_sec)
{
  const float error =
      pid->target_rpm - pid->measured_rpm;


  /* P */
  const float p_term =
      PID_KP * error;


  /* I */
  float new_integral =
      pid->integral +
      PID_KI * error * dt_sec;


  /* D on measurement
   * derivative kick 방지
   */
  float d_term = 0.0f;

  if (dt_sec > 0.0f)
  {
    const float measured_derivative =
        (pid->measured_rpm -
         pid->prev_measured_rpm)
        / dt_sec;

    d_term =
        -PID_KD * measured_derivative;
  }


  float output =
      p_term +
      new_integral +
      d_term;


  /*
   * Simple anti-windup
   *
   * saturation되지 않았거나,
   * saturation을 풀어주는 방향의 error일 때만
   * integral 갱신.
   */
  if ((output >= -MAX_PWM && output <= MAX_PWM) ||
      (output > MAX_PWM && error < 0.0f) ||
      (output < -MAX_PWM && error > 0.0f))
  {
    pid->integral = new_integral;
  }


  output =
      p_term +
      pid->integral +
      d_term;


  pid->output =
      clampPWM(output);

  pid->prev_measured_rpm =
      pid->measured_rpm;
}


/*
 * 4 motor speed controller update
 */
void updatePID(float dt_sec)
{
  /*
   * Encoder snapshot
   */
  const int32_t enc1 = MotorEnc1.read();
  const int32_t enc2 = MotorEnc2.read();
  const int32_t enc3 = MotorEnc3.read();
  const int32_t enc4 = MotorEnc4.read();


  /*
   * RPM은 moving 여부와 상관없이 계속 계산한다.
   * 정지 후 coast-down RPM도 확인 가능.
   */
  updateMeasuredRPM(&M1PID, enc1, dt_sec);
  updateMeasuredRPM(&M2PID, enc2, dt_sec);
  updateMeasuredRPM(&M3PID, enc3, dt_sec);
  updateMeasuredRPM(&M4PID, enc4, dt_sec);


  if (!moving)
    return;


  doPID(&M1PID, dt_sec);
  doPID(&M2PID, dt_sec);
  doPID(&M3PID, dt_sec);
  doPID(&M4PID, dt_sec);


  setMotorSpeeds(
      (int)M1PID.output,
      (int)M2PID.output,
      (int)M3PID.output,
      (int)M4PID.output);
}


#endif
