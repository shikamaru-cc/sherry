#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stddef.h>

#include "sherry.h"

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

struct listNode {
    struct listNode *prev;
    struct listNode *next;
};

struct list {
    struct listNode *head;
    struct listNode *tail;
};

/* True is the list has no items. */
#define listEmpty(self) (!((self)->head))

/* Returns iterator to the first item in the list or NULL if
   the list is empty. */
#define listBegin(self) ((self)->head)

/* Returns iterator to one past the item pointed to by 'it'. */
#define listNext(it) ((it)->next)

void listInit(struct list *l) {
    l->head = NULL;
    l->tail = NULL;
}

void listInsert(struct list *self, struct listNode *item, struct listNode *it) {
    item->prev = it ? it->prev : self->tail;
    item->next = it;
    if(item->prev)
        item->prev->next = item;
    if(item->next)
        item->next->prev = item;
    if(!self->head || self->head == it)
        self->head = item;
    if(!it)
        self->tail = item;
}

struct listNode *listErase(struct list *self, struct listNode *item) {
    struct listNode *next;

    if(item->prev)
        item->prev->next = item->next;
    else
        self->head = item->next;
    if(item->next)
        item->next->prev = item->prev;
    else
        self->tail = item->prev;

    next = item->next;

    item->prev = NULL;
    item->next = NULL;

    return next;
}

void listEnqueue(struct list *q, struct listNode *node) {
    listInsert(q, node, NULL);
}

struct listNode *listDequeue(struct list *q) {
    struct listNode *node = listBegin(q);
    if (node == NULL)
        return NULL;
    listErase(q, node);
    return node;
}

struct messageNode {
    struct message msg;
    struct listNode node;
};

typedef jmp_buf srctx;

#define SHERRY_STACK_SIZE 4096

/* all posible actor status */
#define SHERRY_STATE_DEAD       0
#define SHERRY_STATE_READY      1
#define SHERRY_STATE_RUNNING    2
#define SHERRY_STATE_WAIT_MSG   3

typedef struct actor {
    int rid;              // routine id
    int status;           // actor current status
    srctx ctx;            // routine running context
    void *(*fn)(void *);  // main function need to run
    void *arg;            // args for main function
    struct list mailbox;  // a queue of received messages in this actor
    struct listNode node; // actor is a doule-linked list based
                          // queue in the scheduler now.
} actor;

actor *newActor(int rid, void *(*fn)(void *), void *arg) {
    /* p is the end of the routine's stack. */
    char *p = srmalloc(sizeof(actor) + SHERRY_STACK_SIZE);
    /* the start SHERRY_STACK_SIZE bytes are used as the routine's
     * stack. */
    actor *r = (actor *)(p + SHERRY_STACK_SIZE);
    r->rid = rid;
    r->status = SHERRY_STATE_READY;
    r->fn = fn;
    r->arg = arg;
    r->node.prev = NULL;
    r->node.next = NULL;
    listInit(&r->mailbox);
    return r;
}

void freeActor(actor *r) {
    char *p = (char *)r - SHERRY_STACK_SIZE;
    srfree(p);
}

void *actorStack(actor *r) { return r; }

typedef struct scheduler {
    int sizeactors;     // the size of array routines
    actor **actors; // all register routines
    actor *main;      // the fake main routine
    actor *running;   // the current running routine
    struct list readyq; // routines ready to run
    struct list waitq; // blocked actors
} scheduler;

scheduler *S;

char _tmpstack[4096];
char *tmpstack = _tmpstack + sizeof(_tmpstack);

/* add a new routine to routines array. */
actor *addActor(void *(*fn)(void *), void *arg) {
    /* TODO: O(n) now, make it O(1)? */
    int rid = -1;
    for (int i = 0; i < S->sizeactors; i++) {
        if (S->actors[i] == NULL) {
            rid = i;
            break;
        }
    }
    /* there is no available slot, realloc routines array. */
    if (rid == -1) {
        size_t oldsize = S->sizeactors;
        size_t newsize = S->sizeactors * 2;
        S->actors = srrealloc(S->actors, newsize * sizeof(actor *));
        memset(S->actors + oldsize, 0,
            (newsize - oldsize) * sizeof(actor *));
        S->sizeactors = newsize;
        rid = oldsize;
    }
    actor *rt = newActor(rid, fn, arg);
    S->actors[rid] = rt;
    return rt;
}

