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
    p->pos = sizeof(p->buf);

    /* switch pipe to non-blockin I/O mode */
    res = fcntl(fd, F_GETFL, 0);
    if (res >= 0) {
        res = fcntl(fd, F_SETFL, res | O_NONBLOCK);
    }
    return res;
}

int buf_pipe_empty(const BUF_PIPE* p) {
    return p->pos >= PIPE_BUF;
}

size_t buf_pipe_peek(BUF_PIPE* p, char** buf) {
    *buf = p->buf;
    if ((size_t)p->pos > sizeof(p->buf)) {
        return 0;
    }
    return sizeof(p->buf) - p->pos;
}

size_t buf_pipe_consume(BUF_PIPE* p, size_t size) {
    assert(size + p->pos <= sizeof(p->buf));
    return p->pos += size;
}

ssize_t buf_pipe_read(BUF_PIPE* p) {
    assert(buf_pipe_empty(p));
    return read(p->fd, p->buf, sizeof(p->buf));
}

int buf_pipe_close(BUF_PIPE* p) {
    int res = close(p->fd);
    if (res == 0) {
        p->fd = -1;
        p->pos = sizeof(p->buf);
    }
    return res;
}