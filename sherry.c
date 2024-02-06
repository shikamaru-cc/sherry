#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stddef.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

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

/* ============================================================================
 * Generic double linked list implementation.
 * Most code from github.com/sustrik/libmill.
 * ========================================================================== */

/* Takes a pointer to a member variable and computes pointer to the structure
 * that contains it. 'type' is type of the structure, not the member. */
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

/* Insert node 'item' to a list before the 'it' node. */
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

/* Erase a node from list, the given node must be in the list. Return the
 * next node pointer. */
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

/* Simple double linked list based queue interface. Insert a node at the
 * tail of list. */
void listEnqueue(struct list *q, struct listNode *node) {
    listInsert(q, node, NULL);
}

/* Simple double linked list based queue interface. Pop up the list's 
 * head node. */
struct listNode *listDequeue(struct list *q) {
    struct listNode *node = listBegin(q);
    if (node == NULL)
        return NULL;
    listErase(q, node);
    return node;
}

/* ============================================================================
 * Sherry actor model core implementation.
 * ========================================================================== */

/* A wrapper for struct message that make it a list node and be possible for 
 * maintaining in a queue. */
struct messageNode {
    struct message msg;
    struct listNode node;
};

/* All posible actor status */
#define SHERRY_STATE_DEAD       0
#define SHERRY_STATE_READY      1
#define SHERRY_STATE_RUNNING    2
#define SHERRY_STATE_WAIT_MSG   3
#define SHERRY_STATE_WAIT_RFD   4 // waiting readable fd
#define SHERRY_STATE_WAIT_WFD   5 // waiting writable fd

typedef jmp_buf context;

#define SHERRY_STACK_SIZE 4096

/* Data structure of a actor. Actually, in memory, there is a stack living
 * in the lower address area of the actor struct. The memory model likes:
 *
 * +---------------------------------------------+------------------+
 * |                                       stack |     struct actor |
 * +---------------------------------------------+------------------+
 *
 * So the actual memory size of a actor is sizeof(struct actor) plus the
 * stack size.
 */
typedef struct actor {
    int rid;              // routine id
    int status;           // actor current status
    context ctx;          // actor running context
    void *(*fn)(void *);  // main function need to run
    void *arg;            // args for main function
    struct list mailbox;  // a queue of received messages in this actor
    struct listNode node; // actor is a doule-linked list based
                          // queue in the scheduler now.
} actor;

actor *newActor(int rid, void *(*fn)(void *), void *arg) {
    /* p is the original allocated pointer. */
    char *p = srmalloc(sizeof(actor) + SHERRY_STACK_SIZE);
    /* The first SHERRY_STACK_SIZE bytes are used as the actor's
     * stack. See the memory model above. */
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
    /* Go back to the original allocated pointer. */
    char *p = (char *)r - SHERRY_STACK_SIZE;
    srfree(p);
}

/* The pointer to the struct actor is also the start address of the stack */
void *actorStack(actor *r) { return r; }

#define SHERRY_MAX_FD 1024

typedef struct scheduler {
    int sizeactors;     // the size of array actors
    actor **actors;     // all register actors, indexed by actor id
    actor *main;        // the fake main actor
    actor *running;     // the current running actor
    struct list readyq; // actors ready to run
    struct list waitingmsg; // actors blocked on mailbox
    struct list *waitwrite; // maps fd to a list of actors blocked on write fd
    struct list *waitread; // maps fd a list of actors blocked on read fd
} scheduler;

scheduler *S;

/* Add a new actor to actors array. */
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

/* Delete an actor from actors array. */
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

/* Unblock all actors in the given list. */
void unblockAllActors(struct list *list) {
    struct listNode *node;
    actor *a;
    while (!listEmpty(list)) {
        node = listDequeue(list);
        a = get_cont(node, actor, node);
        setReady(a);
    }
}

void pollFile(void) {
    fd_set rfds;
    fd_set wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);

    for (int fd = 0; fd < SHERRY_MAX_FD; fd++) {
        if (!listEmpty(&S->waitwrite[fd])) FD_SET(fd, &wfds);
        if (!listEmpty(&S->waitread[fd]))  FD_SET(fd, &rfds);
    }

    int ret = select(SHERRY_MAX_FD + 1, &rfds, &wfds, NULL, NULL);
    if (ret == -1) {
        perror("select error()");
        exit(1);
    }

    for (int fd = 0; fd < SHERRY_MAX_FD; fd++) {
        if (FD_ISSET(fd, &rfds))
            unblockAllActors(&S->waitread[fd]);
        if (FD_ISSET(fd, &wfds))
            unblockAllActors(&S->waitwrite[fd]);
    }
}

