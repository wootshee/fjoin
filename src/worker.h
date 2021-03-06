/*
Copyright 2017 Denis Shtyrov. All rights reserved.
Use of this source code is governed by MIT
license that can be found in the LICENSE file.
*/

#ifndef FJOIN_WORKER_H
#define FJOIN_WORKER_H

#include <stdio.h>
#include <unistd.h>

typedef struct _worker {
    /* child process id */
    pid_t pid;
    /* file descriptors or FILE pointers for standard streams */
    union {
        int      fildes;
        FILE*    file;
    }  streams[3];
} worker;

int start_worker(char* cmd[], int num, int total, int serialize_stderr, worker *w);

#endif