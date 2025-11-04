#include "platform.h"
#include "common.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int send_all(int sock, const void *buffer, size_t length) {
    const char *data = (const char *)buffer;
    size_t total_sent = 0;
    while (total_sent < length) {
        ssize_t sent = send(sock, data + total_sent, length - total_sent, 0);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (sent == 0) {
            return -1;
        }
        total_sent += (size_t)sent;
    }
    return 0;
}

int recv_all(int sock, void *buffer, size_t length) {
    char *data = (char *)buffer;
    size_t total_read = 0;
    while (total_read < length) {
        ssize_t received = recv(sock, data + total_read, length - total_read, 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (received == 0) {
            return -1;
        }
        total_read += (size_t)received;
    }
    return 0;
}

int recv_line(int sock, char *buffer, size_t max_len) {
    size_t pos = 0;
    while (pos + 1 < max_len) {
        char c;
        ssize_t received = recv(sock, &c, 1, 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (received == 0) {
            return -1;
        }
        if (c == '\n') {
            buffer[pos] = '\0';
            return 0;
        }
        buffer[pos++] = c;
    }
    errno = EMSGSIZE;
    return -1;
}

int send_fmt(int sock, const char *fmt, ...) {
    char buffer[MCSYNC_MAX_LINE];
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        errno = EMSGSIZE;
        return -1;
    }
    return send_all(sock, buffer, (size_t)written);
}
