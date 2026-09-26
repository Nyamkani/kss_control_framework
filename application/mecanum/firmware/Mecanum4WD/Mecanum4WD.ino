#include <Wire.h>
#include "PS2X_lib.h"
#include "QGPMaker_MotorShield.h"
#include "QGPMaker_Encoder.h"
#include "pid_controller.h"
#include "commands.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "watchdog_config.h"


//debug
#define DEBUG_PID_TIMING  0
#define DEBUG_RPM         0

#if DEBUG_PID_TIMING

uint32_t pid_prev_start_us = 0;

uint32_t pid_period_min_us = 0xFFFFFFFFUL;
uint32_t pid_period_max_us = 0;

uint32_t pid_exec_max_us = 0;

uint32_t pid_stat_count = 0;
uint32_t pid_last_report_ms = 0;

#endif

#if DEBUG_RPM
constexpr uint32_t RPM_DEBUG_INTERVAL_MS = 100;

uint32_t rpm_debug_prev_ms = 0;

int32_t m3_debug_prev_encoder = 0;
uint32_t m3_debug_prev_us = 0;
#endif


QGPMaker_MotorShield AFMS = QGPMaker_MotorShield();
PS2X ps2x;
long ARM_MIN[]={10, 10, 40, 10};

long ARM_MAX[]={170, 140, 170, 102};

/* Variable initialization */
// Current command
char cmd = '\0';

// Parsed command arguments
float arg1 = 0;
float arg2 = 0;
float arg3 = 0;
float arg4 = 0;

// UART RX buffer
const uint8_t SERIAL_RX_BUF_SIZE = 64;
const uint8_t MAX_COMMAND_ARGS = 4;

char serial_rx_buffer[SERIAL_RX_BUF_SIZE] = {0};
uint8_t serial_rx_index = 0;
bool serial_rx_overflow = false;

/* PID control frequency */
constexpr uint32_t PID_RATE_HZ = 80;

/* PID period in microseconds */
constexpr uint32_t PID_INTERVAL_US =
    1000000UL / PID_RATE_HZ;

/* Next PID execution time */
uint32_t next_pid_us = 0;

uint32_t prev_pid_sample_us = 0;

/* Stop the robot if it hasn't received a movement command
  in this number of milliseconds */
#define AUTO_STOP_INTERVAL 10000
uint32_t lastMotorCommand = 0;

// Reuse the original output and PID reset functions; resetPID also clears targets.
void stopMotors()
{
  moving = 0;
  setMotorSpeeds(0, 0, 0, 0);
  resetPID();
}

void checkMotorWatchdog()
{
  if (moving && (uint32_t)((uint32_t)millis() - lastMotorCommand) >= AUTO_STOP_INTERVAL)
    stopMotors();
}

bool validMotorRPM(float rpm)
{
  return isfinite(rpm) && fabsf(rpm) <= MECANUM_MAX_ABS_RPM;
}

extern SetPointInfo M1PID, M2PID, M3PID, M4PID;

//250->rpm =120 = 2rev/s
//200->rpm = 112- >1.86rev/s
//100->rpm= 95 =->1.58rev/s
//50 ->rpm = 60->1rev/s


QGPMaker_DCMotor *DCMotor_2 = AFMS.getMotor(2);
QGPMaker_DCMotor *DCMotor_4 = AFMS.getMotor(4);
QGPMaker_DCMotor *DCMotor_1 = AFMS.getMotor(1);
QGPMaker_DCMotor *DCMotor_3 = AFMS.getMotor(3);
QGPMaker_Encoder MotorEnc1(1, -1);
QGPMaker_Encoder MotorEnc2(2, -1);
QGPMaker_Encoder MotorEnc3(3, 1);
QGPMaker_Encoder MotorEnc4(4, 1);



void forward() {
  DCMotor_1->setSpeed(83);
  DCMotor_1->run(FORWARD);
  DCMotor_2->setSpeed(83);
  DCMotor_2->run(FORWARD);
  DCMotor_3->setSpeed(83);
  DCMotor_3->run(FORWARD);
  DCMotor_4->setSpeed(83);
  DCMotor_4->run(FORWARD);
}

void turnLeft() {
  DCMotor_1->setSpeed(200);
  DCMotor_1->run(BACKWARD);
  DCMotor_2->setSpeed(200);
  DCMotor_2->run(BACKWARD);
  DCMotor_3->setSpeed(200);
  DCMotor_3->run(FORWARD);
  DCMotor_4->setSpeed(200);
  DCMotor_4->run(FORWARD);
}

