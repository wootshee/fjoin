/*
Copyright 2017 Denis Shtyrov. All rights reserved.
Use of this source code is governed by MIT
license that can be found in the LICENSE file.
*/

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/select.h>

static int delim = '\n';
static int numchild = 0;
static int print_delim = 1;
static FILE* input = NULL;
static char* getdelim_buf = NULL;
static size_t getdelim_buf_size = 0;

typedef struct _child {
    pid_t pid;
    int in;
    FILE* out;
    FILE* err;
} child;

void usage() {
  fprintf(stderr, "Usage: fjoin [-c forks] [-d delimeter] [-f input file] [-n] program [args]\n");
}

int redirect(int src, int dst) {
  int res = dup2(src, dst);
  if (res == 0) {
    close(src);
  }
  return res;
}

void swap(child* child1, child* child2) {
  child t = *child1;
  *child1 = *child2;
  *child2 = t;
}

int join(child* children, int childnum) {
  int i;
  size_t size = 0, written = 0;
  char* buf = NULL;

  for (i = 0; i < childnum; ++i) {
    int res, nfds, out, err;
    fd_set fdread;
    fd_set fderr;

    out = fileno(children[i].out);
    err = fileno(children[i].err);
    FD_ZERO(&fdread);
    FD_ZERO(&fderr);
    FD_SET(out, &fdread);
    FD_SET(err, &fdread);
    FD_SET(out, &fderr);
    FD_SET(err, &fderr);
    nfds = out > err ? out + 1 : err + 1;
    while ((res = select(nfds, &fdread, NULL, &fderr, NULL)) == 0) {
      if (errno == EAGAIN) {
        sleep(1);
      } else {
        return errno;
      }
    }
    if (res == -1) {
      return errno;
    }
    if (FD_ISSET(out, &fdread) || FD_ISSET(out, &fderr)) {
      ssize_t s = getdelim(&getdelim_buf, &getdelim_buf_size, delim, children[i].out);
      if (s > 0) {
        if (!print_delim && getdelim_buf[s-1] == delim) {
          /* exlude delimiter from output */
          --s;
        }
        written = fwrite(getdelim_buf, 1, (size_t) s, stdout);
        if (written != (size_t) s) {
          return errno;
        }
      }
    }
    if (FD_ISSET(err, &fdread) || FD_ISSET(err, &fderr)) {
      ssize_t s = getdelim(&getdelim_buf, &getdelim_buf_size, delim, children[i].err);
      if (s > 0) {
        if (!print_delim && buf[s-1] == delim) {
          /* exlude delimiter from output */
          --s;
        }
        written = fwrite(getdelim_buf, 1, (size_t) s, stderr);
        if (written != (size_t) s) {
          return errno;
        }
      }
    }
  }
  return 0;
}

int input_loop(child* children, int childnum) {
  int i = 0;

  while (!feof(input)) {
    int res;
    size_t size;
    ssize_t written;
    /* read next input line */
    char* line = fgetln(input, &size);
    if (!line) {
      return feof(input) ? 0 : -1;
    }
    /* send it to child process for processing */
    for (;;) {
      written = write(children[i].in, line, size);
      if (written == (ssize_t) size) {
        break;
      }
      /* child process has been interrupted - exlude it from list */
      if (childnum == 1) {
        fprintf(stderr, "Error: All child processes exited prematurely\n");
        return errno;
      }
      swap(&children[i], &children[childnum - 1]);
      --childnum;
    }
    /* switch to next child process */
    ++i;
    if (i < childnum)
      continue;
    /* now merge the output of child process in the same order as they received their input */
    res = join(children, i);
    if (res) {
      return res;
    }
    i = 0;
  }
  /* merge unprocessed output */
  return join(children, i);
}

int run(int argc, char* argv[]) {
  int i;
  int res = 1;

  child* children = (child*) malloc(numchild * sizeof(child));
  if (!children) {
    fprintf(stderr, "Failed to allocate memory\n");
    return 1;
  }

  /*
    Run child processes with their stdin, stdout and stderr redirected to pipes
  */
  for (i = 0; i < numchild; ++i) {
    int pipes[3][2];
    int j;
    for (j = 0; j < 3; ++j) {
      if (-1 == pipe(pipes[j])) {
        fprintf(stderr, "Failed to create pipe\n");
        goto cleanup;
      }
    }
    children[i].pid = fork();
    if (children[i].pid == 0) {
      /*
        Child process redirects it's standard file descriptors to pipes
        and executes the program specified on command line
      */
      if (
        -1 == redirect(pipes[STDIN_FILENO][0], STDIN_FILENO) ||
        -1 == redirect(pipes[STDOUT_FILENO][1], STDOUT_FILENO) ||
        -1 == redirect(pipes[STDERR_FILENO][1], STDERR_FILENO)
      ) {
        fprintf(stderr, "Failed to redirect standard streams of child process\n");
        goto cleanup;
      }
      free(children);
      close(pipes[STDIN_FILENO][1]);
      close(pipes[STDOUT_FILENO][0]);
      close(pipes[STDERR_FILENO][0]);
      if (-1 == execvp(argv[0], &argv[0])) {
        fprintf(stderr, "Failed to execute %s\n", argv[0]);
        if (delim != '\n') {
          fprintf(stderr, "%c", delim);
        }
        return 1;
      }
    } else {
      /*
        Parent process
      */
      close(pipes[STDIN_FILENO][0]);
      close(pipes[STDOUT_FILENO][1]);
      close(pipes[STDERR_FILENO][1]);
      fcntl(pipes[STDIN_FILENO][1], F_SETFD, FD_CLOEXEC);
      fcntl(pipes[STDOUT_FILENO][0], F_SETFD, FD_CLOEXEC);
      fcntl(pipes[STDERR_FILENO][0], F_SETFD, FD_CLOEXEC);
      children[i].in = pipes[STDIN_FILENO][1];
      children[i].out = fdopen(pipes[STDOUT_FILENO][0], "r");
      children[i].err = fdopen(pipes[STDERR_FILENO][0], "r");
    }
  }

  res = input_loop(children, numchild);
  if (res) {
    perror(NULL);
  }

cleanup:
  if (getdelim_buf) {
    free(getdelim_buf);
  }
  for (i = 0; i < numchild; ++i) {
    int r;
    r = close(children[i].in);
    r = fclose(children[i].out);
    r = fclose(children[i].err);
    waitpid(children[i].pid, &r, 0);
    if (!(WIFEXITED(r))) {
      res = 1;
    } else if (WEXITSTATUS(r) != 0) {
      res = WEXITSTATUS(r);
    }
  }
  free(children);

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
