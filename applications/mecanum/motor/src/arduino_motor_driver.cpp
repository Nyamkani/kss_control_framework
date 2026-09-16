// Active transaction/parsing path from kss_mecanum_ros v2.0.
#include "mecanum/motor/arduino_motor_driver.hpp"
#include <cstring>
#include <sstream>
namespace mecanum::motor
{
static constexpr int MAX_BUF=100;
int ArduinoMotorDriver::Initialize(const std::string& tty, std::int32_t baud_rate)
{
    motor_data_.assign(no_of_motors_, MotorData{});
    return Serial_.Initialize(tty,baud_rate,UART_STOP_BIT_1,UART_DATA_BIT_8,
        UART_HWFLOW_CONTROL_DISABLE,UART_CONONICAL_MODE_DISABLE,UART_PARATY_NONE);
}
int ArduinoMotorDriver::Close() { return Serial_.Close(); }
int ArduinoMotorDriver::Write(const ArduinoCommand& cmd)
{
    const int arg_length = cmd.args.size();
    std::string write_data_buf;

    write_data_buf.push_back(cmd.command);

    for(int i = 0; i < arg_length; i++)
    {
        write_data_buf.append(" ");
        write_data_buf.append(std::to_string(cmd.args[i]));
    }

    write_data_buf.append("\r");

    if(this->Serial_.SendData(
        reinterpret_cast<unsigned char*>(
            write_data_buf.data()),
        write_data_buf.length()) < 0)
    {
        return -1;
    }

    std::string read_data;
    unsigned char read_buf[MAX_BUF];

    constexpr int ACK_TIMEOUT_US = 1000;
    constexpr int ACK_RETRY_MAX = 100;

    bool ack_received = false;

    for(int repeat = 0;
        repeat < ACK_RETRY_MAX;
        repeat++)
    {
        memset(read_buf, 0, sizeof(read_buf));

        const int ret =
            this->Serial_.ReadDataWithTimeout(
                read_buf,
                sizeof(read_buf),
                ACK_TIMEOUT_US);

        if(ret < 0)
        {
            return -1;
        }

        if(ret == 0)
        {
            continue;
        }

        read_data.append(
            reinterpret_cast<char*>(read_buf),
            static_cast<size_t>(ret));

        if(read_data.find("OK\r\n") !=
           std::string::npos)
        {
            ack_received = true;
            break;
        }
    }

    if(!ack_received)
    {
        return -1;
    }

    return 0;
}

int ArduinoMotorDriver::Read(const ArduinoCommand& cmd)
{
        //read encoders
        // ArduinoCommand cmd;
        std::string write_data_buf;

        //1. send data with read encoder or rpms
        write_data_buf.append(sizeof(char), cmd.command);

        write_data_buf.append(" ");
        write_data_buf.append("\r"); //EOD

        if(this->Serial_.SendData((unsigned char*)write_data_buf.c_str(), write_data_buf.length()) < 0)
            return -1;

        //2. Read data with args
        std::string read_data;

        unsigned char read_buf[MAX_BUF];

        memset(read_buf, 0, sizeof(read_buf));

        constexpr int READ_TIMEOUT_US = 1000;
        constexpr int READ_RETRY_MAX = 100;

        bool complete = false;

        for(int repeat = 0;
            repeat < READ_RETRY_MAX;
            repeat++)
        {
            memset(read_buf, 0, sizeof(read_buf));

            const int ret =
                this->Serial_.ReadDataWithTimeout(
                    read_buf,
                    sizeof(read_buf),
                    READ_TIMEOUT_US);

            if(ret < 0)
            {
                return -1;
            }

            if(ret == 0)
            {
                continue;
            }

            read_data.append(
                reinterpret_cast<char*>(read_buf),
                ret);

            if(read_data.find("\r\n") !=
            std::string::npos)
            {
                complete = true;
                break;
            }
        }

        if(!complete)
        {
            return -1;
        }

        // 3. Parse response
        const size_t eol_pos = read_data.find("\r\n");

        if(eol_pos == std::string::npos)
        {
            return -1;
        }

        // Only parse one complete response frame.
        const std::string frame =
            read_data.substr(0, eol_pos);

        std::istringstream stream(frame);

        switch(cmd.command)
        {
            case 'e':
            {
                std::vector<int32_t> encoder_buf(
                    this->no_of_motors_);

                for(int i = 0;
                    i < this->no_of_motors_;
                    i++)
                {
                    if(!(stream >> encoder_buf[i]))
                    {
                        return -1;
                    }
                }

                // Reject unexpected extra tokens.
                std::string extra_token;

                if(stream >> extra_token)
                {
                    return -1;
                }

                // Commit only after the whole frame is valid.
                for(int i = 0;
                    i < this->no_of_motors_;
                    i++)
                {
                    this->motor_data_[i].encoder =
                        encoder_buf[i];
                }

                break;
            }

            case 'z':
            {
                std::vector<float> rpm_buf(
                    this->no_of_motors_);

                for(int i = 0;
                    i < this->no_of_motors_;
                    i++)
                {
                    if(!(stream >> rpm_buf[i]))
                    {
                        return -1;
                    }
                }

                std::string extra_token;

                if(stream >> extra_token)
                {
                    return -1;
                }

                for(int i = 0;
                    i < this->no_of_motors_;
                    i++)
                {
                    this->motor_data_[i].rpm =
                        rpm_buf[i];
                }

                break;
            }

            default:
            {
                return -1;
            }
        }


    return 0;
}


float ArduinoMotorDriver::ReadRPM(int motor) const
{
    const int select = motor - 1;

    if(select < 0)
    {
        return 0.0f;
    }

    const size_t index =
        static_cast<size_t>(select);

    if(index >= this->motor_data_.size())
    {
        return 0.0f;
    }

    return this->motor_data_[index].rpm;
}

std::int32_t ArduinoMotorDriver::ReadEncoder(int motor) const
{
    const int select = motor - 1;

    if(select < 0)
    {
        return 0;
    }

    const size_t index =
        static_cast<size_t>(select);

    if(index >= this->motor_data_.size())
    {
        return 0;
    }

    return this->motor_data_[index].encoder;
}
} // namespace mecanum::motor
