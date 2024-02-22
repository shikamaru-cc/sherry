#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../sherry.h"

struct testArg {
    const char *name;
    int cnt;
};

void testf(int argc, char **argv) {
    assert(argc == 2);
    int cnt = atoi(argv[1]);
    for (int i = 1; i <= cnt; i++) {
        printf("actor %s: %d\n", argv[0], i);
        sherryYield();
    }
}

void testsimple(void) {
    sherryInit();

    char *arg1[] = {"sherry routine 1", "5"};
    char *arg2[] = {"sherry routine 2", "3"};
    char *arg3[] = {"sherry routine 3", "10"};
    char *arg4[] = {"sherry routine 4", "2"};

    sherrySpawn(testf, 2, arg1);
    sherrySpawn(testf, 2, arg2);
    sherrySpawn(testf, 2, arg3);
    sherrySpawn(testf, 2, arg4);

    sherryExit();
}

#define MSG_PING 100

void pingf(int argc, char **argv) {
    assert(argc == 1);
    int dst = atoi(argv[0]);
    printf("PING!\n");
    sherrySendMsg(dst, MSG_PING, NULL);
    /* send the second message which will not be processed,
     * but we should pass the valgrind test. */
    printf("PING!\n");
    sherrySendMsg(dst, MSG_PING, NULL);
}

void pongf(int argc, char **argv) {
    SHERRY_NOTUSED(argv);
    assert(argc == 0);
    struct message *msg = sherryReceiveMsg();
    if (msg->msgtype == MSG_PING) {
        printf("PONG!\n");
    }
    free(msg);
}

void testMessage(void) {
    sherryInit();

    char str[10];

    int sid = sherrySpawn(pongf, 0, NULL);

    snprintf(str, sizeof(str), "%d", sid);
    char *pingargs[] = {str};
    sherrySpawn(pingf, 1, pingargs);

    sherryExit();
}

void benchf(int argc, char **argv) {
    assert(argc == 1);
    int cnt = atoi(argv[0]);
    for (int i = 0; i < cnt; i++)
        sherryYield();
}

void benchswitch(void) {
    sherryInit();
    char *argv[] = {"1000"};
    for (size_t i = 0; i < 10000; i++)
        sherrySpawn(benchf, 1, argv);
    sherryExit();
}

int main(void) {
    testMessage();
    testsimple();
    benchswitch();
}
