#pragma once
#ifdef __cplusplus
extern "C" {
#endif
int serial_open(const char* device, unsigned int baud);
int serial_close(int fd);
/* Nonblocking: 0 = no bytes, negative errno = failure, positive = bytes. */
int serial_read_data(int fd, unsigned char* data, int length);
int serial_write_data(int fd, const unsigned char* data, int length);
#ifdef __cplusplus
}
#endif
