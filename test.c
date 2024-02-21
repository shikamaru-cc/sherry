#include <stdio.h>
#include <stdlib.h>
#include "sherry.h"

struct testArg {
    const char *name;
    int cnt;
};

void *testf(void *arg) {
    struct testArg *targ = (struct testArg *)arg;
    for (int i = 1; i <= targ->cnt; i++) {
        printf("%s: %d\n", targ->name, i);
        sherryYield();
    }
    return NULL;
}

void testsimple(void) {
    sherryInit();

    struct testArg arg1 = {"sherry routine 1", 5};
    struct testArg arg2 = {"sherry routine 2", 3};
    struct testArg arg3 = {"sherry routine 3", 10};
    struct testArg arg4 = {"sherry routine 4", 2};

    sherrySpawn(testf, &arg1);
    sherrySpawn(testf, &arg2);
    sherrySpawn(testf, &arg3);
    sherrySpawn(testf, &arg4);

    sherryExit();
}

#define MSG_PING 100

void *pingf(void *arg) {
    printf("PING!\n");
    sherrySendMsg((int)arg, MSG_PING, NULL, 0);
    /* send the second message which will not be processed,
     * but we should pass the valgrind test. */
    printf("PING!\n");
    sherrySendMsg((int)arg, MSG_PING, NULL, 0);
    return NULL;
}

void *pongf(void *arg) {
    struct message *msg = sherryReceiveMsg();
    if (msg->msgtype == MSG_PING) {
        printf("PONG!\n");
    }
    free(msg);
    return NULL;
}

void testMessage(void) {
    sherryInit();
    int sid = sherrySpawn(pongf, NULL);
    int rid = sherrySpawn(pingf, (void *)sid);
    sherryExit();
}

void *benchf(void *arg) {
    size_t cnt = (size_t)arg;
    for (size_t i = 0; i < cnt; i++)
        sherryYield();
    return NULL;
}

void benchswitch(void) {
    sherryInit();
    for (size_t i = 0; i < 10000; i++)
        sherrySpawn(benchf, (void *)1000);
    sherryExit();
}

int main(void) {
    // testMessage();
    // testsimple();
    benchswitch();
}
