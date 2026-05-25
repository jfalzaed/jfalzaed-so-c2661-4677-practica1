#include "proto.h"
#include <string.h>
#include <unistd.h>
#include <errno.h>

int msg_send(int fd, const char *json_str) {
    size_t len = strlen(json_str);
    if (len + 1 > MSG_MAX_LEN) return -1;

    char buf[MSG_MAX_LEN];
    memcpy(buf, json_str, len);
    buf[len] = '\n';

    size_t total = len + 1;
    size_t sent  = 0;
    while (sent < total) {
        ssize_t n = write(fd, buf + sent, total - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

ssize_t msg_recv(int fd, char *buf, size_t bufsz) {
    size_t pos = 0;
    while (pos < bufsz - 1) {
        ssize_t n = read(fd, buf + pos, 1);
        if (n < 0) {
            /* Propagar EINTR al caller para que pueda verificar señales.
             * Sólo reintentar si ya leímos parte del mensaje (no perder datos). */
            if (errno == EINTR && pos == 0) return -1;
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            buf[pos] = '\0';
            return (pos == 0) ? 0 : (ssize_t)pos;
        }
        if (buf[pos] == '\n') {
            buf[pos] = '\0';
            return (ssize_t)pos;
        }
        pos++;
    }
    buf[pos] = '\0';
    return (ssize_t)pos;
}
