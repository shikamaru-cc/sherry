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
#include "sds.h"

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

struct list_node {
    struct list_node *prev;
    struct list_node *next;
};

struct list {
    struct list_node *head;
    struct list_node *tail;
};

/* True is the list has no items. */
#define list_empty(self) (!((self)->head))

/* Returns iterator to the first item in the list or NULL if
   the list is empty. */
#define list_begin(self) ((self)->head)

/* Returns iterator to one past the item pointed to by 'it'. */
#define list_next(it) ((it)->next)

void list_init(struct list *l) {
    l->head = NULL;
    l->tail = NULL;
}

/* Insert node 'item' to a list before the 'it' node. */
void list_insert(struct list *self, struct list_node *item, struct list_node *it) {
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
struct list_node *list_erase(struct list *self, struct list_node *item) {
    struct list_node *next;

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
void list_enqueue(struct list *q, struct list_node *node) {
    list_insert(q, node, NULL);
}

/* Simple double linked list based queue interface. Pop up the list's 
 * head node. */
struct list_node *list_dequeue(struct list *q) {
    struct list_node *node = list_begin(q);
    if (node == NULL)
        return NULL;
    list_erase(q, node);
    return node;
}

/* ============================================================================
 * Sherry actor model core implementation.
 * ========================================================================== */

/* A wrapper for struct message that make it a list node and be possible for 
 * maintaining in a queue. */
struct msg_node {
    struct sherry_msg msg;
    struct list_node node;
};

void sherry_msg_free(struct sherry_msg *msg) {
    sdsfree(msg->payload);
    srfree(msg);
}

/* All posible actor status */
#define SHERRY_STATE_DEAD       0
#define SHERRY_STATE_READY      1
#define SHERRY_STATE_RUNNING    2
#define SHERRY_STATE_WAIT_MSG   3
#define SHERRY_STATE_WAIT_RFD   4 // waiting readable fd
#define SHERRY_STATE_WAIT_WFD   5 // waiting writable fd

typedef jmp_buf context;

#define SHERRY_STACK_SIZE 4096
#define SHERRY_MAX_ARGC   20   // maximum arg number for spawning actor

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
    int aid;                   // routine id
    int status;                // actor current status
    context ctx;               // actor running context
    sherry_fn_t fn;            // actor function
    int argc;                  // number of arguments
    sds argv[SHERRY_MAX_ARGC]; // actor arguments
    struct list mailbox;       // a queue of received messages in this actor
    struct list_node node;     // actor is a doule-linked list based
                               // queue in the scheduler now.
} actor_t;

actor_t *new_actor(int rid, sherry_fn_t fn, int argc, char **argv) {
    /* p is the original allocated pointer. */
    char *p = srmalloc(sizeof(actor_t) + SHERRY_STACK_SIZE);
    /* The first SHERRY_STACK_SIZE bytes are used as the actor's
     * stack. See the memory model above. */
    actor_t *r = (actor_t *)(p + SHERRY_STACK_SIZE);
    r->aid = rid;
    r->status = SHERRY_STATE_READY;

    r->fn = fn;
    r->argc = argc;
    /* TODO: error on argc over max */
    if (argc > 0) {
        for (int i = 0; i < r->argc; i++)
            r->argv[i] = sdsnew(argv[i]);
    }

    r->node.prev = NULL;
    r->node.next = NULL;
    list_init(&r->mailbox);
    return r;
}

void free_actor(actor_t *actor) {
    /* release resources */

    for (int i = 0; i < actor->argc; i++)
        sdsfree(actor->argv[i]);

    while (!list_empty(&actor->mailbox)) {
        struct list_node *node = list_dequeue(&actor->mailbox);
        struct msg_node *msg = get_cont(node, struct msg_node, node);
        sdsfree(msg->msg.payload);
        srfree(msg);
    }

    /* Go back to the original allocated pointer. */
    char *p = (char *)actor - SHERRY_STACK_SIZE;
    srfree(p);
}

/* The pointer to the struct actor is also the start address of the stack */
void *actor_stack(actor_t *a) { return a; }

#define SHERRY_MAX_FD 1024

typedef struct scheduler {
    int sizeactors;     // the size of array actors
    actor_t **actors;     // all register actors, indexed by actor id
    actor_t *main;        // the fake main actor
    actor_t *running;     // the current running actor
    struct list readyq; // actors ready to run
    struct list waitingmsg; // actors blocked on mailbox
    struct list *waitwrite; // maps fd to a list of actors blocked on write fd
    struct list *waitread;  // maps fd a list of actors blocked on read fd
} scheduler_t;

scheduler_t *sche;

/* Register a new actor to actors array. */
actor_t *reg_actor(sherry_fn_t fn, int argc, char **argv) {
    /* TODO: O(n) now, make it O(1)? */
    int id = -1;
    for (int i = 0; i < sche->sizeactors; i++) {
        if (sche->actors[i] == NULL) {
            id = i;
            break;
        }
    }

    /* there is no available slot, realloc routines array. */
    if (id == -1) {
        size_t oldsize = sche->sizeactors;
        size_t newsize = sche->sizeactors * 2;
        sche->actors = srrealloc(sche->actors, newsize * sizeof(actor_t *));
        memset(sche->actors + oldsize, 0,
            (newsize - oldsize) * sizeof(actor_t *));
        sche->sizeactors = newsize;
        id = oldsize;
    }

    actor_t *actor = new_actor(id, fn, argc, argv);
    sche->actors[id] = actor;
    return actor;
}

/* Unregister an actor from actors array. */
void unreg_actor(int rid) {
    if (rid < 0 || rid >= sche->sizeactors || sche->actors[rid] == NULL)
        return;

    actor_t *rt = sche->actors[rid];
    free_actor(rt);
    sche->actors[rid] = NULL;

    /* TODO: shrink the array size. */
}

/* set the actor to running state and enqueue it to readyq. */
void setready(actor_t *a) {
    a->status = SHERRY_STATE_READY;
    list_enqueue(&sche->readyq, &a->node);
}

void setrunning(actor_t *a) {
    a->status = SHERRY_STATE_RUNNING;
    sche->running = a;
}

/* Unblock all actors in the given list. */
void unblock_all(struct list *list) {
    struct list_node *node;
    actor_t *a;
    while (!list_empty(list)) {
        node = list_dequeue(list);
        a = get_cont(node, actor_t, node);
        setready(a);
    }
}

void poll_file(void) {
    fd_set rfds;
    fd_set wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);

    for (int fd = 0; fd < SHERRY_MAX_FD; fd++) {
        if (!list_empty(&sche->waitwrite[fd])) FD_SET(fd, &wfds);
        if (!list_empty(&sche->waitread[fd]))  FD_SET(fd, &rfds);
    }

    int ret = select(SHERRY_MAX_FD + 1, &rfds, &wfds, NULL, NULL);
    if (ret == -1) {
        perror("select error()");
        exit(1);
    }

    for (int fd = 0; fd < SHERRY_MAX_FD; fd++) {
        if (FD_ISSET(fd, &rfds))
            unblock_all(&sche->waitread[fd]);
        if (FD_ISSET(fd, &wfds))
            unblock_all(&sche->waitwrite[fd]);
    }
}

