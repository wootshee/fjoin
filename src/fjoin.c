/*
Copyright 2017 Denis Shtyrov. All rights reserved.
Use of this source code is governed by MIT
license that can be found in the LICENSE file.
*/

#include <config.h>
#include "escape.h"
#include "fd.h"
#include "worker.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/wait.h>

static int input_delim = '\n';
static int print_input_delim = 1;
static int output_delim = '\n';
static int print_output_delim = 1;
static int error_delim = '\n';
static int print_error_delim = 1;
static int serialize_stderr = 0;
static int numchild = 1;
static FILE* input = NULL;
static char* getdelim_buf = NULL;
static size_t getdelim_buf_size = 0;

void usage() {
  fprintf(stderr, "Usage: fjoin [-c forks] [-e|i|o delimeter] [-f input file] [-nxEIO] command [args]\n");
}

void move_back(worker* workers, int count) {
  if (count <= 1) {
    return;
  }
  worker w = *workers;
  memmove(workers, workers + 1, sizeof(worker) * (count - 1));
  workers[count - 1] = w;
}

ssize_t copy_message(FILE* src, FILE* dst, int delim, int print_delim) {
  ssize_t r, w;
  size_t size;

  r = getdelim(&getdelim_buf, &getdelim_buf_size, delim, src);
  if (r == -1 && (errno == EINTR || errno == EAGAIN)) {
    r = getdelim(&getdelim_buf, &getdelim_buf_size, delim, src);
  }
  if (r <= 0) {
    return feof(src) ? 0 : r;
  }
  size = (size_t) r - ((print_delim || getdelim_buf[r - 1] != delim) ? 0 : 1);
  w = fwrite(getdelim_buf, 1, size, dst);
  if (w != size) {
    return -1;
  }
  return r;
}

int join_output(worker* workers, int num, int src, FILE* dst, int output_delim, int print_output_delim) {
  int i = 0, res = 0;
  const int close_fd = (src == STDOUT_FILENO ? STDERR_FILENO: STDOUT_FILENO);

  /* Close unused descriptors */
  for (i = 0; i < num; ++i) {
    close(workers[i].streams[STDIN_FILENO].fildes);
    if (workers[i].streams[close_fd].fildes != -1)
      close(workers[i].streams[close_fd].fildes);
  }

  for (i = 0; i < num; i++) {
    /* convert pipe descriptors to buffered pipe streams */
    FILE* out = fdopen(workers[i].streams[src].fildes, "r");

    if (!out) {
      num = i;
      goto cleanup;
    }
    workers[i].streams[src].file = out;
  }
  
  i = 0;

  while (res >= 0 && num) {
    FILE* out;

    if (i >= num) {
      i = 0;
    }
    out = workers[i].streams[src].file;

    /* Try to consume one message from child's STDOUT */
    res = (int) copy_message(out, dst, output_delim, print_output_delim);
    if (res == 0) {
      /* child's output has been fully consumed */
      fclose(out);
      move_back(&workers[i], num - i);
      --num;
      continue;
    } else if (res < 0) {
      perror("Failed to copy child's output");
      break;
    }
    ++i;
  }

cleanup:
  for (i = 0; i < num; ++i) {
    fclose(workers[i].streams[src].file);
  }

  return res;
}

int fork_input(worker* workers, int num) {
  int i = 0;
  int res = 0;
  char* line;
  size_t size;
  ssize_t written;

  /* Close unused descriptors */
  for (i = 0; i < num; ++i) {
    close(workers[i].streams[STDOUT_FILENO].fildes);
  }

  /* Convert stdin file descriptors to buffered streams */
  for (i = 0; i < num; ++i) {
    FILE* in = fdopen(workers[i].streams[STDIN_FILENO].fildes, "w");
    if (!in) {
      num = i;
      goto cleanup;
    }
    workers[i].streams[STDIN_FILENO].file = in;
  }

  i = 0;

  /*
    Distribute input lines to worker processes in round-robin manner
  */

  while ((res = copy_message(input, workers[i].streams[STDIN_FILENO].file, input_delim, print_input_delim)) > 0) {
    /* switch to next child process */
    ++i;
    if (i == num) {
      i = 0;
    }
  }
  
  if (res != 0) {
    perror("fork_input()");
  }

cleanup:
  for (i = 0; i < num; ++i) {
    /* Close write end of worker's STDIN pipe to signal the EOF to worker process */
    res = fclose(workers[i].streams[STDIN_FILENO].file);
    if (res == -1) {
      perror("fclose() on child STDIN failed");
    }
  }
  return res;
}