void turnRight() {
  DCMotor_1->setSpeed(200);
  DCMotor_1->run(FORWARD);
  DCMotor_2->setSpeed(200);
  DCMotor_2->run(FORWARD);
  DCMotor_3->setSpeed(200);
  DCMotor_3->run(BACKWARD);
  DCMotor_4->setSpeed(200);
  DCMotor_4->run(BACKWARD);
}

void moveLeft() {
  DCMotor_1->setSpeed(200);
  DCMotor_1->run(BACKWARD);
  DCMotor_2->setSpeed(200);
  DCMotor_2->run(FORWARD);
  DCMotor_3->setSpeed(200);
  DCMotor_3->run(BACKWARD);
  DCMotor_4->setSpeed(200);
  DCMotor_4->run(FORWARD);
}

void moveRight() {
  DCMotor_1->setSpeed(200);
  DCMotor_1->run(FORWARD);
  DCMotor_2->setSpeed(200);
  DCMotor_2->run(BACKWARD);
  DCMotor_3->setSpeed(200);
  DCMotor_3->run(FORWARD);
  DCMotor_4->setSpeed(200);
  DCMotor_4->run(BACKWARD);
}

void backward() {
  DCMotor_1->setSpeed(200);
  DCMotor_1->run(BACKWARD);
  DCMotor_2->setSpeed(200);
  DCMotor_2->run(BACKWARD);
  DCMotor_3->setSpeed(200);
  DCMotor_3->run(BACKWARD);
  DCMotor_4->setSpeed(200);
  DCMotor_4->run(BACKWARD);
}




void stopMoving() {
  DCMotor_1->setSpeed(0);
  DCMotor_1->run(RELEASE);
  DCMotor_2->setSpeed(0);
  DCMotor_2->run(RELEASE);
  DCMotor_3->setSpeed(0);
  DCMotor_3->run(RELEASE);
  DCMotor_4->setSpeed(0);
  DCMotor_4->run(RELEASE);
}

void forwardMoving()
{
  DCMotor_1->setSpeed(arg1);
  DCMotor_1->run(FORWARD);
  DCMotor_2->setSpeed(arg2);
  DCMotor_2->run(FORWARD);
  DCMotor_3->setSpeed(arg3);
  DCMotor_3->run(FORWARD);
  DCMotor_4->setSpeed(arg4);
  DCMotor_4->run(FORWARD);
  return;
}

void readWheelRPMs()
{
  Serial.print(M1PID.measured_rpm, 2);
  Serial.print(" ");

  Serial.print(M2PID.measured_rpm, 2);
  Serial.print(" ");

  Serial.print(M3PID.measured_rpm, 2);
  Serial.print(" ");

  Serial.println(M4PID.measured_rpm, 2);
}


void readEncoders()
{
  Serial.print(MotorEnc1.read());
  Serial.print(" ");
  Serial.print(MotorEnc2.read());
  Serial.print(" ");
  Serial.print(MotorEnc3.read());
  Serial.print(" ");
  Serial.print(MotorEnc4.read());
  Serial.print("\r\n");
  return;
}

void resetEncoders()
{
    MotorEnc1.write(0);
    MotorEnc2.write(0);
    MotorEnc3.write(0);
    MotorEnc4.write(0);

  return;
}


/* Clear the current command parameters */
void resetCommand()
{
  cmd = '\0';

  arg1 = 0.0f;
  arg2 = 0.0f;
  arg3 = 0.0f;
  arg4 = 0.0f;
}



bool parseFloatToken(const char* token, float* value)
{
  if (token == NULL || value == NULL || token[0] == '\0')
    return false;

  char* end_ptr = NULL;

  double parsed = strtod(token, &end_ptr);

  if (end_ptr == token || *end_ptr != '\0' || !isfinite(parsed) ||
      parsed > FLT_MAX || parsed < -FLT_MAX)
    return false;

  *value = (float)parsed;

  return isfinite(*value);
}


bool parseCommand(char* line)
{
  resetCommand();

  char* save_ptr = NULL;
  char* token = strtok_r(line, " ", &save_ptr);

  if (token == NULL ||
      token[0] == '\0' ||
      token[1] != '\0')
  {
    return false;
  }

  cmd = token[0];

  float* args[MAX_COMMAND_ARGS] =
  {
    &arg1,
    &arg2,
    &arg3,
    &arg4
  };

  uint8_t arg_count = 0;

  while ((token = strtok_r(NULL, " ", &save_ptr)) != NULL)
  {
    if (arg_count >= MAX_COMMAND_ARGS)
      return false;

    if (!parseFloatToken(token, args[arg_count]))
      return false;

    arg_count++;
  }


  if (cmd == MOTOR_SPEEDS)
  {
    return arg_count == 4 && validMotorRPM(arg1) && validMotorRPM(arg2) &&
           validMotorRPM(arg3) && validMotorRPM(arg4);
  }

  if (cmd == READ_ENCODERS ||
      cmd == READ_RPMS ||
      cmd == RESET_ENCODERS)
  {
    return (arg_count == 0);
  }

  return true;
}


