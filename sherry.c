#include <assert.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <valgrind/valgrind.h>

#include <uv.h>

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
 * Low-level context switch stuff.
 * ========================================================================== */

#if defined (__x86_64__)

typedef struct context_t {
    void *rsp, *rbp, *rbx, *r12, *r13, *r14, *r15;
} context_t;

void _makecontext(context_t *ctx, void (*f)(), char *stack, size_t stacksz) {
    stacksz = stacksz - (sizeof(void*)); // reverse room for ret address
    ctx->rsp = stack + stacksz;
    void **rsp = (void **)ctx->rsp;
    rsp[0] = (void *)f; // setup for ret
}

int _swapcontext(context_t *octx, context_t *ctx);

__asm__(
    ".text\n"
    ".globl _swapcontext\n"
    "_swapcontext:\n"

    "    movq %rsp,   (%rdi)\n"
    "    movq %rbp,  8(%rdi)\n"
    "    movq %rbx, 16(%rdi)\n"
    "    movq %r12, 24(%rdi)\n"
    "    movq %r13, 32(%rdi)\n"
    "    movq %r14, 40(%rdi)\n"
    "    movq %r15, 48(%rdi)\n"

    "    movq   (%rsi), %rsp\n"
    "    movq  8(%rsi), %rbp\n"
    "    movq 16(%rsi), %rbx\n"
    "    movq 24(%rsi), %r12\n"
    "    movq 32(%rsi), %r13\n"
    "    movq 40(%rsi), %r14\n"
    "    movq 48(%rsi), %r15\n"

    "    xor %rax, %rax\n"
    "    ret\n"
);

#else

#include <ucontext.h>

typedef ucontext_t context_t;

void _makecontext(context_t *ctx, void (*f)(), char *stack, size_t stacksz) {
    getcontext(ctx); // TODO: handle error
    ctx->uc_stack.ss_sp = stack;
    ctx->uc_stack.ss_size = stacksz;
    makecontext(ctx, f, 0);
}

int _swapcontext(context_t *octx, context_t *ctx) {
    return swapcontext(octx, ctx);
}

#endif

/* ============================================================================
 * Sherry actor model core implementation.
 * ========================================================================== */

/* Errors should be defined here. */
#define SHERRY_ERR_INVALID 1

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
#define SHERRY_STATE_NONE       0
#define SHERRY_STATE_DEAD       1
#define SHERRY_STATE_READY      2
#define SHERRY_STATE_RUNNING    3
#define SHERRY_STATE_WAIT_MSG   4
#define SHERRY_STATE_WAIT_EV    5

#define SHERRY_STACK_SIZE (10 * 4096)
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
    context_t ctx;
    sherry_fn_t fn;            // actor function
    void *argv;                // actor argument
    int valgrind_stk_id;
    struct list mailbox;       // a queue of received messages in this actor
    struct list_node node;     // actor is a doule-linked list based
                               // queue in the scheduler now.
} actor_t;

actor_t *new_actor(sherry_fn_t fn, void *argv) {
    /* p is the original allocated pointer. */
    char *p = srmalloc(sizeof(actor_t) + SHERRY_STACK_SIZE);

    /* The first SHERRY_STACK_SIZE bytes are used as the actor's
     * stack. See the memory model above. */
    actor_t *r = (actor_t *)(p + SHERRY_STACK_SIZE);
    r->aid = -1;
    r->status = SHERRY_STATE_NONE;

    r->fn = fn;
    r->argv = argv;

    r->valgrind_stk_id = VALGRIND_STACK_REGISTER(p, r);

    r->node.prev = NULL;
    r->node.next = NULL;
    list_init(&r->mailbox);

    return r;
}

void free_actor(actor_t *actor) {
    /* release resources */
    while (!list_empty(&actor->mailbox)) {
        struct list_node *node = list_dequeue(&actor->mailbox);
        struct msg_node *msg = get_cont(node, struct msg_node, node);
        sdsfree(msg->msg.payload);
        srfree(msg);
    }

    VALGRIND_STACK_DEREGISTER(actor->valgrind_stk_id);

    /* Go back to the original allocated pointer. */
    char *p = (char *)actor - SHERRY_STACK_SIZE;
    srfree(p);
}

/* The pointer to the struct actor is also the start address of the stack */
void *actor_stack(actor_t *a) { return (char *)a - SHERRY_STACK_SIZE; }

#define SHERRY_MAX_FD 1024

typedef struct scheduler {
    int sizeactors;         // the size of array actors
    int curractors;         // current registered actors
    actor_t **actors;       // all register actors, indexed by actor id
    actor_t *main;          // the fake main actor
    actor_t *running;       // the current running actor
    struct list readyq;     // actors ready to run
    struct list waitingmsg; // actors blocked on mailbox
    uv_loop_t loop;
    context_t ctx;
    char *tmpstack;         // temp stack for context switch
    int valgrind_tstk_id;   // valgrind temp stack id
} scheduler_t;

