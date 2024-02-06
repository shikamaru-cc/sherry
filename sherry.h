#ifndef SHERRY_H
#define SHERRY_H

#include <stddef.h>
#include <sys/socket.h>

struct message {
    int senderid;
    int receiverid;
    int msgtype;
    void *payload;
    size_t size;
};

#define SHERRY_MAIN_ID 0

int sherrySpawn(void *(*fn)(void *), void *arg);
void sherryYield(void);
void sherrySendMsg(int dst, int msgtype, void *payload, size_t size);
struct message *sherryReceiveMsg(void);

int sherryAccept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
ssize_t sherryRead(int fd, void *buf, size_t count);
ssize_t sherryWrite(int fd, const void *buf, size_t count);

void sherryInit(void);
void sherryExit(void);

#endif // !SHERRY_H
