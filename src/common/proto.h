#pragma once
#include <sys/types.h>
#include <stddef.h>

/* Tamaño máximo de un mensaje JSON (incluyendo el '\n' final). */
#define MSG_MAX_LEN 4096

/*
 * Envía json_str seguido de '\n' al descriptor fd.
 * Retorna 0 en éxito, -1 en error.
 */
int msg_send(int fd, const char *json_str);

/*
 * Lee un mensaje JSON terminado en '\n' del descriptor fd.
 * Almacena el resultado (sin el '\n') en buf.
 * Retorna la longitud leída en éxito, 0 en EOF, -1 en error.
 */
ssize_t msg_recv(int fd, char *buf, size_t bufsz);
