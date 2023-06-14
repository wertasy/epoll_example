#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
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

struct buff {
    char *ptr;
    size_t len;
    size_t cap;
};

struct http_head {
    char *method;
    char *path;
    size_t content_size;
};

enum head_state {
    st_read_request_line,
    st_read_content_size,
    st_read_crlf,
    st_read_request_body,
    st_done,
};

struct conn {
    int fd;
    enum head_state state;
    struct sockaddr_in addr;
    struct buff buf;
    struct http_head head;
    char *errmsg;
};

void head_destory(struct http_head *head) {
    free(head->method);
    free(head->path);
}

void conn_destory(struct conn *c) {
    close(c->fd);
    free(c->buf.ptr);
    head_destory(&c->head);
    free(c);
}

struct conn *accept_conn(struct server *s) {
    assert(s);

    struct conn *c = malloc(sizeof(struct conn));
    if (!c) {
        return NULL;
    }

    bzero(c, sizeof(struct conn));

    socklen_t socklen = sizeof(c->addr);
    c->fd = accept(s->listenfd, (struct sockaddr *)&c->addr, &socklen);
    if (c->fd < 0) {
        goto cleanup0;
    }

    fprintf(stderr, "accept connection form %s:%u\n",
            inet_ntoa(c->addr.sin_addr), ntohs(c->addr.sin_port));

    if (setnonblocking(c->fd) < 0) {
        goto cleanup1;
    }

    struct epoll_event ev = {
        .events = EPOLLIN | EPOLLET,
        .data.ptr = c,
    };

    if (epoll_ctl(s->epollfd, EPOLL_CTL_ADD, c->fd, &ev) < 0) {
        goto cleanup1;
    }

    return c;

cleanup1:
    close(c->fd);

cleanup0:
    free(c);

    return NULL;
}

ssize_t buf_write(struct buff *buf, char *data, size_t size) {
    if (buf->ptr == NULL || buf->cap == 0) {
        buf->len = 0;
        buf->cap = size;
        buf->ptr = malloc(buf->cap);
        if (buf->ptr == NULL) {
            return -1;
        }
    }

    size_t new_len = buf->len + size;
    if (buf->cap < new_len) {
        size_t new_cap = 2 * buf->cap;
        while (new_cap < new_len) {
            new_cap *= 2;
        }
        char *new_ptr = realloc(buf->ptr, new_cap);
        if (new_ptr == NULL) {
            return -1;
        }
        buf->ptr = new_ptr;
        buf->cap = new_cap;
    }

    memcpy(buf->ptr + buf->len, data, size);
    buf->len += size;

    return size;
}

void buff_clear(struct buff *buf) {
    buf->len = 0;
    bzero(buf->ptr, buf->cap);
}

int parse_content_size(struct http_head *head, char *buf, size_t n) {
#define CONTENT_LENGTH "Content-Length"
    char tmpbuf[20] = {0};
    if (!strncasecmp(buf, CONTENT_LENGTH, sizeof(CONTENT_LENGTH) - 1)) {
        char *p = buf + sizeof(CONTENT_LENGTH) - 1;
        while ((p < buf + n) && (*p == ' ' || *p == ':')) {
            p++;
        }
        strncpy(tmpbuf, p, n);
        size_t s = strtoul(tmpbuf, NULL, 10);
        if (errno == ERANGE) {
            return -1;
        }
        head->content_size = s;
    }
    return 1;
}

int parse_request_line(struct http_head *head, char *buf) {
    int rc = 0;
    for (int i = 0; i < 2; buf = NULL, i++) {
        char *t = strtok(buf, " ");
        if (t == NULL) {
            return 0;
        }
        if (i == 0) {
            head->method = strdup(t);
            rc++;
        }
        if (i == 1) {
            head->path = strdup(t);
            rc++;
        }
    }
    return rc;
}

int conn_read_body(struct conn *c) {
    if (c->buf.len < c->head.content_size) {
        return -1;
    }
    return 0;
}

int conn_read_head(struct conn *c) {
    char *p = memchr(c->buf.ptr, '\r', c->buf.len);
    if (p != NULL && !strncmp(p, "\r\n", 2)) {
        // got a new line
        switch (c->state) {
        case st_read_request_line:
            if (parse_request_line(&c->head, c->buf.ptr) != 2) {
                c->errmsg = "fail to parse request lien";
                return -1;
            }

            c->state = st_read_content_size;
            goto clearbuff;

        case st_read_content_size:
            if (parse_content_size(&c->head, c->buf.ptr, c->buf.len) != 1) {
                c->errmsg = "fail to parse content size";
                return -1;
            }

            c->state = st_read_crlf;
            goto clearbuff;

        case st_read_crlf:
            if (c->buf.ptr == p) {
                if (c->head.content_size == 0) {
                    c->state = st_done;
                } else {
                    c->state = st_read_request_body;
                }
            }

            goto clearbuff;
        case st_done:
        case st_read_request_body:
            c->errmsg = "err state";
            return -1;
        }
    }

    return 0;

clearbuff:
    buff_clear(&c->buf);
    return 0;
}

void handle_conn(struct conn *c) {
    assert(c);

    char buf[10];
    for (;;) {
        bzero(buf, 10);
        int n = read(c->fd, buf, 10);
        if (n < 0) {
            if (errno == EAGAIN) {
                return;
            }

            perror("read");
            goto cleanup;
        }

        if (n == 0) {
            fprintf(stderr, "connection %s:%u closed\n",
                    inet_ntoa(c->addr.sin_addr), ntohs(c->addr.sin_port));
            goto cleanup;
        }

        if (c->state == st_done) {
            return;
        } else if (c->state == st_read_request_body) {
            size_t remain = c->head.content_size - c->buf.len;
            if (n <= remain) {
                buf_write(&c->buf, buf, n);
            } else {
                buf_write(&c->buf, buf, remain);
                c->state = st_done;
                goto reply;
            }
        } else {
            buf_write(&c->buf, buf, n);
            if (conn_read_head(c) < 0) {
                goto badreq;
            }
            if (c->state == st_done) {
                goto reply;
            }
        }
    }

    return;

reply:

#define RSP                                                                    \
    "HTTP/1.0 200 OK\r\n"                                                      \
    "Content-Length: 5\r\n"                                                    \
    "\r\n"                                                                     \
    "hello"
    write(c->fd, RSP, sizeof(RSP) - 1);

    return;

badreq:
    if (c->errmsg != NULL) {
        write(c->fd, c->errmsg, strlen(c->errmsg));
    }

cleanup:
    conn_destory(c);
}

struct server *create_server(const char *host, uint16_t port) {
    struct server *s = malloc(sizeof(struct server));
    if (!s) {
        return NULL;
    }

    bzero(s, sizeof(struct conn));

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
