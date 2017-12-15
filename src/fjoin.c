/*
Copyright 2017 Denis Shtyrov. All rights reserved.
Use of this source code is governed by MIT
license that can be found in the LICENSE file.
*/

#include <config.h>

#include "bufpipe.h"
#include "fd.h"
#include "worker.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

void move_back(worker* workers, int pos) {
  if (pos == 0) {
    return;
  }
  worker w = *workers;
  memmove(workers, workers + 1, sizeof(worker) * (pos - 1));
  workers[pos] = w;
}

ssize_t copy_message(BUF_PIPE* src, FILE* dst) {
  ssize_t r = 0;
  char* buf, *pos;
  size_t size;

  if (buf_pipe_empty(src)) {
    /* read next message(s) from pipe */
    r = buf_pipe_read(src);
    if (r <= 0)
      return r;
  }

  /*
    Consume one message from pipe's buffer
  */
  size = buf_pipe_peek(src, &buf);
  
  for (pos = memchr(buf, delim, size); !pos; pos = memchr(buf, delim, size)) {
    /*
      Message is partially contained in pipe's buffer - copy the available portion
      and issue a blocking read on pipe to retrieve the remaining part(s)
    */
    r = fwrite(buf, 1, size, dst);
    if (r > 0) {
      buf_pipe_consume(src, (size_t)r);
    }
    if (r != size) {
      return -1;
    }
    r = buf_pipe_read(src);
    if (r <= 0) {
      return r;
    }
    size = buf_pipe_peek(src, &buf);
  }

  /* found end of message - copy it to destination stream */
  size = pos - buf + (print_delim ? 1 : 0);
  r = fwrite(buf, 1, size, dst);
  if (r > 0) {
    buf_pipe_consume(src, (size_t)r);
  }
  if (r != size) {
    return -1;
  }
  if (!print_delim) {
    /* skip delimiter */
    buf_pipe_consume(src, 1);
    ++r;
  }
  return r;
}

