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
    pid_t pid;
    FILE* fd[3];
} worker;

int start_worker(const char* cmd[], worker *w);

#endif