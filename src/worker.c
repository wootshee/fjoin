/*
Copyright 2017 Denis Shtyrov. All rights reserved.
Use of this source code is governed by MIT
license that can be found in the LICENSE file.
*/

#include "worker.h"
#include "fd.h"

#include <stdlib.h>

int start_worker(int delim, char* cmd[], worker *w) {
  int pipes[3][2];
  int i, j;
  for (i = STDIN_FILENO; i <= STDERR_FILENO; ++i) {
    if (-1 == pipe(pipes[i])) {
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
  close(pipes[STDOUT_FILENO][1]);
  close(pipes[STDERR_FILENO][1]);

  /* Initialize standard streams */
  w->streams[STDIN_FILENO].fd = pipes[STDIN_FILENO][1];
  w->streams[STDOUT_FILENO].fd = pipes[STDOUT_FILENO][0];
  w->streams[STDERR_FILENO].fd = pipes[STDERR_FILENO][0];

  /*
    Every child process must inherit only the file descriptors used for communication between
    the child process and its parent. Descriptors used for communication with other children
    must not be inherited to prevent deadlocks.
  */
  if (
    -1 == clo_exec(w->streams[STDIN_FILENO].fd, 1) ||
    -1 == clo_exec(w->streams[STDOUT_FILENO].fd, 1) ||
    -1 == clo_exec(w->streams[STDERR_FILENO].fd, 1)
  ) {
    perror("Cannot change pipe mode");
    goto errexit;
  }

  return 0; 
  
errexit:
  for (j = 0; j < i; ++j) {
    close(w->streams[j].fd);
  }
  return -1;
}