scheduler_t *sche;

/* Register a new actor to actors array. */
actor_t *reg_actor(actor_t *actor) {
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

    actor->aid = id;
    sche->actors[id] = actor;
    sche->curractors++;

    return actor;
}

/* Unregister an actor from actors array. */
void unreg_actor(int rid) {
    if (rid < 0 || rid >= sche->sizeactors || sche->actors[rid] == NULL)
        return;

    // actor_t *rt = sche->actors[rid];
    // free_actor(rt);
    sche->actors[rid] = NULL;
    sche->curractors--;

    /* TODO: shrink the array size. */
}

actor_t *current_actor(void) { return sche->running; }

actor_t *get_actor(int aid) {
    return (aid < sche->sizeactors) ? sche->actors[aid] : NULL;
}

/* set the actor to running state and enqueue it to readyq. */
void setready(actor_t *a) {
    if (a->status != SHERRY_STATE_READY) {
        a->status = SHERRY_STATE_READY;
        list_enqueue(&sche->readyq, &a->node);
    }
    /* Already ready, skip. */
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

/* Resume one actor in the ready queue. */
void schdule(void) {
    while (1) {
        struct list_node *node = list_dequeue(&sche->readyq);
        actor_t *next = get_cont(node, actor_t, node);
        if (next != NULL) {
            setrunning(next);
            _swapcontext(&sche->ctx, &next->ctx);
            continue;
        }
        uv_run(&sche->loop, UV_RUN_ONCE);
    }
}

/* Yield current running actor, save the context and go to resume for running
 * the next one. Note that, this yield function would not do any extra work,
 * other work like enqueueing to the ready queue or wait queue should be done
 * before calling this function. */
void yield(void) {
    actor_t *a = sche->running;
    int e = _swapcontext(&a->ctx, &sche->ctx);
    assert(e == 0);
}

void sherry_main(void) {
    actor_t *actor = current_actor();
    actor->fn(actor->argv);
    unreg_actor(sche->running->aid);
    yield();
}

/* Spawn a new actor, interrupt the current running actor and give the control
 * to the new spawned one. */
int sherry_spawn(sherry_fn_t fn, void *argv) {
    actor_t *newactor = new_actor(fn, argv);
    reg_actor(newactor);
    setready(newactor);
    _makecontext(&newactor->ctx, sherry_main,
        actor_stack(newactor), SHERRY_STACK_SIZE);
    return newactor->aid;
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

/* ============================================================================
 * Sherry nonblock network stuff.
 * ========================================================================== */

#define SHERRY_FD_TCP 1

#define SHERRY_EV_READ  0
#define SHERRY_EV_WRITE 1
#define SHERRY_EV_MAX   2

typedef struct sherry_fd {
    int type;
    uv_buf_t rbuf; // read buffer
    size_t roffset; // read buffer offset
    int rerror; // read error
    struct list waitq[SHERRY_EV_MAX]; // blocked actors waiting for
                                      // different events
    union uv_any_handle handle;
} sherry_fd_t;

sherry_fd_t *fdnew(int type) {
    sherry_fd_t *fd = srmalloc(sizeof(*fd));
    if (fd == NULL)
        return NULL;

    fd->type = type;

    fd->rbuf.base = NULL;
    fd->rbuf.len = 0;

    fd->roffset = 0;
    fd->rerror = 0;

    uv_handle_set_data((uv_handle_t *)&fd->handle, fd);

    for (int i = 0; i < SHERRY_EV_MAX; i++)
        list_init(&fd->waitq[i]);

    return fd;
}

void fdfree(sherry_fd_t *fd) {
    if (fd->rbuf.base)
        srfree((void *)fd->rbuf.base);
    srfree(fd);
}

void close_cb(uv_handle_t *handle) {
    fdfree(handle->data);
}

int sherry_fd_close(sherry_fd_t *fd) {
    uv_close((uv_handle_t *)&fd->handle, close_cb);
    return 0;
}

/* Wait until the given file descriptor is ready. */
void block_fd(sherry_fd_t *fd, int event) {
    actor_t *actor = current_actor();
    actor->status = SHERRY_STATE_WAIT_EV;
    list_enqueue(&fd->waitq[event], &actor->node);
    yield();
}

sherry_fd_t *sherry_fd_tcp(void) {
    sherry_fd_t *fd;

    fd = fdnew(SHERRY_FD_TCP);
    if (fd == NULL)
        return NULL;

    /* setup libuv tcp handle */
    uv_tcp_init(&sche->loop, &fd->handle.tcp);

    return fd;
}

int sherry_tcp_bind(sherry_fd_t *fd, const char *ip, int port) {
    struct sockaddr_in addr;
    uv_ip4_addr(ip, port, &addr);
    return uv_tcp_bind(&fd->handle.tcp, (const struct sockaddr *)&addr, 0);
}

int sherry_tcp_peername(sherry_fd_t *fd, struct sockaddr *name, int *namelen) {
    return uv_tcp_getpeername(&fd->handle.tcp, name, namelen);
}

void listen_cb(uv_stream_t *server, int status) {
    if (status < 0) {
        // TODO: handle error
    }
    sherry_fd_t *fd = (sherry_fd_t *)server->data;
    unblock_all(&fd->waitq[SHERRY_EV_READ]);
}

int sherry_listen(sherry_fd_t *fd, int backlog) {
    if (fd == NULL || fd->type != SHERRY_FD_TCP)
        return SHERRY_ERR_INVALID;
    return uv_listen(&fd->handle.stream, backlog, listen_cb);
}

int sherry_accept(sherry_fd_t *serverfd, sherry_fd_t *clientfd) {
    while (1) {
        int err = uv_accept(&serverfd->handle.stream, &clientfd->handle.stream);
        if (err >= 0 || err != UV_EAGAIN)
            return err;
        else /* we got UV_EAGAIN */
            block_fd(serverfd, SHERRY_EV_READ);
    }
}

void rbuf_alloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    SHERRY_NOTUSED(handle);
    buf->base = srmalloc(suggested_size);
    buf->len = suggested_size;
}

void read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    sherry_fd_t *fd = stream->data;

    /* From libuv document, nread < 0 when error, nread == 0 when nothing
     * read but everything ok. It's ok to set rerror to nread. */
    if (nread <= 0) {
        fd->rerror = nread;
        srfree(buf->base);
        return;
    }

    /* When we reach this callback, the old buffer should not be used
     * anymore, reset it. */
    if (fd->rbuf.base) {
        srfree((void *)fd->rbuf.base);
        fd->rbuf.base = NULL;
        fd->rbuf.len = 0;
        fd->roffset = 0;
    }

    /* We get a new buffer, set it and unblock all waiting actors. */
    fd->rbuf = *buf;

    unblock_all(&fd->waitq[SHERRY_EV_READ]);
}

