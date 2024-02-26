#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../sherry.h"

struct test_arg {
    const char *prefix;
    int round;
};

void testf(void *argv) {
    struct test_arg *arg = (struct test_arg *)argv;
    for (int i = 1; i <= arg->round; i++) {
        printf("actor %s: %d\n", arg->prefix, i);
        sherry_yield();
    }
}

void test_simple(void) {
    sherry_init();

    struct test_arg arg1 = {"sherry routine 1", 5};
    struct test_arg arg2 = {"sherry routine 2", 3};
    struct test_arg arg3 = {"sherry routine 3", 10};
    struct test_arg arg4 = {"sherry routine 4", 2};

    sherry_spawn(testf, &arg1);
    sherry_spawn(testf, &arg2);
    sherry_spawn(testf, &arg3);
    sherry_spawn(testf, &arg4);

    sherry_exit();
}

#define MSG_PING 100

void pingf(void *argv) {
    unsigned long dst = (unsigned long)argv;
    printf("PING!\n");
    sherry_msg_send(dst, MSG_PING, NULL);
    /* send the second message which will not be processed,
     * but we should pass the valgrind test. */
    printf("PING!\n");
    sherry_msg_send(dst, MSG_PING, NULL);
}

void pongf(void *argv) {
    SHERRY_NOTUSED(argv);
    struct sherry_msg *msg = sherry_msg_recv();
    if (msg->msgtype == MSG_PING) {
        printf("PONG!\n");
    }
    free(msg);
}

void test_message(void) {
    sherry_init();
    intptr_t sid = sherry_spawn(pongf, NULL);
    sherry_spawn(pingf, (void *)sid);
    sherry_exit();
}

void benchf(void *argv) {
    unsigned long cnt = (unsigned long)argv;
    for (unsigned long i = 0; i < cnt; i++)
        sherry_yield();
}

void bench_switch(void) {
    sherry_init();
    for (unsigned long i = 0; i < 10000; i++)
        sherry_spawn(benchf, (void *)i);
    sherry_exit();
}

int main(void) {
    test_simple();
    test_message();
    bench_switch();
}
