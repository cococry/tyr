#define _XOPEN_SOURCE 600
#define _GNU_SOURCE
#include <stdio.h>
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
#include <signal.h>

#include <leif/ui_core.h>
#include <leif/win.h>

typedef struct {
  char* buf;
  size_t buflen;
  int32_t masterfd;
  struct termios prevterm;
  pthread_t ptythread;
  pid_t childpid;

} pty_data_t;

typedef struct {
  lf_ui_state_t* ui;
  pty_data_t* pty;
} state_t;

static state_t s;

#define BUF_SIZE 65536

void cleanup() {
  tcsetattr(STDIN_FILENO, TCSANOW, &s.pty->prevterm);
  close(s.pty->masterfd);
  pthread_cancel(s.pty->ptythread);  
  pthread_join(s.pty->ptythread, NULL);  
  free(s.pty->buf);
  free(s.pty);
}

void siginthandler(int sig) {
  (void)sig;
  if (s.pty) {
    cleanup();
  }
  exit(0);  
}

void* ptyhandler(void* data) {
  pty_data_t* pty = (pty_data_t*)data;
  fd_set fds;
  char buf[1024];
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
      ssize_t n = read(pty->masterfd, buf, sizeof(buf));
      if (n <= 0) {
        s.ui->running = false;
        break;
      }

      if(pty->buflen + n < BUF_SIZE) {
        memcpy(pty->buf + pty->buflen, buf, n);
        pty->buflen += n;
      }
    }
  }

  return NULL;
}

pty_data_t* setuppty(void) {
  pty_data_t* data = malloc(sizeof(*data));
  data->buf = malloc(BUF_SIZE);
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
    execlp("bash", "bash", (char *)NULL);
    perror("execlp");
    fprintf(stderr, "twr: failed to call execlp() with bash.\n");
    perror("execlp");
    return NULL;
  }

  return data;
}

void charcb(
  lf_ui_state_t* ui,
  lf_window_t win,
  char* utf8,
  uint32_t utf8len
) {
  (void)win;
  (void)ui;
  write(s.pty->masterfd, utf8, utf8len);
}

void keycb(
  lf_ui_state_t* ui,
  lf_window_t win,
  int32_t key, 
  int32_t scancode,
  int32_t action,
  int32_t mods
) {
  (void)win;
  (void)ui;
  (void)scancode;
  (void)action;
  (void)mods;
  if(key == KeyEnter && action == LF_KEY_ACTION_PRESS) {
    write(s.pty->masterfd, "\r\n", 2);
  }
}
void* waitforchild(void* arg) {
  pty_data_t* pty = (pty_data_t*)arg;
  int status;
  waitpid(pty->childpid, &status, 0);
  s.ui->running = 0; // Tell main loop to stop
  return NULL;
}

int main() {
  signal(SIGINT, siginthandler);

  s.pty = setuppty();
  if(!s.pty) return 1;

  pthread_t childwait;
  pthread_create(&childwait, NULL, waitforchild, s.pty);
  if (pthread_create(&s.pty->ptythread, NULL, ptyhandler, (void *)s.pty) != 0) {
    fprintf(stderr, "twr: pty: error creating pty thread\n");
    return 1;
  }
  
  if(lf_windowing_init() != 0) return EXIT_FAILURE;

  lf_window_t win = lf_ui_core_create_window(1280, 720, "hello leif");
  s.ui = lf_ui_core_init(win);

  lf_win_set_typing_char_cb(win, charcb);
  lf_win_set_key_cb(win, keycb);

  while(s.ui->running) {
    lf_ui_core_next_event(s.ui);
  }
  lf_ui_core_terminate(s.ui);

  cleanup();

  return 0;
}

