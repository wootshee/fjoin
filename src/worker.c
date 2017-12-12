/*
Copyright 2017 Denis Shtyrov. All rights reserved.
Use of this source code is governed by MIT
license that can be found in the LICENSE file.
*/

#include "worker.h"
#include "fd.h"

int start_worker(int delim, const char* cmd[], worker *w) {
  int pipes[3][2];
  int i, j;
  for (i = STDIN_FILENO; i <= STDERR_FILENO; ++i) {
    if (-1 == pipe(pipes[j])) {
      perror("Cannot create pipe");
      goto errexit;
    }
  }
  w->pid = fork();
  if (w->pid == 0) {
    /*
      Child process redirects it's standard file descriptors to pipes
      and executes the program specified on command line
    */
    if (
      -1 == redirect(pipes[STDIN_FILENO][0], STDIN_FILENO) ||
      -1 == redirect(pipes[STDOUT_FILENO][1], STDOUT_FILENO) ||
      -1 == redirect(pipes[STDERR_FILENO][1], STDERR_FILENO)
    ) {
      perror("Cannot create worker");
      exit(1);
    }
    close(pipes[STDIN_FILENO][1]);
    close(pipes[STDOUT_FILENO][0]);
    close(pipes[STDERR_FILENO][0]);
    if (-1 == execvp(cmd[0], &cmd[0])) {
      perror("Failed to execute worker process");
      if (delim != '\n') {
        fprintf(stderr, "%c", delim);
      }
      return 1;
    }
  }
  /*
    Parent process
  */

  /* Close unused ends of pipes */
  close(pipes[STDIN_FILENO][0]);
  pipes[STDIN_FILENO][0] = -1;
  close(pipes[STDOUT_FILENO][1]);
  pipes[STDOUT_FILENO][1] = -1;
  close(pipes[STDERR_FILENO][1]);
  pipes[STDERR_FILENO][1] = -1;
  /*
    Every child process must inherit only the file descriptors used for communication between
    the child process and its parent. Descriptors used for communication with other children
    must not be inherited to prevent deadlocks.
  */
  if (
    -1 == clo_exec(pipes[STDIN_FILENO][1], 1) ||
    -1 == clo_exec(pipes[STDOUT_FILENO][0], 1) ||
    -1 == clo_exec(pipes[STDERR_FILENO][0], 1)
  ) {
    perror("Cannot change pipe mode");
    goto errexit;
  }
  /*
    Use buffered I/O on pipes
  */
  if (
    NULL == (w->fd[STDIN_FILENO] = fdopen(pipes[STDIN_FILENO][1], "w")) ||
    NULL == (w->fd[STDOUT_FILENO] = fdopen(pipes[STDOUT_FILENO][0], "r")) ||
    NULL == (w->fd[STDERR_FILENO] = fdopen(pipes[STDERR_FILENO][0], "r"))
  ) {
    perror("fdopen() error");
    goto errexit;
  }

  return 0; 
  
errexit:
  for (j = 0; j < i; ++j) {
    if (w->fd[j]) {
      /* Pipe has been fdopen'ed */
      fclose(w->fd[j]);
    } else if (pipes[j][0] != -1) {
      close(pipes[j][0]);
    } else {
      close(pipes[j][1]);
    }
  }
  return -1;
}