void delActor(int rid) {
    if (rid < 0 || rid >= S->sizeactors || S->actors[rid] == NULL)
        return;

    actor *rt = S->actors[rid];
    freeActor(rt);
    S->actors[rid] = NULL;

    /* TODO: shrink the array size. */
}

/* set the actor to running state and enqueue it to readyq. */
void setReady(actor *a) {
    a->status = SHERRY_STATE_READY;
    listEnqueue(&S->readyq, &a->node);
}

void setRunning(actor *a) {
    a->status = SHERRY_STATE_RUNNING;
    S->running = a;
}

/* resume one routine from ready list */
void resume(void) {
    struct listNode *node = listDequeue(&S->readyq);
    actor *next = get_cont(node, actor, node);
    if (next != NULL) {
        setRunning(next);
        siglongjmp(next->ctx, 1);
    }
}

void yield(void) {
    actor *a = S->running;
    if (!sigsetjmp(a->ctx, 0))
        resume();
}

#define switchsp(sp) do { \
        asm volatile ("movq %0, %%rsp" : : "r"(sp)); \
    } while(0)

int sherrySpawn(void *(*fn)(void *), void *arg) {

    actor *newr = addActor(fn, arg);

    /* interrupt current running routine */
    actor *curr = S->running;
    setReady(curr);

    if (sigsetjmp(curr->ctx, 0)) {
        /* If the return value of sigsetjmp is greater than zero,
         * it means that we go back to this saved context
         * by siglongjmp, then we should return and continue
         * the interrupted routine. */
        return newr->rid;
    }

    /* The return value of sigsetjmp is zero means that we continue the
     * current context, spawn the new routine and run it. */
    setRunning(newr);

    /* switch stack here, then we cannot access fn and arg from stack
     * params, we call it through the global scheduler. */
    switchsp(actorStack(newr));
    S->running->fn(S->running->arg);

    /* the routine ends, release resources and give control back to
     * scheduler. */

    /* we should not free the memory of current stack, so we switch to
     * the global temp stack first. */
    switchsp(tmpstack);
    delActor(S->running->rid);

    resume();

    return 0; // never reach here
}

void sherryYield(void) {
    setReady(S->running);
    yield();
}

/* Send a message to the receiver actor. This function never blocks the
 * running actor. */
void sherrySendMsg(int dst, int msgtype, void *payload, size_t size) {
    actor *sender = S->running;
    actor *receiver = S->actors[dst]; // TODO: check the dst idx

    struct messageNode *msg = srmalloc(sizeof(*msg));
    msg->msg.senderid = sender->rid;
    msg->msg.receiverid = receiver->rid;
    msg->msg.msgtype = msgtype;
    msg->msg.payload = payload;
    msg->msg.size = size;
    msg->node.prev = NULL;
    msg->node.next = NULL;
    listEnqueue(&receiver->mailbox, &msg->node);

    if (receiver->status == SHERRY_STATE_WAIT_MSG) {
        listErase(&S->waitq, &receiver->node);
        setReady(receiver);
    }
}

/* Running actor receives a message from its mailbox. If there is nothing
 * in the mailbox, the running actor will yield and be invoked again when
 * there is something in the mailbox. */
struct message *sherryReceiveMsg(void) {
    actor *a = S->running;
    if (listEmpty(&a->mailbox)) {
        a->status = SHERRY_STATE_WAIT_MSG;
        listEnqueue(&S->waitq, &a->node);
        yield();
    }
    struct listNode *node = listDequeue(&a->mailbox);
    struct messageNode *msg = get_cont(node, struct messageNode, node);
    return (struct message *)msg;
}

void sherryInit(void) {
    S = srmalloc(sizeof(scheduler));

    const size_t sz = 1000; // initial S->routines size
    S->sizeactors = sz;
    S->actors = srmalloc(sizeof(actor *) * sz);
    memset(S->actors, 0, sizeof(actor *) * sz);

    /* set current running to the fake main routine. */
    S->main = addActor(NULL, NULL);
    S->running = S->main;

    listInit(&S->readyq);
    listInit(&S->waitq);
}

void sherryExit(void) {
    /* wait all ready actors exit. */
    while (!listEmpty(&S->readyq))
        sherryYield();
    delActor(S->main->rid);
    srfree(S->actors);
    srfree(S);
}