int run(int argc, char* argv[]) {
  int i;
  int res = 1;
  pid_t pid_join_output = -1, pid_join_error = -1;

  worker* workers = (worker*) malloc(numchild * sizeof(worker));
  if (!workers) {
    perror(NULL);
    return 1;
  }

  /*
    Run worker processes with their stdin, stdout and stderr redirected to pipes
  */
  for (i = 0; i < numchild; ++i) {
    if (0 != start_worker(argv, &workers[i], serialize_stderr)) {
      perror("Cannot start worker process");
      goto cleanup;
    }
  }

  /*
    Make stderr and stdout pipes inheritable by join process
  */
  for (i = 0; i < numchild; ++i) {
    if (
      -1 == clo_exec(workers[i].streams[STDIN_FILENO].fildes, 0) ||
      -1 == clo_exec(workers[i].streams[STDOUT_FILENO].fildes, 0)
    ) {
      perror("Cannot change pipe's mode");
      goto cleanup;
    }
  }

  /*
    Create child process that joins the stdout streams of worker processes
  */

  if (0 == (pid_join_output = fork())) {
    close(STDIN_FILENO);
    res = join_output(workers, numchild, STDOUT_FILENO, stdout, output_delim, print_output_delim);
    fflush(stdout);
  } else if (pid_join == -1) {
    perror("Cannot fork child process");
    res = -1;
  } else if (serialize_stderr) {
    /*
      Create child process that joins the stderr streams of worker processes
    */
    if (0 == (pid_join_error = fork()))) {
      close(STDIN_FILENO);
      close(STDOUT_FILENO);
      res = join_output(workers, numchild, STDERR_FILENO, stderr, error_delim, print_error_delim);
      fflush(stderr);
    } else if (pid_join_error == -1) {
      perror("Cannot fork child process");
      res = -1;
      if (-1 == kill(pid_join_output, SIGKILL)) {
        perror("Failed to send KILL signal to child process");
        exit(-1);
      }
    } else {
      res = 0;
    }
  } else {
    res = 0;
  }

  if (pid_join_output > 0) {
    int r;
    pid_t p;

    if (res == 0) {
      close(STDOUT_FILENO);
      res = fork_input(workers, numchild);
    }

    for (
      p = waitpid(pid_join, &r, 0);
      p == -1 && errno == EINTR;
      p = waitpid(pid_join, &r, 0)
    );
    
    if (p == -1) {
      perror("waitpid() failed");
      res = -1;
    } else if (!(WIFEXITED(r))) {
      res = 1;
    } else if (WEXITSTATUS(r) != 0) {
      res = WEXITSTATUS(r);
    }
  }

cleanup:
  if (getdelim_buf) {
    free(getdelim_buf);
  }
  free(workers);
  return res;
}

int main(int argc, char* argv[]) {
  int ch = 0;
  int res = 0;
  if (argc < 2) {
    usage();
    return 1;
  }

  input = stdin;

   /* Parse command line */
  while ((ch = getopt(argc, argv, "0:c:i:f:o:nIO")) != -1) {
    switch (ch) {
      case 'c':
        numchild = (int) strtol(optarg, NULL, 10);
        if (!numchild && errno) {
          usage();
          return 1;
        }
        break;
      case 'i':
        if ((input_delim = unescape(optarg)) == 0 && errno != 0) {
          usage();
          return 1;
        }
        break;
      case 'I':
        print_input_delim = 0;
        break;
      case 'f':
        input = fopen(optarg, "r");
        if (!input || -1 == clo_exec(fileno(input), 1)) {
          perror("main()");
          return -1;
        }
        break;
      case 'o':
        if ((output_delim = unescape(optarg)) == 0 && errno != 0) {
          usage();
          return 1;
        }
        break;
      case 'O':
        print_output_delim = 0;
        break;
      case '?':
      default:
        usage();
        return 1;
    }
  }
  argc -= optind;
  if (argc == 0) {
    usage();
    return 1;
  }
  argv += optind;
  res = run(argc, argv);
  fclose(input);
  return res;
}
