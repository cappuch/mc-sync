#ifndef MCSYNC_COMMON_H
#define MCSYNC_COMMON_H

#include <stddef.h>

/* max line length */
#define MCSYNC_MAX_LINE 1024

int send_all(int sock, const void *buffer, size_t length);
int recv_all(int sock, void *buffer, size_t length);
int recv_line(int sock, char *buffer, size_t max_len);
int send_fmt(int sock, const char *fmt, ...);

#endif /* MCSYNC_COMMON_H */
