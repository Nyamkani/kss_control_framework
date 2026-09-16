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

// Active UART implementation ported from kss_mecanum_ros v2.0.
#include "mecanum/motor/serial_uart.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>
namespace mecanum::motor
{
SerialComm::~SerialComm() { if(ttyfd_>=0) Close(); }
int SerialComm::Initialize(std::string ttydir,
                int baudrate,
                int stopbit,
                int databits,
                bool hwflow,
                bool canonicalmode,
                int paraty)
{
    if(ttyfd_ >= 0) return -EBUSY;
    if(ttydir.empty())
    {
		printf( "tty dir is empty.\r\n ");

        Close(); return -1;
    }
    else
    {
        this->ttydir_ = ttydir;
    }

    int defined_baudrate = get_baud(baudrate);

    if(defined_baudrate < 0)
    {
		printf( "baudrate has wrong value.\r\n ");

        Close(); return -1;
    }

	this->ttyfd_ = open(this->ttydir_.c_str(), O_RDWR | O_NOCTTY);

	if(this->ttyfd_ < 0)
	{
		printf( "%s : >> tty Open Fail, Try sudo [%s]\r\n ", strerror(EACCES), this->ttydir_.c_str());

		Close(); return -1;
	}
    printf( "Got Pid: [%d]\r\n ", this->ttyfd_);

	memset(&(this->huart_), 0, sizeof(this->huart_));

    this->huart_.c_cflag &= ~CSIZE;

    switch(databits)
    {
        case UART_DATA_BIT_5:  this->huart_.c_cflag |=  CS5; break;
        case UART_DATA_BIT_6:  this->huart_.c_cflag |=  CS6; break;
        case UART_DATA_BIT_7:  this->huart_.c_cflag |=  CS7; break;
        case UART_DATA_BIT_8:  this->huart_.c_cflag |=  CS8; break;

        default : printf("range over. init failed.\r\n"); Close(); return -1;
    }

    switch(stopbit)
    {
        case UART_STOP_BIT_1:  this->huart_.c_cflag &= ~CSTOPB;    break;
        case UART_STOP_BIT_2:  this->huart_.c_cflag |= CSTOPB;     break;

        default : printf("range over. init failed.\r\n"); Close(); return -1;
    }

    if(hwflow)
    {
        this->huart_.c_cflag |= CRTSCTS;

        this->huart_.c_iflag |= (IXON | IXOFF | IXANY);
    }
    else
    {

        this->huart_.c_cflag &= ~(CRTSCTS);

        this->huart_.c_iflag &= ~(IXON | IXOFF | IXANY);
    }

    switch(paraty)
    {
        case UART_PARATY_NONE:  this->huart_.c_cflag &= ~PARENB;  break;
        case UART_PARATY_ODD:
        {
            this->huart_.c_cflag |= PARENB | PARODD;

            this->huart_.c_iflag &= ~(IGNBRK | BRKINT | IGNPAR | ICRNL | INLCR | ISTRIP | IGNCR | IUCLC);

            break;
        }

        case UART_PARATY_EVEN:
        {
            this->huart_.c_cflag |= PARENB;

            this->huart_.c_cflag &= ~PARODD;

            this->huart_.c_iflag &= ~( IGNBRK | BRKINT | IGNPAR | ICRNL | INLCR | ISTRIP | IGNCR | IUCLC);

            break;
        }

        default : printf("range over. init failed.\r\n"); Close(); return -1;
    }

    if(canonicalmode)
    {
        this->huart_.c_lflag |= (ICANON | ECHO | ECHOE | ISIG);

        this->huart_.c_oflag |= OPOST;
    }
    else
    {
        this->huart_.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);

        this->huart_.c_oflag &= ~OPOST;

        this->huart_.c_cc[VMIN]  = 1;

        this->huart_.c_cc[VTIME] = 0;

    }

    this->huart_.c_cflag |=  CREAD | CLOCAL;