int join_output(int parent_err, worker* workers, int num) {
  int i = 0, res = 0;
  char* buf = NULL;
  ssize_t size = 0;

  BUF_PIPE perr;

  /* Close unused descriptors */
  for (i = 0; i < num; ++i) {
    close(workers[i].streams[STDIN_FILENO].fildes);
  }

  if (-1 == buf_pipe(parent_err, &perr)) {
    perror("Cannot open parent STDERR");
    res = -1;
  } else for (i = 0; i < num; i++) {
    /* convert pipe descriptors to buffered pipe streams */
    BUF_PIPE* out = &workers[i].streams[STDOUT_FILENO].bpipe;
    BUF_PIPE* err = &workers[i].streams[STDERR_FILENO].bpipe;

    if (-1 == buf_pipe(workers[i].streams[STDOUT_FILENO].fildes, out)) {
      num = i;
      goto cleanup;
    }
    if (-1 == buf_pipe(workers[i].streams[STDERR_FILENO].fildes, err)) {
      num = i;
      buf_pipe_close(out);
      goto cleanup;
    }
  }
  
  i = 0;

  while (res >= 0 && num) {
    int nfds = -1, child_message_consumed = 0;
    BUF_PIPE* out;
    BUF_PIPE* err;
    fd_set fdread;

    FD_ZERO(&fdread);

    /* First try to consume all error messages sent so far by parent process */
    if (parent_err != -1) {
      while ((res = copy_message(&perr, stderr) > 0)) { }
      if (res == -1) {
        if (errno == EWOULDBLOCK) {
          /* parent STDERR pipe has no messages - read it later */
          FD_SET(parent_err, &fdread);
          if (parent_err > nfds) {
            nfds = parent_err;
          }
        }
        else {
          perror("Failed to read parent's STDERR");
          break;
        }
      } else if (res == 0) {
        /* parent process STDERR has been fully consumed */
        buf_pipe_close(&perr);
        parent_err = -1;
        fprintf(stderr, "perr consumed\n");
        continue;
      }
    }

    out = &workers[i].streams[STDOUT_FILENO].bpipe;
    err = &workers[i].streams[STDERR_FILENO].bpipe;

    /* Try to consume one message from child's STDERR */
    if (err->fd != -1) {
      res = copy_message(err, stderr);
      if (res > 0) {
        ++child_message_consumed;
      } else if (res == 0) {
        /* child's STDERR has been fully consumed */
        buf_pipe_close(err);
        fprintf(stderr, "%d child's stderr consumed\n", i);
        /* Wait until STDOUT pipe receives EOF */
        if (out->fd != -1) {
          fprintf(stderr, "waiting for %d child's stdout\n", i);
          FD_SET(out->fd, &fdread);
          if (out->fd > nfds) {
            nfds = out->fd;
          }
          goto wait_io;
        }
      } else if (errno == EWOULDBLOCK) {
        /* child's STDERR pipe has no messages - read it later */
        FD_SET(err->fd, &fdread);
        if (err->fd > nfds) {
          nfds = err->fd;
        }
      } else {
        perror("Failed to read child's STDERR");
        break;
      }
    }

    /* Try to consume one message from child's STDOUT */
    if (out->fd != -1) {
      res = copy_message(out, stdout);
      if (res > 0) {
        ++child_message_consumed;
      } else if (res == 0) {
        /* child's STDOUT has been fully consumed */
        buf_pipe_close(out);
        fprintf(stderr, "%d child's stdout consumed\n", i);
        if (err->fd != -1) {
          fprintf(stderr, "waiting for %d child's stderr\n", i);
          FD_SET(err->fd, &fdread);
          if (err->fd > nfds) {
            nfds = err->fd;
          }
          goto wait_io;
        }
      } else if (errno == EWOULDBLOCK) {
        /* child's STDOUT pipe has no messages - read it later */
        FD_SET(out->fd, &fdread);
        if (out->fd > nfds) {
          nfds = out->fd;
        }
      } else {
        perror("Failed to read child's STDOUT");
        break;
      }
    }

    if (child_message_consumed) {
      /* switch to consuming message from next child */
      ++i;
      if (i == num) {
        i = 0;
      }
      continue;
    } else if (err->fd == -1 && out->fd == - 1) {
      /* child's output has been fully consumed - exclude it from I/O loop */
      move_back(&workers[i], num - i);
      --num;
      continue;
    }

    /* Child message has not been consumed yet - wait until it arrives */

    nfds++;
wait_io:
    while ((res = select(nfds, &fdread, NULL, NULL, NULL)) == -1) {
      switch(errno) {
        case EAGAIN: 
          sleep(1);
        case EINTR:
          continue;
        default:
          perror("select() failed");
          break;
      }
    }
  }
cleanup:
  if (parent_err != -1) buf_pipe_close(&perr);

  for (i = 0; i < num; ++i) {
    buf_pipe_close(&workers[i].streams[STDOUT_FILENO].bpipe);
    buf_pipe_close(&workers[i].streams[STDERR_FILENO].bpipe);
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
    close(workers[i].streams[STDERR_FILENO].fildes);
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

  while ((line = fgetln(input, &size)) != NULL) {
    written = fwrite(line, 1, size, workers[i].streams[STDIN_FILENO].file);
    if (written != (ssize_t) size) {
        break;
    }
    /* switch to next child process */
    ++i;
    if (i == num) {
      i = 0;
    }
  }
  
  if ((!line && ferror(input)) || ferror(workers[i].streams[STDIN_FILENO].file)) {
    res = -1;
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
      -1 == clo_exec(workers[i].streams[STDIN_FILENO].fildes, 0) ||
      -1 == clo_exec(workers[i].streams[STDOUT_FILENO].fildes, 0) ||
      -1 == clo_exec(workers[i].streams[STDERR_FILENO].fildes, 0)
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
  int res = 0;
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
        if (!input || -1 == clo_exec(fileno(input), 1)) {
          perror("main()");
          return -1;
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
  res = run(argc, argv);
  fclose(input);
  return res;
}
