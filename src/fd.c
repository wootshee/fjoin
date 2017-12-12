/*
Copyright 2017 Denis Shtyrov. All rights reserved.
Use of this source code is governed by MIT
license that can be found in the LICENSE file.
*/

#include "fd.h"

#include <fcntl.h>
#include <unistd.h>

int redirect(int src, int dst) {
  int res = dup2(src, dst);
  if (res != -1) {
    close(src);
  }
  return res;
}

/* Enable/disable FD_CLOEXEC flag on file descriptor */
int clo_exec(int fd, int enable) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0)
    return flags;
  if (enable)
    flags |= FD_CLOEXEC;
  else
    flags &= ~FD_CLOEXEC;
  return fcntl(fd, F_SETFL, flags);
}