void runCommand();   // forward declaration


void processSerial()
{
  // Bound both input work and reply-producing commands per Loop.
  // Partial/overflow frames remain buffered across calls.
  uint8_t budget = SERIAL_RX_BUF_SIZE;
  while (budget-- > 0 && Serial.available() > 0)
  {
    checkMotorWatchdog();
    char ch = (char)Serial.read();

    // 기존 RPi protocol의 packet terminator
    if (ch == '\r')
    {
      if (serial_rx_overflow)
      {
        Serial.println("Invalid Command");
      }
      else if (serial_rx_index > 0)
      {
        serial_rx_buffer[serial_rx_index] = '\0';

        if (parseCommand(serial_rx_buffer))
        {
          runCommand();
        }
        else
        {
          Serial.println("Invalid Command");
        }
      }

      // 다음 frame 준비
      serial_rx_index = 0;
      serial_rx_overflow = false;

      resetCommand();
      return; // at most one frame/reply before the next watchdog/PID check
    }
    else if (ch == '\n')
    {

    }
    else
    {
      if (!serial_rx_overflow)
      {
        if (serial_rx_index < SERIAL_RX_BUF_SIZE - 1)
        {
          serial_rx_buffer[serial_rx_index++] = ch;
        }
        else
        {
          serial_rx_overflow = true;
        }
      }
    }
  }
}

void runCommand()
{
  switch(cmd) {
    case GET_BAUDRATE:
      // Serial.println(BAUDRATE);
      break;
    case ANALOG_READ:
      // Serial.println(analogRead(arg1));
      break;
    case DIGITAL_READ:
      // Serial.println(digitalRead(arg1));
      break;
    // case ANALOG_WRITE:
      // analogWrite(arg1, arg2);
      // Serial.println("OK"); 
      // break;
    // case DIGITAL_WRITE:
      // if (arg2 == 0) digitalWrite(arg1, LOW);
      // else if (arg2 == 1) digitalWrite(arg1, HIGH);
      // Serial.println("OK"); 
      // break;
    case PIN_MODE:
      // if (arg2 == 0) pinMode(arg1, INPUT);
      // else if (arg2 == 1) pinMode(arg1, OUTPUT);
      // Serial.println("OK");
      break;
    case PING:
      // Serial.println(Ping(arg1));
      break;
  // #ifdef USE_SERVOS
  //   case SERVO_WRITE:
  //     servos[arg1].setTargetPosition(arg2);
  //     Serial.println("OK");
  //     break;
  //   case SERVO_READ:
  //     Serial.println(servos[arg1].getServo().read());
  //     break;
  // #endif
      
  // #ifdef USE_BASE
    case READ_ENCODERS:
        readEncoders();
      break;
    case RESET_ENCODERS:
      resetEncoders();
      resetPID();
      Serial.println("OK");
      break;
    case READ_RPMS:
        readWheelRPMs();
      break;
    case MOTOR_SPEEDS:
    
      if (!validMotorRPM(arg1) || !validMotorRPM(arg2) ||
          !validMotorRPM(arg3) || !validMotorRPM(arg4))
      {
        Serial.println("Invalid Command");
        break;
      }
    
      if (arg1 == 0.0f &&
          arg2 == 0.0f &&
          arg3 == 0.0f &&
          arg4 == 0.0f)
      {
        stopMotors();
      }
      else
      {
        M1PID.target_rpm = arg1;
        M2PID.target_rpm = arg2;
        M3PID.target_rpm = arg3;
        M4PID.target_rpm = arg4;
    
        moving = 1;
      }
      lastMotorCommand = (uint32_t)millis();
      Serial.println("OK");
      break;
  //   case UPDATE_PID:
  //     while ((str = strtok_r(p, ":", &p)) != '\0') {
  //       pid_args[i] = atoi(str);
  //       i++;
  //     }
  //     Kp = pid_args[0];
  //     Kd = pid_args[1];
  //     Ki = pid_args[2];
  //     Ko = pid_args[3];
  //     Serial.println("OK");
  //     break;
  // #endif
    default:
      Serial.println("Invalid Command");
      break;
    }
}