/* Resume one actor in the ready queue. */
void resume(void) {
    while (1) {
        struct list_node *node = list_dequeue(&sche->readyq);
        actor_t *next = get_cont(node, actor_t, node);
        if (next != NULL) {
            setrunning(next);
            siglongjmp(next->ctx, 1);
        }
        poll_file();
    }
}

/* Yield current running actor, save the context and go to resume for running
 * the next one. Note that, this yield function would not do any extra work,
 * other work like enqueueing to the ready queue or wait queue should be done
 * before calling this function. */
void yield(void) {
    actor_t *a = sche->running;
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
int sherry_spawn(sherry_fn_t fn, int argc, char **argv) {

    actor_t *newactor = reg_actor(fn, argc, argv);

    /* interrupt current running actor */
    actor_t *curr = sche->running;
    setready(curr);

    if (sigsetjmp(curr->ctx, 0)) {
        /* If the return value of sigsetjmp is greater than zero,
         * it means that we go back to this saved context
         * by siglongjmp, then we should return and continue
         * the interrupted routine. */
        return newactor->aid;
    }

    /* The return value of sigsetjmp is zero means that we continue the
     * current context, spawn the new actor and run it. */
    setrunning(newactor);

    /* Switch stack here, then we cannot access fn and arg from stack params,
     * so we call it through the global scheduler. */
    switchsp(actor_stack(newactor));
    sche->running->fn(sche->running->argc, sche->running->argv);

    /* The actor exits, we should release resources and give control back to
     * scheduler. We should not free the memory of current stack, so we keep
     * a temp stack as a static variable and switch to it before we do the
     * cleanup job. */

    static char _tmpstack[4096];
    static char *tmpstack = _tmpstack + sizeof(_tmpstack);

    switchsp(tmpstack);
    unreg_actor(sche->running->aid);

    /* Give the control back to scheduler. */
    resume();

    return 0; // never reach here
}

/* Put the current running actor to ready queue and yield. */
void sherry_yield(void) {
    setready(sche->running);
    yield();
}

/* Send a message to the receiver actor. This function never blocks the
 * running actor. This method will take the ownership for payload,
 * the caller should not use payload anymore. */
void sherry_msg_send(int dst, int msgtype, sds payload) {
    actor_t *sender = sche->running;
    actor_t *recver = sche->actors[dst]; // TODO: check the dst idx

    struct msg_node *msg = srmalloc(sizeof(*msg));

    msg->msg.sender = sender->aid;
    msg->msg.recver = recver->aid;

    msg->msg.msgtype = msgtype;
    msg->msg.payload = payload; // take ownership

    msg->node.prev = NULL;
    msg->node.next = NULL;
    list_enqueue(&recver->mailbox, &msg->node);

    /* Revoke the receiver actor if it is blocked. */
    if (recver->status == SHERRY_STATE_WAIT_MSG) {
        list_erase(&sche->waitingmsg, &recver->node);
        setready(recver);
    }
}

/* Running actor receives a message from its mailbox. If there is nothing
 * in the mailbox, the running actor will yield and be invoked again when
 * there is something in the mailbox. After receving message, the caller
 * takes the ownership of the message, and has the responsibility to call
 * `sherry_msg_free` to release resource. */
struct sherry_msg *sherry_msg_recv(void) {
    actor_t *a = sche->running;
    if (list_empty(&a->mailbox)) {
        a->status = SHERRY_STATE_WAIT_MSG;
        list_enqueue(&sche->waitingmsg, &a->node);
        yield();
    }
    struct list_node *node = list_dequeue(&a->mailbox);
    struct msg_node *msg = get_cont(node, struct msg_node, node);
    return (struct sherry_msg *)msg;
}

#define SE_READABLE 1
#define SE_WRITABLE 2

/* Wait until the given file descriptor is ready. */
void wait_file(int fd, int event) {
    actor_t *a = sche->running;
    if (event == SE_READABLE)
        list_enqueue(&sche->waitread[fd], &a->node);
    if (event == SE_WRITABLE)
        list_enqueue(&sche->waitwrite[fd], &a->node);
    yield();
}

int sherry_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    int s;
    while (1) {
        s = accept(sockfd, addr, addrlen);
        if (s >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            return s;
        /* We get EAGAIN or EWOULDBLOCK, block. */
        wait_file(sockfd, SE_READABLE);
    }
}

ssize_t sherry_read(int fd, void *buf, size_t count) {
    int nread;
    while (1) {
        nread = read(fd, buf, count);
        if (nread >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            return nread;
        wait_file(fd, SE_READABLE);
    }
}

ssize_t sherry_write(int fd, const void *buf, size_t count) {
    int nwrite;
    while (1) {
        nwrite = write(fd, buf, count);
        if (nwrite >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            return nwrite;
        wait_file(fd, SE_READABLE);
    }
}

void sherry_init(void) {
    sche = srmalloc(sizeof(scheduler_t));

    const size_t sz = 1000; // initial S->actors size
    sche->sizeactors = sz;
    sche->actors = srmalloc(sizeof(actor_t *) * sz);
    memset(sche->actors, 0, sizeof(actor_t *) * sz);

    /* set current running to the fake main routine. */
    sche->main = reg_actor(NULL, 0, NULL);
    sche->running = sche->main;

    list_init(&sche->readyq);
    list_init(&sche->waitingmsg);

    sche->waitwrite = srmalloc(sizeof(*sche->waitwrite) * SHERRY_MAX_FD);
    sche->waitread  = srmalloc(sizeof(*sche->waitread)  * SHERRY_MAX_FD);
    for (int i = 0; i < SHERRY_MAX_FD; i++) {
        list_init(&sche->waitwrite[i]);
        list_init(&sche->waitread[i]);
    }
}

void sherry_exit(void) {
    /* wait all ready actors exit. */
    while (!list_empty(&sche->readyq))
        sherry_yield();
    unreg_actor(sche->main->aid);
    srfree(sche->actors);
    srfree(sche->waitwrite);
    srfree(sche->waitread);
    srfree(sche);
}

