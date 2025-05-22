#include "pty.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <pty.h>
#include <utmp.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <wchar.h>

#include <leif/task.h>

#include "term.h"

#define FRAME_INTERVAL_SEC (1 / 60.0f) 

pty_data_t* setuppty(void) {
  pty_data_t* data = malloc(sizeof(*data));
  if (!data) {
    perror("malloc");
    return NULL;
  }

  data->buf = malloc(BUF_SIZE);
  if (!data->buf) {
    perror("malloc");
    free(data);
    return NULL;
  }
  memset(data->buf, 0, BUF_SIZE);
  data->buflen = 0;


  data->childpid = forkpty(&data->masterfd, NULL, NULL, NULL);
  if (data->childpid == -1) {
    perror("forkpty");
    free(data->buf);
    free(data);
    return NULL;
  }

  if (data->childpid == 0) {
    execlp("/usr/bin/bash", "bash", (char *)NULL);

    perror("execlp");
    _exit(1); 
  }

  if (pipe(data->shutdown_pipe) == -1) {
    perror("pipe");
    free(data);
    return NULL;
  }
  
  if (pipe(data->notify_pipe) == -1) {
    perror("pipe");
    free(data);
    return NULL;
  }


  // Parent process: return the pty data
  return data;
}

void* ptyhandler(void* data) {

  while (true) {
  }


  return NULL;
}

void writetopty(const char* buf, size_t len) {
  fd_set fdwrite, fdread;
  size_t writelimit = 256;

  while (len > 0) {
    FD_ZERO(&fdwrite);
    FD_ZERO(&fdread);
    FD_SET(s.pty->masterfd, &fdwrite);
    FD_SET(s.pty->masterfd, &fdread);
    if (pselect(s.pty->masterfd+1, &fdread, &fdwrite, NULL, NULL, NULL) < 0) {
      if (errno == EINTR)
        continue;
      fprintf(stderr, "tyr: pselect() failed: %s\n", strerror(errno));
      exit(1);
    }
    if (FD_ISSET(s.pty->masterfd, &fdwrite)) {
      int32_t nwritten = write(
        s.pty->masterfd, buf, 
        MIN(len, writelimit));
      if(nwritten < 0) {
        fprintf(stderr, "tyr: write error on pty: %s\n", strerror(errno));
        exit(1);
      }
      if(nwritten >= (int32_t)len) break;
      if (nwritten < (int32_t)writelimit)
        writelimit = readfrompty();
      len -= nwritten;
      buf += nwritten;
    }
    if (FD_ISSET(s.pty->masterfd, &fdread))
      writelimit = readfrompty();
  }
  return;
}

size_t readfrompty(void) {
  static char readbuf[BUF_SIZE];
  static int buflen = 0;

  int n = read(s.pty->masterfd, readbuf + buflen, sizeof(readbuf) - buflen);
  if (n == 0) {
    s.ui->running = false;
    return 0;
  } else if (n == -1) {
    fprintf(stderr, "tyr: failed to read from shell: %s\n", strerror(errno));
    exit(1);
  }

  buflen += n;

  int i = 0;
  while (i < buflen) {
    uint32_t c;
    int len = utf8decode((const char*)&readbuf[i], &c);
    if (len < 1 || i + len > buflen) break; // incomplete UTF-8 sequence
    handlechar(c);
    i += len;
  }

  // move leftover bytes (incomplete UTF-8) to beginning
  if (i < buflen)
    memmove(readbuf, readbuf + i, buflen - i);
  buflen -= i;

  return n;
}

uint32_t
termhandlecharstream(const char* buf, uint32_t buflen) {
  uint32_t n = 0;
  uint32_t charsize = 0;
  uint32_t codepoint;

  while (n < buflen) {
    if (lf_flag_exists(&s.termmode, TERM_MODE_UTF8)) {
      charsize = utf8decode(buf + n, &codepoint); 
      if (charsize == 0) {
        break;
      }
    } else {
      codepoint = buf[n] & 0xFF;
      charsize = 1;
    }
    handlechar(codepoint);
    n += charsize;
  }
  
  char dummy = 1;
  write(s.pty->notify_pipe[1], &dummy, 1);

  return n;
}

void termwrite(const char* buf, size_t len, bool mayecho) {
  if(mayecho && lf_flag_exists(&s.termmode, TERM_MODE_ECHO)) {
    termhandlecharstream(buf, len);
  }
  if (!lf_flag_exists(&s.termmode, TERM_MODE_CR_AND_LF)) {
    writetopty(buf, len);
    return;
  }

  while (len > 0) {
    if (*buf == '\r') {
      // If the current character is a carriage return, output CRLF
      writetopty("\r\n", 1); // normally \r\n but this is not working so lets just do that
      buf++;
      len--;
    } else {
      // Find the next carriage return or end of buffer
      char *next_cr = memchr(buf, '\r', len);
      if (!next_cr) {
        next_cr = (char*)buf + len;
      }

      // Write the characters till the next CR 
      writetopty(buf, next_cr - buf);

      // Decrement length  
      len -= next_cr - buf;
      // Advance buffeer to the next carriage return 
      buf = next_cr;
    }
  }
}
