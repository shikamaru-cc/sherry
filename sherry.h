#ifndef SHERRY_H
#define SHERRY_H

#include <stddef.h>

struct message {
    int senderid;
    int receiverid;
    int msgtype;
    void *payload;
    size_t size;
};

int sherrySpawn(void *(*fn)(void *), void *arg);
void sherryYield(void);
void sherrySendMsg(int dst, int msgtype, void *payload, size_t size);
struct message *sherryReceiveMsg(void);
void sherryInit(void);
void sherryExit(void);

#endif // !SHERRY_H
