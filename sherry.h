#ifndef SHERRY_H
#define SHERRY_H

#include <stdint.h>
#include <sys/socket.h>
#include <uv.h>

#include "sds.h"

struct sherry_msg {
    uint64_t sender;
    uint64_t recver;
    int msgtype;
    sds payload;
};

void sherry_msg_free(struct sherry_msg *msg);

#define SHERRY_MAIN_ID 0

typedef void (*sherry_fn_t)(void *);

/* Unused arguments generate annoying warnings... */
#define SHERRY_NOTUSED(V) ((void) V)

uint64_t sherry_spawn(sherry_fn_t fn, void *arg);
void sherry_yield(void);
void sherry_msg_send(uint64_t dst, int msgtype, sds payload);
struct sherry_msg *sherry_msg_recv(void);
int sherry_kill(int aid);

struct sherry_fd;
typedef struct sherry_fd sherry_fd_t;

int sherry_fd_close(sherry_fd_t *fd);

sherry_fd_t *sherry_fd_tcp(void);
int sherry_tcp_bind(sherry_fd_t *fd, const char *ip, int port);
int sherry_tcp_peername(sherry_fd_t *fd, struct sockaddr *name, int *namelen);
int sherry_listen(sherry_fd_t *fd, int backlog);
int sherry_accept(sherry_fd_t *serverfd, sherry_fd_t *clientfd);
ssize_t sherry_read(sherry_fd_t *fd, void *buf, size_t count);
ssize_t sherry_write(sherry_fd_t *fd, const void *buf, size_t count);

void sherry_init(void);
void sherry_exit(void);

#endif // !SHERRY_H
