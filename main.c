#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_EVS 10
#define N 10

int setnonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return -1;
    }
    flags |= O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags);
}

struct server {
    int listenfd;
    int epollfd;
    struct sockaddr_in addr;
    struct epoll_event evs[MAX_EVS];
};

struct conn {
    int fd;
    struct sockaddr_in remote_addr;
    char buf[N];
};

void conn_destory(struct conn *c) {
    close(c->fd);
    free(c);
}

struct conn *accept_conn(struct server *ctx) {
    assert(ctx);

    struct conn *c = malloc(sizeof(struct conn));
    if (!c) {
        return NULL;
    }

    socklen_t socklen = sizeof(c->remote_addr);
    c->fd = accept(ctx->listenfd, (struct sockaddr *)&c->remote_addr, &socklen);
    if (c->fd < 0) {
        goto cleanup;
    }

    fprintf(stderr, "accept connection form %s:%u\n",
            inet_ntoa(c->remote_addr.sin_addr), ntohs(c->remote_addr.sin_port));

    if (setnonblocking(c->fd) < 0) {
        goto cleanup;
    }

    struct epoll_event ev = {0};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = c;

    if (epoll_ctl(ctx->epollfd, EPOLL_CTL_ADD, c->fd, &ev) < 0) {
        goto cleanup;
    }

    return c;

cleanup:
    free(c);
    return NULL;
}

void handle_conn(struct conn *c) {
    assert(c);

    for (;;) {
        memset(c->buf, 0, N);
        int nr = read(c->fd, c->buf, N);
        if (nr < 0) {
            if (errno != EAGAIN) {
                perror("read");
                goto cleanup;
            }
            return;
        }

        if (nr == 0) {
            fprintf(stderr, "connection %s:%u closed\n",
                    inet_ntoa(c->remote_addr.sin_addr),
                    ntohs(c->remote_addr.sin_port));
            goto cleanup;
            return;
        }

        for (int i = 0; i < N; i++) {
            c->buf[i] = toupper(c->buf[i]);
        }

        write(c->fd, c->buf, nr);
    }

    return;

cleanup:
    conn_destory(c);
}

struct server *create_server(const char *host, uint16_t port) {
    struct server *s = malloc(sizeof(struct server));
    if (!s) {
        return NULL;
    }

    s->addr.sin_family = AF_INET;
    s->addr.sin_addr.s_addr = inet_addr(host);
    s->addr.sin_port = htons(port);

    s->listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listenfd < 0) {
        goto cleanup1;
    }

    s->epollfd = epoll_create1(0);
    if (s->epollfd < 0) {
        goto cleanup2;
    }

    struct epoll_event ev = {
        .events = EPOLLIN | EPOLLET,
        .data.fd = s->listenfd,
    };

    if (epoll_ctl(s->epollfd, EPOLL_CTL_ADD, s->listenfd, &ev) < 0) {
        goto cleanup3;
    }

    return s;

cleanup3:
    close(s->epollfd);

cleanup2:
    close(s->listenfd);

cleanup1:
    free(s);
    return NULL;
}

void server_destory(struct server *ctx) {
    close(ctx->epollfd);
    close(ctx->listenfd);
    free(ctx);
}

int listen_and_serve(struct server *s, void (*phandler)(struct conn *)) {
    assert(s && phandler);

    int rc = 0;
    int enable = 1;

    if ((rc = setsockopt(s->listenfd, SOL_SOCKET, SO_REUSEADDR, &enable,
                         sizeof(enable))) < 0) {
        return rc;
    }

    if ((rc = bind(s->listenfd, (struct sockaddr *)&s->addr, sizeof(s->addr))) <
        0) {
        return rc;
    }

    if ((rc = listen(s->listenfd, 10)) < 0) {
        return rc;
    }

    for (;;) {
        int n = epoll_wait(s->epollfd, s->evs, MAX_EVS, -1);
        if (n < 0) {
            perror("epoll_wait");
            continue;
        }

        for (int i = 0; i < n; i++) {
            int fd = s->evs[i].data.fd;
            struct conn *c = s->evs[i].data.ptr;
            if (fd == s->listenfd) {
                accept_conn(s);
            } else {
                phandler(c);
            }
        }
    }
}

int main(int argc, char *argv[]) {
    struct server *s = create_server("0.0.0.0", 8080);
    if (!s) {
        perror("create_server");
        exit(EXIT_FAILURE);
    }

    int rc = listen_and_serve(s, handle_conn);
    if (rc < 0) {
        perror("listen_and_serve");
        exit(EXIT_FAILURE);
    }

    server_destory(s);
    return EXIT_SUCCESS;
}
