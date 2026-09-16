// Internal UART for the Jetson nano
//
// Copyright (c) 2024 Kss
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// v0.1    042524
//

#pragma once
#include <cstddef>
#include <string>
#include <termios.h>

namespace mecanum::motor
{
inline constexpr int UART_DATA_BIT_5=5, UART_DATA_BIT_6=6, UART_DATA_BIT_7=7, UART_DATA_BIT_8=8;
inline constexpr int UART_STOP_BIT_1=1, UART_STOP_BIT_2=2;
inline constexpr int UART_PARATY_NONE=0, UART_PARATY_ODD=1, UART_PARATY_EVEN=2;
inline constexpr bool UART_HWFLOW_CONTROL_DISABLE=false, UART_CONONICAL_MODE_DISABLE=false;
inline constexpr std::size_t UART_MAX_BUF_SIZE=255;
inline constexpr unsigned SERIAL_DATA_SLEEP_TIME_US=10;
int get_baud(int baud);
class SerialComm
{
public:
    SerialComm() = default;
    ~SerialComm();
    SerialComm(const SerialComm&)=delete;
    SerialComm& operator=(const SerialComm&)=delete;
    int Initialize(std::string ttydir, int baudrate, int stopbit, int databits,
                   bool hwflow, bool canonicalmode, int paraty);
    int SendData(unsigned char* msg, std::size_t msg_length);
    int ReadData(unsigned char* msg, std::size_t msg_length);
    int ReadDataWithTimeout(unsigned char* msg, std::size_t msg_length, std::size_t timeout_us);
    int Close();
private:
    termios huart_{};
    int ttyfd_{-1};
    std::string ttydir_;
    bool is_init_{false};
};
} // namespace mecanum::motor
