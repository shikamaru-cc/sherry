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
        sherry_yield();
    }
}

void test_simple(void) {
    sherry_init();

    char *arg1[] = {"sherry routine 1", "5"};
    char *arg2[] = {"sherry routine 2", "3"};
    char *arg3[] = {"sherry routine 3", "10"};
    char *arg4[] = {"sherry routine 4", "2"};

    sherry_spawn(testf, 2, arg1);
    sherry_spawn(testf, 2, arg2);
    sherry_spawn(testf, 2, arg3);
    sherry_spawn(testf, 2, arg4);

    sherry_exit();
}

#define MSG_PING 100

void pingf(int argc, char **argv) {
    assert(argc == 1);
    int dst = atoi(argv[0]);
    printf("PING!\n");
    sherry_msg_send(dst, MSG_PING, NULL);
    /* send the second message which will not be processed,
     * but we should pass the valgrind test. */
    printf("PING!\n");
    sherry_msg_send(dst, MSG_PING, NULL);
}

void pongf(int argc, char **argv) {
    SHERRY_NOTUSED(argv);
    assert(argc == 0);
    struct sherry_msg *msg = sherry_msg_recv();
    if (msg->msgtype == MSG_PING) {
        printf("PONG!\n");
    }
    free(msg);
}

void test_message(void) {
    sherry_init();

    char str[10];

    int sid = sherry_spawn(pongf, 0, NULL);

    snprintf(str, sizeof(str), "%d", sid);
    char *pingargs[] = {str};
    sherry_spawn(pingf, 1, pingargs);

    sherry_exit();
}

void benchf(int argc, char **argv) {
    assert(argc == 1);
    int cnt = atoi(argv[0]);
    for (int i = 0; i < cnt; i++)
        sherry_yield();
}

void bench_switch(void) {
    sherry_init();
    char *argv[] = {"1000"};
    for (size_t i = 0; i < 10000; i++)
        sherry_spawn(benchf, 1, argv);
    sherry_exit();
}

int main(void) {
    test_message();
    test_simple();
    bench_switch();
}