    cfsetispeed(&(this->huart_), (speed_t)defined_baudrate);
    cfsetospeed(&(this->huart_),  (speed_t)defined_baudrate);

    if(tcflush(this->ttyfd_, TCIOFLUSH)!=0) { Close(); return -1; }

	if(tcsetattr(this->ttyfd_, TCSANOW, &(this->huart_))!=0) { Close(); return -1; }

	printf( ">> tty Opened [%s] with baudrate [%d]\r\n", this->ttydir_.c_str(), baudrate);

    usleep(500000);

    this->is_init_ = true;

    return 0;
}

int SerialComm::SendData(unsigned char *msg, const size_t msg_length)
{

    if(!(this->is_init_))
    {
        printf("Init is not done. Send data failed.\r\n");

        return -1;
    }

    const int fid = this->ttyfd_;
    unsigned char tx_buffer[UART_MAX_BUF_SIZE] = {0,};

    if(msg_length > sizeof(tx_buffer)) return -EMSGSIZE;
    memcpy(tx_buffer, msg, msg_length);

    const ssize_t count =
        write(fid, tx_buffer, msg_length);

    if(count < 0 ||
    static_cast<size_t>(count) != msg_length)
    {
        printf("UART TX error\n");
        return -1;
    }

    usleep(SERIAL_DATA_SLEEP_TIME_US);

    return 0;
}

int SerialComm::ReadData(unsigned char* recv_msg, const size_t recv_msg_length)
{
    if(!is_init_) return -1;
    size_t received=0;
    while(received<recv_msg_length)
    {
        const auto count=read(ttyfd_,recv_msg+received,recv_msg_length-received);
        if(count<=0) return -1;
        received+=static_cast<size_t>(count);
        usleep(SERIAL_DATA_SLEEP_TIME_US);
        tcflush(ttyfd_,TCIOFLUSH);
    }
    return 0;
}

int SerialComm::ReadDataWithTimeout(
    unsigned char *recv_msg,
    const size_t recv_msg_length,
    size_t timeout_val)
{
    if(!(this->is_init_))
    {
        printf("Init is not done. Read data failed.\r\n");
        return -1;
    }

    const int fid = this->ttyfd_;

    fd_set set;
    FD_ZERO(&set);
    if(fid >= FD_SETSIZE) return -1;
    FD_SET(fid, &set);

    struct timeval timeout;
    timeout.tv_sec = timeout_val / 1000000;
    timeout.tv_usec = timeout_val % 1000000;

    const int rv =
        select(fid + 1, &set, nullptr, nullptr, &timeout);

    if(rv < 0)
    {
        return -1;
    }

    if(rv == 0)
    {
        return 0;
    }

    const ssize_t rx_length =
        read(fid, recv_msg, recv_msg_length);

    if(rx_length < 0)
    {
        return -1;
    }

    return static_cast<int>(rx_length);
}

int SerialComm::Close()
{
    is_init_=false;
    if(ttyfd_<0) return 0;
    const int fd=ttyfd_;
    ttyfd_=-1;
    return close(fd)==0 ? 0 : -errno;
}

int get_baud(int baud)
{
    switch (baud) {
    case 9600:
        return B9600;
    case 19200:
        return B19200;
    case 38400:
        return B38400;
    case 57600:
        return B57600;
    case 115200:
        return B115200;
    case 230400:
        return B230400;
    case 460800:
        return B460800;
    case 500000:
        return B500000;
    case 576000:
        return B576000;
    case 921600:
        return B921600;
    case 1000000:
        return B1000000;
    case 1152000:
        return B1152000;
    case 1500000:
        return B1500000;
    case 2000000:
        return B2000000;
    case 2500000:
        return B2500000;
    case 3000000:
        return B3000000;
    case 3500000:
        return B3500000;
    case 4000000:
        return B4000000;
    default:
        return -1;
    }
}


} // namespace mecanum::motor
