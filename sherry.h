#ifndef SHERRY_H
#define SHERRY_H

#include <stddef.h>
#include <sys/socket.h>

#include "sds.h"

struct message {
    int sender;
    int recver;
    int msgtype;
    sds payload;
};

#define SHERRY_MAIN_ID 0

typedef void (*sherry_fn_t)(int, char **);

/* Unused arguments generate annoying warnings... */
#define SHERRY_NOTUSED(V) ((void) V)

int sherrySpawn(sherry_fn_t fn, int argc, char **argv);
void sherryYield(void);
void sherrySendMsg(int dst, int msgtype, sds payload);
struct message *sherryReceiveMsg(void);

int sherryAccept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
ssize_t sherryRead(int fd, void *buf, size_t count);
ssize_t sherryWrite(int fd, const void *buf, size_t count);

void sherryInit(void);
void sherryExit(void);

#endif // !SHERRY_H
