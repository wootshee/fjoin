/*
Copyright 2017 Denis Shtyrov. All rights reserved.
Use of this source code is governed by MIT
license that can be found in the LICENSE file.
*/

#ifndef FJOIN_FD_H
#define FJOIN_FD_H

int redirect(int src, int dst);

/* Enable/disable FD_CLOEXEC flag on file descriptor */
int clo_exec(int fd, int enable);

#endif