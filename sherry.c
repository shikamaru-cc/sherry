#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stddef.h>

void *srmalloc(size_t sz) {
    void *p = malloc(sz);
    // printf("srmalloc at: %ld\n", p);
    return p;
}

void srfree(void *p) {
    // printf("srfree at: %ld\n", p);
    free(p);
}

typedef jmp_buf srctx;

#define SHERRY_STACK_SIZE 4096

typedef struct srRoutine {
    int rid; /* routine id */
    srctx ctx;
    void *(*fn)(void *);
    void *arg;
    struct srRoutine *next;
} srRoutine;

int nextrid = 0;

srRoutine *newRoutine(void *(*fn)(void *), void *arg) {
    /* p is the end of the routine's stack. */
    char *p = srmalloc(sizeof(srRoutine) + SHERRY_STACK_SIZE);

    /* the start SHERRY_STACK_SIZE memory is used as the routine's
     * stack. */
    srRoutine *r = (srRoutine *)(p + SHERRY_STACK_SIZE);
    r->rid = nextrid; nextrid++;
    r->fn = fn;
    r->arg = arg;
    r->next = NULL;

    return r;
}

void freeRoutine(srRoutine *r) {
    char *p = (char *)r - SHERRY_STACK_SIZE;
    srfree(p);
}

/* TODO: now is O(n), make it O(1) */
void enqueueRoutine(srRoutine **queue, srRoutine *new) {
    while (*queue)
        queue = &(*queue)->next;
    *queue = new;
}

srRoutine *dequeueRoutine(srRoutine **queue) {
    srRoutine *ret = *queue;
    if (ret == NULL) return NULL;
    *queue = ret->next;
    ret->next = NULL;
    return ret;
}

int numRoutines(srRoutine *queue) {
    int i = 0;
    while (queue) {
        i++;
        queue = queue->next;
    }
    return i;
}

void *getstack(srRoutine *r) { return r; }

typedef struct srScheduler {
    srRoutine *rtmain; /* the fake main routine */
    srRoutine *rtrunning; /* the current running routine */
    srRoutine *rtready;
} srScheduler;

srScheduler *S;

char _tmpstack[4096];
char *tmpstack = _tmpstack + sizeof(_tmpstack);

/* resume one routine from ready list */
void resume(void) {
    srRoutine *next = dequeueRoutine(&S->rtready);
    if (next != NULL) {
        S->rtrunning = next;
        siglongjmp(next->ctx, 1);
    }
}

#define switchsp(sp) do { \
        asm volatile ("movq %0, %%rsp" : : "r"(sp)); \
    } while(0)

int srspawn(void *(*fn)(void *), void *arg) {

    srRoutine *newr = newRoutine(fn, arg);

    /* interrupt current running routine */
    srRoutine *curr = S->rtrunning;
    enqueueRoutine(&S->rtready, curr);

    if (sigsetjmp(curr->ctx, 0)) {
        /* If the return value of sigsetjmp is greater than zero,
         * it means that we go back to this saved context
         * by siglongjmp, then we should return and continue
         * the interrupted routine. */
        return newr->rid;
    }

    /* The return value of sigsetjmp is zero means that we continue the
     * current context, spawn the new routine and run it. */
    S->rtrunning = newr;

    switchsp(getstack(newr));
    S->rtrunning->fn(S->rtrunning->arg);

    /* we should not free the memory of current stack, so we switch to
     * the global temp stack first. */
    switchsp(tmpstack);
    freeRoutine(S->rtrunning);

    resume();

    return 0; // never reach here
}

void sryield(void) {
    srRoutine *curr = S->rtrunning;
    enqueueRoutine(&S->rtready, curr);
    if (!sigsetjmp(curr->ctx, 0))
        resume();
}

void srinit(void) {
    S = srmalloc(sizeof(srScheduler));
    S->rtmain = newRoutine(NULL, NULL);
    S->rtrunning = S->rtmain;
    S->rtready = NULL;
}

void srexit(void) {
    freeRoutine(S->rtmain);
    srfree(S);
}

struct testArg {
    const char *name;
    int cnt;
};

void *testf(void *arg) {
    struct testArg *targ = (struct testArg *)arg;
    for (int i = 1; i <= targ->cnt; i++) {
        printf("%s: %d\n", targ->name, i);
        sryield();
    }
    return NULL;
}

int main(void) {
    srinit();

    struct testArg arg1 = {"sherry routine 1", 5};
    struct testArg arg2 = {"sherry routine 2", 3};
    struct testArg arg3 = {"sherry routine 3", 10};
    struct testArg arg4 = {"sherry routine 4", 2};

    srspawn(testf, &arg1);
    srspawn(testf, &arg2);
    srspawn(testf, &arg3);
    srspawn(testf, &arg4);

    while (numRoutines(S->rtready) > 0)
        sryield();

    srexit();
}
