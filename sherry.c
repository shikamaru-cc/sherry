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

void *srrealloc(void *ptr, size_t sz) {
    return realloc(ptr, sz);
}

void srfree(void *p) {
    // printf("srfree at: %ld\n", p);
    free(p);
}

/* Takes a pointer to a member variable and computes pointer to the structure
 * that contains it. 'type' is type of the structure, not the member.
 * NOTE: This macro code from github.com/sustrik/libmill. */
#define get_cont(ptr, type, member) \
    (ptr ? ((type*) (((char*) ptr) - offsetof(type, member))) : NULL)

struct queueNode {
    struct queueNode *prev;
    struct queueNode *next;
};

struct queue {
    struct queueNode *head;
    struct queueNode *tail;
};

void queueInit(struct queue *q) {
    q->head = NULL;
    q->tail = NULL;
}

static inline int queueEmpty(struct queue *q) { return q->head == NULL; }

void enqueue(struct queue *q, struct queueNode *node) {
    node->prev = q->tail;
    node->next = NULL;
    q->tail = node;
    if (node->prev != NULL)
        node->prev->next = node;
    if (q->head == NULL)
        q->head = node;
}

struct queueNode *dequeue(struct queue *q) {
    struct queueNode *node = q->head;

    if (node == NULL)
        return NULL;

    if (node->next != NULL)
        node->next->prev = NULL;

    q->head = node->next;
    q->tail = q->tail == node ? NULL : q->tail;

    node->prev = NULL;
    node->next = NULL;

    return node;
}

typedef jmp_buf srctx;

#define SHERRY_STACK_SIZE 4096

typedef struct srRoutine {
    int rid; /* routine id */
    srctx ctx;
    void *(*fn)(void *);
    void *arg;
    /* srRoutine is a doule-linked list based queue in the scheduler now. */
    struct queueNode qnode;
} srRoutine;

srRoutine *newRoutine(int rid, void *(*fn)(void *), void *arg) {
    /* p is the end of the routine's stack. */
    char *p = srmalloc(sizeof(srRoutine) + SHERRY_STACK_SIZE);
    /* the start SHERRY_STACK_SIZE bytes are used as the routine's
     * stack. */
    srRoutine *r = (srRoutine *)(p + SHERRY_STACK_SIZE);
    r->rid = rid;
    r->fn = fn;
    r->arg = arg;
    r->qnode.prev = NULL;
    r->qnode.next = NULL;
    return r;
}

void freeRoutine(srRoutine *r) {
    char *p = (char *)r - SHERRY_STACK_SIZE;
    srfree(p);
}

void *getstack(srRoutine *r) { return r; }

typedef struct srScheduler {
    int sizeroutines;     // the size of array routines
    srRoutine **routines; // all register routines
    srRoutine *main;      // the fake main routine
    srRoutine *running;   // the current running routine
    struct queue readyq;     // routines ready to run
} srScheduler;

srScheduler *S;

char _tmpstack[4096];
char *tmpstack = _tmpstack + sizeof(_tmpstack);

/* add a new routine to routines array. */
srRoutine *addRoutine(void *(*fn)(void *), void *arg) {
    /* TODO: O(n) now, make it O(1)? */
    int rid = -1;
    for (int i = 0; i < S->sizeroutines; i++) {
        if (S->routines[i] == NULL) {
            rid = i;
            break;
        }
    }
    /* there is no available slot, realloc routines array. */
    if (rid == -1) {
        size_t oldsize = S->sizeroutines;
        size_t newsize = S->sizeroutines * 2;
        S->routines = srrealloc(S->routines, newsize * sizeof(srRoutine *));
        memset(S->routines + oldsize, 0,
            (newsize - oldsize) * sizeof(srRoutine *));
        S->sizeroutines = newsize;
        rid = oldsize;
    }
    srRoutine *rt = newRoutine(rid, fn, arg);
    S->routines[rid] = rt;
    return rt;
}

void delRoutine(int rid) {
    if (rid < 0 || rid >= S->sizeroutines || S->routines[rid] == NULL)
        return;

    srRoutine *rt = S->routines[rid];
    freeRoutine(rt);
    S->routines[rid] = NULL;

    /* TODO: shrink the array size. */
}

/* resume one routine from ready list */
void resume(void) {
    struct queueNode *node = dequeue(&S->readyq);
    srRoutine *next = get_cont(node, srRoutine, qnode);
    if (next != NULL) {
        S->running = next;
        siglongjmp(next->ctx, 1);
    }
}

#define switchsp(sp) do { \
        asm volatile ("movq %0, %%rsp" : : "r"(sp)); \
    } while(0)

int srspawn(void *(*fn)(void *), void *arg) {

    srRoutine *newr = addRoutine(fn, arg);

    /* interrupt current running routine */
    srRoutine *curr = S->running;
    enqueue(&S->readyq, &curr->qnode);

    if (sigsetjmp(curr->ctx, 0)) {
        /* If the return value of sigsetjmp is greater than zero,
         * it means that we go back to this saved context
         * by siglongjmp, then we should return and continue
         * the interrupted routine. */
        return newr->rid;
    }

    /* The return value of sigsetjmp is zero means that we continue the
     * current context, spawn the new routine and run it. */
    S->running = newr;

    /* switch stack here, then we cannot access fn and arg from stack
     * params, we call it through the global scheduler. */
    switchsp(getstack(newr));
    S->running->fn(S->running->arg);

    /* the routine ends, release resources and give control back to
     * scheduler. */

    /* we should not free the memory of current stack, so we switch to
     * the global temp stack first. */
    switchsp(tmpstack);
    delRoutine(S->running->rid);

    resume();

    return 0; // never reach here
}

void sryield(void) {
    srRoutine *curr = S->running;
    enqueue(&S->readyq, &curr->qnode);
    if (!sigsetjmp(curr->ctx, 0))
        resume();
}

void srinit(void) {
    S = srmalloc(sizeof(srScheduler));

    const size_t sz = 1000; // initial S->routines size
    S->sizeroutines = sz;
    S->routines = srmalloc(sizeof(srRoutine *) * sz);
    memset(S->routines, 0, sizeof(srRoutine *) * sz);

    /* set current running to the fake main routine. */
    S->main = addRoutine(NULL, NULL);
    S->running = S->main;

    queueInit(&S->readyq);
}

void srexit(void) {
    delRoutine(S->main->rid);
    srfree(S->routines);
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

void testsimple(void) {
    struct testArg arg1 = {"sherry routine 1", 5};
    struct testArg arg2 = {"sherry routine 2", 3};
    struct testArg arg3 = {"sherry routine 3", 10};
    struct testArg arg4 = {"sherry routine 4", 2};

    srspawn(testf, &arg1);
    srspawn(testf, &arg2);
    srspawn(testf, &arg3);
    srspawn(testf, &arg4);

    while (!queueEmpty(&S->readyq))
        sryield();
}

void *benchf(void *arg) {
    size_t cnt = (size_t)arg;
    for (size_t i = 0; i < cnt; i++)
        sryield();
    return NULL;
}

void benchswitch(void) {
    for (size_t i = 0; i < 10000; i++)
        srspawn(benchf, (void *)1000);
    while (!queueEmpty(&S->readyq))
        sryield();
}

int main(void) {
    srinit();
    // testsimple();
    benchswitch();
    srexit();
}
