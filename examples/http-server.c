#include "../sherry.h"
#include <stdio.h>
#include <string.h>
#include <uv.h>

void http_handler(void *argv) {
    sherry_fd_t *client_fd = (sherry_fd_t *)argv;

    char request[1024];
    ssize_t bytes_read = sherry_read(client_fd, request, sizeof(request) - 1);
    if (bytes_read <= 0) {
        fprintf(stderr, "Error read request: %s\n", uv_strerror(bytes_read));
        return;
    }

    request[bytes_read] = '\0';

    const char *response_header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n\r\n";

    const char *response_body =
        "<html><head><title>Hello, Sherry!</title></head>"
        "<body><h1>Hello, Sherry! This is a simple HTTP server.</h1></body></html>";

    sherry_write(client_fd, response_header, strlen(response_header));
    sherry_write(client_fd, response_body, strlen(response_body));

    sherry_fd_close(client_fd);
}

int main() {
    sherry_init();

    sherry_fd_t *server_fd = sherry_fd_tcp();

    if (sherry_tcp_bind(server_fd, "127.0.0.1", 6324) != 0) {
        fprintf(stderr, "Error: Unable to bind to port 6324\n");
        return 1;
    }

    if (sherry_listen(server_fd, 511) != 0) {
        fprintf(stderr, "Error: Unable to listen on port 6324\n");
        return 1;
    }

    printf("Server listening on port 6324...\n");

    while (1) {
        sherry_fd_t *client_fd = sherry_fd_tcp();
        int err = sherry_accept(server_fd, client_fd);
        if (err) {
            fprintf(stderr, "Error accept connection: %s\n", uv_strerror(err));
            continue;
        }
        sherry_spawn(http_handler, client_fd);
    }

    sherry_exit();
}