void setup(){
  AFMS.begin(1600);
  Serial.begin(115200);

  stopMotors();
  
  const uint32_t now_us = micros();
  
  prev_pid_sample_us = now_us;
  next_pid_us = now_us + PID_INTERVAL_US;

#if DEBUG_RPM

  m3_debug_prev_encoder = MotorEnc3.read();
  m3_debug_prev_us = micros();

#endif

  Serial.println("Setup Complete!");
}

//encoder = 4320
//pwm == 144 -> rpm 60 ->1rev /s
void loop()
{
  checkMotorWatchdog();
  processSerial();
  checkMotorWatchdog();

  const uint32_t now_us = micros();

  if ((int32_t)(now_us - next_pid_us) >= 0)
  {
    const uint32_t pid_sample_us = micros();
  
    float dt_sec;
  
    if (prev_pid_sample_us == 0)
    {
      dt_sec =
          (float)PID_INTERVAL_US /
          1000000.0f;
    }
    else
    {
      dt_sec =
          (float)(pid_sample_us -
                  prev_pid_sample_us)
          / 1000000.0f;
    }
  
    prev_pid_sample_us =
        pid_sample_us;
  
  
  #if DEBUG_PID_TIMING
  
    const uint32_t pid_start_us =
        pid_sample_us;
  
    if (pid_prev_start_us != 0)
    {
      const uint32_t period_us =
          pid_start_us -
          pid_prev_start_us;
  
      if (period_us < pid_period_min_us)
        pid_period_min_us = period_us;
  
      if (period_us > pid_period_max_us)
        pid_period_max_us = period_us;
    }
  
    pid_prev_start_us =
        pid_start_us;
  
  #endif
  
  
    updatePID(dt_sec);
  
  
  #if DEBUG_PID_TIMING
  
    const uint32_t pid_exec_us =
        micros() - pid_start_us;
  
    if (pid_exec_us > pid_exec_max_us)
      pid_exec_max_us = pid_exec_us;
  
    pid_stat_count++;
  
  #endif
  
  
    next_pid_us =
        pid_sample_us +
        PID_INTERVAL_US;
  }

  checkMotorWatchdog();
#if DEBUG_PID_TIMING

  if ((millis() - pid_last_report_ms) >= 1000)
  {
    pid_last_report_ms = millis();
  
    Serial.print("PID period min/max(us): ");
    Serial.print(pid_period_min_us);
    Serial.print(" / ");
    Serial.print(pid_period_max_us);
  
    Serial.print("  exec max(us): ");
    Serial.print(pid_exec_max_us);
  
    Serial.print("  count: ");
    Serial.println(pid_stat_count);
  
    pid_period_min_us = 0xFFFFFFFFUL;
    pid_period_max_us = 0;
    pid_exec_max_us = 0;
    pid_stat_count = 0;
}

#endif

#if DEBUG_RPM

  const uint32_t now_ms = millis();
  
  if ((now_ms - rpm_debug_prev_ms) >= RPM_DEBUG_INTERVAL_MS)
  {
    rpm_debug_prev_ms = now_ms;
  
    const uint32_t debug_now_us = micros();
    const int32_t m3_encoder_now = MotorEnc3.read();
  
    const int32_t m3_delta_100ms =
        m3_encoder_now - m3_debug_prev_encoder;
  
    const uint32_t m3_dt_us =
        debug_now_us - m3_debug_prev_us;
  
    float m3_raw_rpm = 0.0f;
  
    if (m3_dt_us > 0)
    {
      const float dt_sec =
          (float)m3_dt_us / 1000000.0f;
  
      m3_raw_rpm =
          ((float)m3_delta_100ms /
           ENCODER_COUNTS_PER_REV)
          * (60.0f / dt_sec);
    }
  
    m3_debug_prev_encoder = m3_encoder_now;
    m3_debug_prev_us = debug_now_us;
  
  
    Serial.print("RPM: ");
  
    Serial.print(M1PID.measured_rpm, 2);
    Serial.print(" ");
  
    Serial.print(M2PID.measured_rpm, 2);
    Serial.print(" ");
  
    Serial.print(M3PID.measured_rpm, 2);
    Serial.print(" ");
  
    Serial.print(M4PID.measured_rpm, 2);
  
  
    Serial.print("  M3_DTICK: ");
    Serial.print(M3PID.delta_ticks);
  
    Serial.print("  M3_RAW100: ");
    Serial.print(M3PID.raw_rpm, 2);
    Serial.print(" / ");
    Serial.print(M3PID.measured_rpm, 2);
  
    Serial.print("  RAW_TICK: ");
    Serial.println(m3_delta_100ms);
  }

#endif
  
}
