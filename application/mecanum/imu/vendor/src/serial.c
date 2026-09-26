/* Adapted from wt90c1c v2.0 serial.c, Apache-2.0. Retains Linux 8N1 UART.
 * Fixes: failure cleanup, nonblocking/error semantics, explicit baud validation,
 * CLOEXEC and exclusive ownership; no stdin checks or unsolicited sensor writes. */
#include "wt901c/serial.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <termios.h>
#include <unistd.h>

int serial_close(int fd) { return fd < 0 || close(fd) == 0 ? 0 : -errno; }
int serial_open(const char* device, unsigned int baud) {
    speed_t speed;
    switch (baud) {
#define BAUD(n) case n: speed=B##n; break
        BAUD(2400); BAUD(4800); BAUD(9600); BAUD(19200); BAUD(38400);
        BAUD(57600); BAUD(115200); BAUD(230400); BAUD(460800); BAUD(921600);
#undef BAUD
        default: return -EINVAL;
    }
    int fd=open(device,O_RDWR|O_NOCTTY|O_NONBLOCK|O_CLOEXEC);
    if (fd<0) return -errno;
    struct termios config;
    if (flock(fd,LOCK_EX|LOCK_NB) || tcgetattr(fd,&config)) goto fail;
    cfmakeraw(&config);
    config.c_cflag &= ~(CSIZE|PARENB|CSTOPB|CRTSCTS);
    config.c_cflag |= CS8|CLOCAL|CREAD;
    config.c_cc[VTIME]=0; config.c_cc[VMIN]=0;
    if (cfsetispeed(&config,speed) || cfsetospeed(&config,speed) ||
        tcsetattr(fd,TCSANOW,&config) || tcflush(fd,TCIFLUSH)) goto fail;
    return fd;
fail: {
    int error=errno;
    close(fd);
    return -error;
    }
}
int serial_read_data(int fd, unsigned char* data, int length) {
    if (fd<0 || length<=0 || !data) return -EINVAL;
    struct pollfd p={fd,POLLIN,0};
    int result=poll(&p,1,0);
    if (result<0) return errno==EINTR ? 0 : -errno;
    if (p.revents&(POLLHUP|POLLERR|POLLNVAL)) return -EIO;
    if (!result || !(p.revents&POLLIN)) return 0;
    result=(int)read(fd,data,(size_t)length);
    if (result<0) return errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR ? 0 : -errno;
    return result; // VMIN=0 permits an empty read
}
int serial_write_data(int fd, const unsigned char* data, int length) {
    if (fd<0 || !data || length<=0) return -EINVAL;
    int result=(int)write(fd,data,(size_t)length);
    return result<0 ? -errno : result;
}
