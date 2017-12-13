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
#include <unistd.h>

#include <sys/select.h>

static int delim = '\n';
static int numchild = 1;
static int print_delim = 1;
static FILE* input = NULL;
static char* getdelim_buf = NULL;
static size_t getdelim_buf_size = 0;

void usage() {
  fprintf(stderr, "Usage: fjoin [-c forks] [-d delimeter] [-f input file] [-n] program [args]\n");
}

ssize_t copy_output(FILE* src, FILE* dst) {
  ssize_t in_size = 0;
  size_t size = 0, written = 0;

  in_size = getdelim(&getdelim_buf, &getdelim_buf_size, delim, src);

  if (in_size > 0) {
    if (!print_delim && getdelim_buf[in_size-1] == delim) {
      /* exlude delimiter from output */
      --in_size;
    }
    written = fwrite(getdelim_buf, 1, (size_t) in_size, dst);
    if (written != (size_t) in_size) {
      return -1;
    }
    return written;
  } else if (feof(src)) {
    return 0;
  }
  return in_size;
}

int join_output(int parent_err, worker* workers, int num) {
  int i = 0, res = 0;
  char* buf = NULL;
  int eofed = 0;

  FILE* perr;

  if (!(perr = fdopen(parent_err, "r"))) {
    perror("Cannot open parent STDERR");
    res = -1;
  } else while (res >= 0 && eofed < num) {
    int nfds, out, err;
    fd_set fdread;
    fprintf(stderr, "J0:%d\n", i);
    out = fileno(workers[i].fd[STDOUT_FILENO]);
    err = fileno(workers[i].fd[STDERR_FILENO]);
    FD_ZERO(&fdread);
    FD_SET(out, &fdread);
    FD_SET(err, &fdread);
    if (parent_err != -1) {
      FD_SET(parent_err, &fdread);
    }
    nfds = parent_err;
    if (err > nfds) {
      nfds = err;
    }
    if (out > nfds) {
      nfds = out;
    }
    nfds++;
read_output:
    while ((res = select(nfds, &fdread, NULL, NULL, NULL)) == -1) {
      switch(errno) {
        case EAGAIN: 
          sleep(1);
        case EINTR:
          continue;
        default:
          break;
      }
    }
    fprintf(stderr, "J1:%d\n", res);
    if (res == -1) {
      break;
    }
    /* Check parent STDERR */
    if (FD_ISSET(parent_err, &fdread)) {
      fprintf(stderr, "J2\n");
      res = copy_output(perr, stderr);
      if (res == 0) {
        /*
          parent stderr is fully consumed - exclude it from further I/O processing
        */
        parent_err = -1;
        continue;
      } else if (res < 0) {
        break;
      }
      continue;
    }
    /* Check worker STDERR */
    if (FD_ISSET(err, &fdread)) {
      fprintf(stderr, "J3\n");
      res = copy_output(workers[i].fd[STDERR_FILENO], stderr);
      if (res == 0) {
        ++eofed;
      } else if(res < 0) {
        break;
      }
    }
    /* Check worker STDOUT */
    if (FD_ISSET(out, &fdread)) {
      fprintf(stderr, "J4\n");
      res = copy_output(workers[i].fd[STDOUT_FILENO], stdout);
      if (res == 0) {
        ++eofed;
      } else if(res < 0) {
        break;
      }
    }
    /* switch to next worker */
    ++i;
    if (i == num) {
      i = 0;
    }
  }
  if (perr) fclose(perr);

  for (i = 0; i < num; ++i) {
    /* 
      Since STDIN pipes are not inherited by join process,
      close only STDOUT and STDERR streams
    */
    fclose(workers[i].fd[STDOUT_FILENO]);
    fclose(workers[i].fd[STDERR_FILENO]);
  }

  return res;
}

int fork_input(worker* workers, int num) {
  int i = 0;
  int res = 0;
  char* line;
  size_t size;
  ssize_t written;

  for (i = 0; i < num; ++i) {
    fclose(workers[i].fd[STDERR_FILENO]);
    fclose(workers[i].fd[STDOUT_FILENO]);
  }

  i = 0;

  /*
    Distribute input lines to worker processes in round-robin manner
  */

  while ((line = fgetln(input, &size)) != NULL) {
    written = fwrite(line, 1, size, workers[i].fd[STDIN_FILENO]);
    if (written != (ssize_t) size) {
        break;
    }
    /* switch to next child process */
    ++i;
    if (i == num) {
      i = 0;
    }
  }
  
  if ((!line && ferror(input)) || ferror(workers[i].fd[STDIN_FILENO])) {
    res = 1;
    perror("fork_input()");
  }

  for (i = 0; i < num; ++i) {
    /* Close write end of worker's STDIN pipe to signal the EOF to worker process */
    fclose(workers[i].fd[STDIN_FILENO]);
  }
  return res;
}

int run(int argc, char* argv[]) {
  int i;
  int res = 1;
  pid_t pid_join = -1;
  int err_pipe[2];

  worker* workers = (worker*) malloc(numchild * sizeof(worker));
  if (!workers) {
    perror(NULL);
    return 1;
  }

  /*
    Run worker processes with their stdin, stdout and stderr redirected to pipes
  */
  for (i = 0; i < numchild; ++i) {
    if (0 != start_worker(delim, argv, &workers[i])) {
      fprintf(stderr, "Cannot start worker process\n");
      goto cleanup;
    }
  }

  /*
    Make stderr and stdout pipes inheritable by join process
  */
  for (i = 0; i < numchild; ++i) {
    if (
      -1 == clo_exec(fileno(workers[i].fd[STDOUT_FILENO]), 0) ||
      -1 == clo_exec(fileno(workers[i].fd[STDERR_FILENO]), 0)
    ) {
      perror("Cannot change pipe mode");
      goto cleanup;
    }
  }

  /*
    Create STDERR pipe for sending the error messages from input fork process to
    join output process
  */

  if (-1 == pipe(err_pipe)) {
    perror("Cannot create STDERR pipe");
    goto cleanup;
  }

  /*
    Create child process that joins the stdout and stderr streams of worker processes
  */

  if (0 == (pid_join = fork())) {
    close(STDIN_FILENO);
    close(err_pipe[1]);
    res = join_output(err_pipe[0], workers, numchild);
    fflush(stdout);
    fprintf(stderr, "res = %d,%d\n", res, errno);
  } else {
    int r;
    close(STDOUT_FILENO);
    close(err_pipe[0]);
    if (-1 == redirect(err_pipe[1], STDERR_FILENO)) {
      perror("Cannot redirect STDERR to output join process");
      goto cleanup;
    }
    res = fork_input(workers, numchild);
    fclose(stderr);
    waitpid(pid_join, &r, 0);
    if (!(WIFEXITED(r))) {
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
  if (argc < 2) {
    usage();
    return 1;
  }

  input = stdin;

   /* Parse command line */
  while ((ch = getopt(argc, argv, "c:d:f:n")) != -1) {
    switch (ch) {
      case 'c':
        numchild = (int) strtol(optarg, NULL, 10);
        if (!numchild) {
          usage();
          return 1;
        }
        break;
      case 'd':
        delim = *optarg;
        break;
      case 'f':
        input = fopen(optarg, "r");
        if (!input) {
          perror(NULL);
          return 1;
        }
        break;
      case 'n':
        print_delim = 0;
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
  return run(argc, argv);
}
