/*
Copyright 2017 Denis Shtyrov. All rights reserved.
Use of this source code is governed by MIT
license that can be found in the LICENSE file.
*/

#include <config.h>
#include "fd.h"
#include "worker.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/select.h>

static int input_delim = '\n';
static int print_input_delim = 1;
static int output_delim = '\n';
static int print_output_delim = 1;
static int numchild = 1;
static FILE* input = NULL;
static char* getdelim_buf = NULL;
static size_t getdelim_buf_size = 0;

void usage() {
  fprintf(stderr, "Usage: fjoin [-c forks] [-d delimeter] [-f input file] [-nIO] command [args]\n");
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
  ssize_t r = 0;
  size_t size;

  r = getdelim(&getdelim_buf, &getdelim_buf_size, delim, src);
  if (r == -1 && (errno == EINTR || errno == EAGAIN)) {
    r = getdelim(&getdelim_buf, &getdelim_buf_size, delim, src);
  }
  if (r <= 0) {
    return feof(src) ? 0 : r;
  }

  size = (size_t) r - (print_delim ? 0 : 1);
  r = fwrite(getdelim_buf, 1, size, dst);
  if (r != size) {
    return -1;
  }
  return r;
}

int join_output(worker* workers, int num) {
  int i = 0, res = 0;

  /* Close unused descriptors */
  for (i = 0; i < num; ++i) {
    close(workers[i].streams[STDIN_FILENO].fildes);
  }

  for (i = 0; i < num; i++) {
    /* convert pipe descriptors to buffered pipe streams */
    FILE* out = fdopen(workers[i].streams[STDOUT_FILENO].fildes, "r");

    if (!out) {
      num = i;
      goto cleanup;
    }
    workers[i].streams[STDOUT_FILENO].file = out;
  }
  
  i = 0;

  while (res >= 0 && num) {
    FILE* out;

    if (i >= num) {
      i = 0;
    }
    out = workers[i].streams[STDOUT_FILENO].file;

    /* Try to consume one message from child's STDOUT */
    res = (int) copy_message(out, stdout, output_delim, print_output_delim);
    if (res == 0) {
      /* child's STDOUT has been fully consumed */
      fclose(out);
      move_back(&workers[i], num - i);
      --num;
      continue;
    } else if (res < 0) {
      perror("Failed to read child's STDOUT");
      break;
    }
    ++i;
  }

cleanup:
  for (i = 0; i < num; ++i) {
    fclose(workers[i].streams[STDOUT_FILENO].file);
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
  pid_t pid_join = -1;

  worker* workers = (worker*) malloc(numchild * sizeof(worker));
  if (!workers) {
    perror(NULL);
    return 1;
  }

  /*
    Run worker processes with their stdin, stdout and stderr redirected to pipes
  */
  for (i = 0; i < numchild; ++i) {
    if (0 != start_worker(argv, &workers[i])) {
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
      perror("Cannot change pipe mode");
      goto cleanup;
    }
  }

  /*
    Create child process that joins the stdout streams of worker processes
  */

  if (0 == (pid_join = fork())) {
    close(STDIN_FILENO);
    res = join_output(workers, numchild);
    fflush(stdout);
  } else {
    int r;
    pid_t p;
    close(STDOUT_FILENO);
    res = fork_input(workers, numchild);
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
  while ((ch = getopt(argc, argv, "c:i:f:o:nIO")) != -1) {
    switch (ch) {
      case 'c':
        numchild = (int) strtol(optarg, NULL, 10);
        if (!numchild) {
          usage();
          return 1;
        }
        break;
      case 'i':
        input_delim = *optarg;
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
        output_delim = *optarg;
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
