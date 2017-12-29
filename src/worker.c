/*
Copyright 2017 Denis Shtyrov. All rights reserved.
Use of this source code is governed by MIT
license that can be found in the LICENSE file.
*/

#include "worker.h"
#include "fd.h"

#include <stdlib.h>

int start_worker(char* cmd[], int num, int total, int serialize_stderr, worker *w) {
  int pipes[3][2] = {-1};
  char buf[16];
  int i;
  int j = serialize_stderr ? STDERR_FILENO : STDOUT_FILENO;
  for (i = STDIN_FILENO; i <= j; ++i) {
    if (-1 == pipe(pipes[i])) {
      perror("Cannot create pipe");
      goto errexit;
    }
  }
  /* Report to a child process its relative number and total number of child processes */
  snprintf(buf, sizeof(buf) / sizeof(*buf), "%d", num);
  if (-1 == setenv("FJOIN_CHILD", buf, 1)) {
    perror("Cannot set FJOIN_CHILD environment variable");
    goto errexit;
  }
  snprintf(buf, sizeof(buf) / sizeof(*buf), "%d", total);
  if (-1 == setenv("FJOIN_CHILDREN", buf, 1)) {
    perror("Cannot set FJOIN_CHILDREN environment variable");
    goto errexit;
  }
  w->pid = fork();
  if (w->pid == 0) {
    /*
      Child process redirects it's STDIN and STDERR file descriptors to pipes
      and executes the program specified on command line. STDERR becomes shared
      with parent process.
    */
    if (
      -1 == redirect(pipes[STDIN_FILENO][0], STDIN_FILENO) ||
      -1 == redirect(pipes[STDOUT_FILENO][1], STDOUT_FILENO) ||
      -1 == (serialize_stderr ? redirect(pipes[STDERR_FILENO][1], STDERR_FILENO) : 0)
    ) {
      perror("Cannot create worker");
      goto errexit;
    }
    close(pipes[STDIN_FILENO][1]);
    close(pipes[STDOUT_FILENO][0]);
    if (serialize_stderr)
      close(pipes[STDERR_FILENO][0]);
    if (-1 == execvp(cmd[0], &cmd[0])) {
      perror("Failed to execute worker process");
      goto errexit;
    }
  }
  /*
    Parent process
  */
  unsetenv("FJOIN_CHILDREN");
  unsetenv("FJOIN_CHILD");
  /* Close unused ends of pipes */
  close(pipes[STDIN_FILENO][0]);
  close(pipes[STDOUT_FILENO][1]);
  if (serialize_stderr)
      close(pipes[STDERR_FILENO][1]);

  /* Initialize standard streams */
  w->streams[STDIN_FILENO].fildes = pipes[STDIN_FILENO][1];
  w->streams[STDOUT_FILENO].fildes = pipes[STDOUT_FILENO][0];
  w->streams[STDERR_FILENO].fildes = pipes[STDERR_FILENO][0];

  /*
    Every child process must inherit only the file descriptors used for communication between
    the child process and its parent. Descriptors used for communication with other children
    must not be inherited to prevent deadlocks.
  */
  if (
    -1 == clo_exec(w->streams[STDIN_FILENO].fildes, 1) ||
    -1 == clo_exec(w->streams[STDOUT_FILENO].fildes, 1) ||
    -1 == (serialize_stderr ? clo_exec(w->streams[STDERR_FILENO].fildes, 1) : 0)
  ) {
    perror("Cannot change pipe's mode");
    goto errexit;
  }

  return 0; 
  
errexit:
  for (j = 0; j < i; ++j) {
    close(w->streams[j].fildes);
  }
  return -1;
}