/* Resume one actor in the ready queue. */
void resume(void) {
    while (1) {
        struct listNode *node = listDequeue(&S->readyq);
        actor *next = get_cont(node, actor, node);
        if (next != NULL) {
            setRunning(next);
            siglongjmp(next->ctx, 1);
        }
        pollFile();
    }
}

/* Yield current running actor, save the context and go to resume for running
 * the next one. Note that, this yield function would not do any extra work,
 * other work like enqueueing to the ready queue or wait queue should be done
 * before calling this function. */
void yield(void) {
    actor *a = S->running;
    if (!sigsetjmp(a->ctx, 0))
        resume();
}

/* Assembly level code to switch current stack. This macro should satisfy any
 * supported architectures. */
#define switchsp(sp) do { \
        asm volatile ("movq %0, %%rsp" : : "r"(sp)); \
    } while(0)

/* Spawn a new actor, interrupt the current running actor and give the control
 * to the new spawned one. */
int sherrySpawn(void *(*fn)(void *), void *arg) {

    actor *newactor = addActor(fn, arg);

    /* interrupt current running actor */
    actor *curr = S->running;
    setReady(curr);

    if (sigsetjmp(curr->ctx, 0)) {
        /* If the return value of sigsetjmp is greater than zero,
         * it means that we go back to this saved context
         * by siglongjmp, then we should return and continue
         * the interrupted routine. */
        return newactor->rid;
    }

    /* The return value of sigsetjmp is zero means that we continue the
     * current context, spawn the new actor and run it. */
    setRunning(newactor);

    /* Switch stack here, then we cannot access fn and arg from stack params,
     * so we call it through the global scheduler. */
    switchsp(actorStack(newactor));
    S->running->fn(S->running->arg);

    /* The actor exits, we should release resources and give control back to
     * scheduler. We should not free the memory of current stack, so we keep
     * a temp stack as a static variable and switch to it before we do the
     * cleanup job. */

    static char _tmpstack[4096];
    static char *tmpstack = _tmpstack + sizeof(_tmpstack);

    switchsp(tmpstack);
    delActor(S->running->rid);

    /* Give the control back to scheduler. */
    resume();

    return 0; // never reach here
}

/* Put the current running actor to ready queue and yield. */
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

    /* Revoke the receiver actor if it is blocked. */
    if (receiver->status == SHERRY_STATE_WAIT_MSG) {
        listErase(&S->waitingmsg, &receiver->node);
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
        listEnqueue(&S->waitingmsg, &a->node);
        yield();
    }
    struct listNode *node = listDequeue(&a->mailbox);
    struct messageNode *msg = get_cont(node, struct messageNode, node);
    return (struct message *)msg;
}

#define SE_READABLE 1
#define SE_WRITABLE 2

/* Wait until the given file descriptor is ready. */
void waitFile(int fd, int event) {
    actor *a = S->running;
    if (event == SE_READABLE)
        listEnqueue(&S->waitread[fd], &a->node);
    if (event == SE_WRITABLE)
        listEnqueue(&S->waitwrite[fd], &a->node);
    yield();
}

int sherryAccept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    int s;
    while (1) {
        s = accept(sockfd, addr, addrlen);
        if (s >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            return s;
        /* We get EAGAIN or EWOULDBLOCK, block. */
        waitFile(sockfd, SE_READABLE);
    }
}

ssize_t sherryRead(int fd, void *buf, size_t count) {
    int nread;
    while (1) {
        nread = read(fd, buf, count);
        if (nread >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            return nread;
        waitFile(fd, SE_READABLE);
    }
}

ssize_t sherryWrite(int fd, const void *buf, size_t count) {
    int nwrite;
    while (1) {
        nwrite = write(fd, buf, count);
        if (nwrite >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            return nwrite;
        waitFile(fd, SE_READABLE);
    }
}

void sherryInit(void) {
    S = srmalloc(sizeof(scheduler));

    const size_t sz = 1000; // initial S->actors size
    S->sizeactors = sz;
    S->actors = srmalloc(sizeof(actor *) * sz);
    memset(S->actors, 0, sizeof(actor *) * sz);

    /* set current running to the fake main routine. */
    S->main = addActor(NULL, NULL);
    S->running = S->main;

    listInit(&S->readyq);
    listInit(&S->waitingmsg);

    S->waitwrite = srmalloc(sizeof(*S->waitwrite) * SHERRY_MAX_FD);
    S->waitread  = srmalloc(sizeof(*S->waitread)  * SHERRY_MAX_FD);
    for (int i = 0; i < SHERRY_MAX_FD; i++) {
        listInit(&S->waitwrite[i]);
        listInit(&S->waitread[i]);
    }
}

void sherryExit(void) {
    /* wait all ready actors exit. */
    while (!listEmpty(&S->readyq))
        sherryYield();
    delActor(S->main->rid);
    srfree(S->actors);
    srfree(S);
}

