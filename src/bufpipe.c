/*
Copyright 2017 Denis Shtyrov. All rights reserved.
Use of this source code is governed by MIT
license that can be found in the LICENSE file.
*/

#include "bufpipe.h"

#include <assert.h>
#include <fcntl.h>
#include <unistd.h>

int buf_pipe(int fd, BUF_PIPE* p) {
    int res;

    /* initialize pipe as empty */
    p->fd = fd;
    p->pos = 0;
    p->size = 0;

    /* switch pipe to non-blockin I/O mode */
    res = fcntl(fd, F_GETFL, 0);
    if (res >= 0) {
        res = fcntl(fd, F_SETFL, res | O_NONBLOCK);
    }
    return res;
}

int buf_pipe_empty(const BUF_PIPE* p) {
    return p->pos >= p->size;
}

size_t buf_pipe_peek(BUF_PIPE* p, char** buf) {
    if ((size_t)p->pos >= p->size) {
        return 0;
    }
    *buf = p->buf + p->pos;
    return p->size - p->pos;
}

size_t buf_pipe_consume(BUF_PIPE* p, size_t size) {
    assert(size + p->pos <= sizeof(p->buf));
    return p->pos += size;
}

ssize_t buf_pipe_read(BUF_PIPE* p) {
    ssize_t res;
    assert(buf_pipe_empty(p));
    res = read(p->fd, p->buf, sizeof(p->buf));
    if (res >= 0) {
        p->size = (size_t) res;
        p->pos = 0;
    }
    return res;
}

int buf_pipe_close(BUF_PIPE* p) {
    int res = close(p->fd);
    if (res == 0) {
        p->fd = -1;
        p->pos = 0;
        p->size = 0;
    }
    return res;
}