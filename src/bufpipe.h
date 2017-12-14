/*
Copyright 2017 Denis Shtyrov. All rights reserved.
Use of this source code is governed by MIT
license that can be found in the LICENSE file.
*/

#ifndef FJOIN_BUFPIPE_H
#define FJOIN_BUFPIPE_H

#include <limits.h>
#include <stddef.h>
#include <stdio.h>

typedef struct _BUF_PIPE {
    int fd;
    char buf[PIPE_BUF];
    size_t pos;
} BUF_PIPE;

int buf_pipe(int fd, BUF_PIPE* p);
int buf_pipe_empty(const BUF_PIPE* p);
size_t buf_pipe_peek(BUF_PIPE* p, char** buf);
size_t buf_pipe_consume(BUF_PIPE* p, size_t size);
ssize_t buf_pipe_read(BUF_PIPE* p);
int buf_pipe_close(BUF_PIPE* p);

#endif