ssize_t sherry_read(sherry_fd_t *fd, void *buf, size_t count) {
    /* uv_read_start is re-entryable, just make sure it starts. */
    int err = uv_read_start(&fd->handle.stream, rbuf_alloc, read_cb);
    if (err)
        return err;

    while (1) {
        if (fd->rerror)
            return (fd->rerror == UV_EOF) ? 0 : fd->rerror;

        /* Buffer not empty, we only read from buffer. Now, If the remaining
         * size is less than count, we simply read all remaining bytes. */
        if (fd->rbuf.base && fd->rbuf.len > fd->roffset) {
            size_t remain = fd->rbuf.len - fd->roffset;
            size_t nread  = (remain > count) ? count : remain;
            memcpy(buf, fd->rbuf.base + fd->roffset, nread);
            fd->roffset += nread;
            return nread;
        }

        /* Nothing in buffer, block */
        block_fd(fd, SHERRY_EV_READ);
    }
}

void write_cb(uv_write_t *req, int status) {
    actor_t *actor = (actor_t *)req->data;
    setready(actor);
    // TODO: handle status error
    assert(status == 0);
}

ssize_t sherry_write(sherry_fd_t *fd, const void *buf, size_t count) {
    uv_write_t req;
    req.data = current_actor();

    uv_buf_t uvbuf;
    uvbuf.base = (char *)buf;
    uvbuf.len = count;

    uv_write(&req, &fd->handle.stream, &uvbuf, 1, write_cb);
    current_actor()->status = SHERRY_STATE_WAIT_EV;
    yield();

    // TODO: handle error
    return count;
}

void sherry_init(void) {
    sche = srmalloc(sizeof(scheduler_t));

    const size_t sz = 1000; // initial S->actors size
    sche->sizeactors = sz;
    sche->curractors = 0;
    sche->actors = srmalloc(sizeof(actor_t *) * sz);
    memset(sche->actors, 0, sizeof(actor_t *) * sz);

    /* set current running to the fake main routine. */
    sche->main = new_actor(NULL, NULL);
    reg_actor(sche->main);
    setrunning(sche->main);

    list_init(&sche->readyq);
    list_init(&sche->waitingmsg);

    uv_loop_init(&sche->loop);

    sche->tmpstack = srmalloc(SHERRY_STACK_SIZE);
    sche->valgrind_tstk_id = VALGRIND_STACK_REGISTER(
            sche->tmpstack, sche->tmpstack + SHERRY_STACK_SIZE);

    _makecontext(&sche->ctx, schdule, sche->tmpstack, SHERRY_STACK_SIZE);
}

void sherry_exit(void) {
    /* wait all ready actors exit. */
    while (sche->curractors > 1) {
        sherry_yield();
    }

    unreg_actor(sche->main->aid);
    srfree(sche->actors);

    uv_loop_close(&sche->loop);

    srfree(sche->tmpstack);
    VALGRIND_STACK_DEREGISTER(sche->valgrind_tstk_id);

    srfree(sche);
}

