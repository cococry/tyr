#include "pty.h"

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
#include "render.h"

pty_data_t* setuppty(void) {
  pty_data_t* data = malloc(sizeof(*data));
  data->buf = malloc(BUF_SIZE);
  memset(data->buf, 0, BUF_SIZE);
  data->buflen = 0;


  tcgetattr(STDIN_FILENO, &data->prevterm); 
  struct termios raw = data->prevterm;
  cfmakeraw(&raw);
  tcsetattr(STDIN_FILENO, TCSANOW, &raw);

  data->childpid = forkpty(&data->masterfd, NULL, NULL, NULL);
  if (data->childpid == -1) {
    fprintf(stderr, "twr: failed to call forkpty().\n");
    perror("forkpty");
    return NULL;
  }

  if (data->childpid == 0) {
    // Child: replace with shell
    execlp("/usr/bin/bash", "/usr/bin/bash", (char *)NULL);
    perror("execlp");
    fprintf(stderr, "twr: failed to call execlp() with bash.\n");
    perror("execlp");
    return NULL;
  }

  return data;
}

void* ptyhandler(void* data) {
  pty_data_t* pty = (pty_data_t*)data;
  fd_set fds;
  while (1) {
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    FD_SET(pty->masterfd, &fds);
    int nfds = (STDIN_FILENO > pty->masterfd ? STDIN_FILENO : pty->masterfd) + 1;

    if (select(nfds, &fds, NULL, NULL, NULL) < 0) {
      perror("select");
      break;
    }
    if (FD_ISSET(pty->masterfd, &fds)) {
      readfrompty();
    }
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
    return n;
  } else if (n == -1) {
    fprintf(stderr, "tyr: failed to read from shell: %s\n", strerror(errno));
    exit(1);
  }

  buflen += n;

  // decode as much as we can
  int i = 0;
  while (i < buflen) {
    uint32_t c;
    int len = utf8decode((const char*)&readbuf[i], &c);
    if (len < 1 || i + len > buflen) break; // incomplete UTF-8 sequence
    handlechar(c);
    i += len;
  }

  // move any remaining incomplete sequence to the beginning
  if (i < buflen)
    memmove(readbuf, readbuf + i, buflen - i);
  buflen -= i;

  // queue a rerender task
  if (s.ui) {
    task_data_t* data = malloc(sizeof(*data));
    data->ui = s.ui;
    lf_task_enqueue(taskrerender, (void*)data);
  }

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
