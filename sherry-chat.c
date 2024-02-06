#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sherry.h"

/* Set the specified socket in non-blocking mode, with no delay flag. */
int socketSetNonBlockNoDelay(int fd) {
    int flags, yes = 1;

    /* Set the socket nonblocking.
     * Note that fcntl(2) for F_GETFL and F_SETFL can't be
     * interrupted by a signal. */
    if ((flags = fcntl(fd, F_GETFL)) == -1) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) return -1;

    /* This is best-effort. No need to check for errors. */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    return 0;
}

/* Create a TCP socket listening to 'port' ready to accept connections. */
int createTCPServer(int port) {
    int s, yes = 1;
    struct sockaddr_in sa;

    if ((s = socket(AF_INET, SOCK_STREAM, 0)) == -1) return -1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)); // Best effort.

    memset(&sa,0,sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s,(struct sockaddr*)&sa,sizeof(sa)) == -1 ||
        listen(s, 511) == -1 ||
        socketSetNonBlockNoDelay(s) == -1)
    {
        close(s);
        return -1;
    }
    return s;
}

#define MSG_READ   1
#define MSG_WRITE  2
#define MSG_CREATE 3
#define MSG_EXIT   4

const int serverid = SHERRY_MAIN_ID;

#define SC_PORT   6324
#define SC_MAXCLI 1000
int clireader[SC_MAXCLI];
int cliwriter[SC_MAXCLI];

void sendStringMsg(int recver, int type, const char *data) {
    char *copy = malloc(strlen(data)+1);
    strcpy(copy, data);
    sherrySendMsg(recver, type, copy, strlen(copy)+1);
}

void *clientAccpetor(void *arg) {
    int sockfd = (int)arg;
    socketSetNonBlockNoDelay(sockfd);
    while (1) {
        struct sockaddr_in sa;
        socklen_t slen = sizeof(sa);
        int fd = sherryAccept(sockfd,(struct sockaddr*)&sa,&slen);
        assert(fd != -1);
        socketSetNonBlockNoDelay(fd);
        printf("Connected client fd:%d\n", fd);
        sherrySendMsg(serverid, MSG_CREATE, (void *)fd, 0);
    }
}

void *clientReader(void *arg) {
    int fd = (int)arg;
    int nread;

    char nick[20];
    char username[20];
    char readbuf[256];
    char msg[sizeof(username)+sizeof(readbuf)];

    memset(nick, 0, sizeof(nick));
    sprintf(username, "user:%d", fd);

    while (1) {
        nread = sherryRead(fd, readbuf, sizeof(readbuf)-1);
        if(nread <= 0) break; // exit
        readbuf[nread] = 0;
        sprintf(msg, "%s> %s", username, readbuf);
        sendStringMsg(serverid, MSG_READ, msg);
    }

    printf("Disconnected client fd=%d, nick=%s\n", fd, nick);
    sherrySendMsg(serverid, MSG_EXIT, arg, 0);

    return NULL;
}

void *clientWriter(void *arg) {
    int fd = (int)arg;
    while (1) {
        struct message *msg = sherryReceiveMsg();
        if (msg->msgtype == MSG_EXIT) {
            free(msg);
            break;
        }
        if (msg->msgtype == MSG_WRITE) {
            sherryWrite(fd, msg->payload, msg->size);
            free(msg->payload);
        }
        free(msg);
    }
    return NULL;
}

const char *welcomeMsg =
    "Welcome to sherry chat.\n"
    "Use /nick <nick> to set your nick.\n";

int main(void) {
    sherryInit();

    memset(clireader, 0, sizeof(clireader));
    memset(cliwriter, 0, sizeof(cliwriter));

    int sockfd = createTCPServer(SC_PORT);
    assert(sockfd != -1);

    sherrySpawn(clientAccpetor, (void *)sockfd);

    while (1)
    {
        struct message *msg = sherryReceiveMsg();
        if (msg->msgtype == MSG_CREATE)
        {
            int fd = (int)msg->payload;
            clireader[fd] = sherrySpawn(clientReader, (void *)fd);
            cliwriter[fd] = sherrySpawn(clientWriter, (void *)fd);
            sendStringMsg(cliwriter[fd], MSG_WRITE, welcomeMsg);
        }
        else if (msg->msgtype == MSG_EXIT)
        {
            int fd = (int)msg->payload;
            sherrySendMsg(cliwriter[fd], MSG_EXIT, NULL, 0);
            close(fd);
            clireader[fd] = 0;
            cliwriter[fd] = 0;
        }
        else if (msg->msgtype == MSG_READ)
        {
            printf("%s", (char *)msg->payload);
            for (int fd = 0; fd < SC_MAXCLI; fd++) {
                if (cliwriter[fd] == 0)
                    continue;
                if (clireader[fd] == msg->senderid)
                    continue;
                sendStringMsg(cliwriter[fd], MSG_WRITE, msg->payload);
            }
            free(msg->payload);
        }
        free(msg);
    }

    sherryExit();
}
