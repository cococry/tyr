#include <leif/util.h>
#define _XOPEN_SOURCE 600
#define _GNU_SOURCE
#include <locale.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>
#include <signal.h>

#include "render.h"
#include "tyr.h"
#include "term.h"
#include "pty.h"

state_t s;

void handlechar(uint32_t c);

void cleanup() {
  if (kill(s.pty->childpid, SIGTERM) == -1) {
    perror("Failed to send SIGTERM to child");
  }
  if (tcsetattr(STDIN_FILENO, TCSANOW, &s.pty->prevterm) == -1) {
    perror("tcsetattr failed to restore terminal");
  }

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

void charcb(
  lf_ui_state_t* ui,
  lf_window_t win,
  char* utf8,
  uint32_t utf8len
) {
  (void)win;
  (void)ui;
  termwrite(utf8 ,utf8len, false);
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
  (void)mods;

  if (action != LF_KEY_ACTION_PRESS) return;

  switch (key) {
    case KeyEnter: {
      char cr = '\r';
      termwrite(&cr,1, false);
      break;
    }
  }
}
void* waitforchild(void* arg) {
  pty_data_t* pty = (pty_data_t*)arg;
  int status;
  waitpid(pty->childpid, &status, 0);
  s.ui->running = 0; 
  return NULL;
}


void sendwinsize(int fd, int rows, int cols, int pixelw, int pixelh) {
  struct winsize ws = {
    .ws_row = rows,
    .ws_col = cols,
    .ws_xpixel = pixelw,
    .ws_ypixel = pixelh,
  };
  if (ioctl(fd, TIOCSWINSZ, &ws) < 0) {
    perror("ioctl TIOCSWINSZ failed");
  }
}

void resizeterm(int32_t w, int32_t h, int32_t cw, int32_t ch) {
  s.cols = w / cw; 
  s.rows = h / ch; 
  s.cells = realloc(s.cells, sizeof(cell_t) * MAX_ROWS * s.cols);
  for(uint32_t i = 0; i < (uint32_t)MAX_ROWS * s.cols; i++) {
    s.cells[i].codepoint = ' ';
  }
  s.altcells = malloc(sizeof(cell_t) * MAX_ROWS * s.cols);
  for(uint32_t i = 0; i < (uint32_t)MAX_ROWS * s.cols; i++) {
    s.altcells[i].codepoint = ' ';
  }
  s.tabs = malloc(sizeof(*s.tabs) * s.cols);
  for(int32_t i = 0; i < s.cols; i++) {
    s.tabs[i] = i % 5 == 0;
  }
  s.termmode = TERM_MODE_UTF8|TERM_MODE_AUTO_WRAP;
  s.tabs[0] = 0;
}

int main() {
  signal(SIGINT, siginthandler);
  memset(&s, 0, sizeof(s));
  resizeterm(1280, 720, 17, 28);
  s.cursorstate = CURSOR_STATE_NORMAL;
  s.pty = setuppty();
  s.scrolltop = 0;
  s.scrollbottom = s.rows - 1;
  s.escflags = 0;
  s.saved_scrollbottom = s.scrollbottom;
  s.saved_scrolltop = s.scrolltop;
  s.saved_head = s.head;
  setlocale(LC_CTYPE, "");
  sendwinsize(s.pty->masterfd, s.rows, s.cols, 1280, 720);
  if(!s.pty) return 1;

  pthread_t childwait;
  pthread_create(&childwait, NULL, waitforchild, s.pty);
  if (pthread_create(&s.pty->ptythread, NULL, ptyhandler, (void *)s.pty) != 0) {
    fprintf(stderr, "twr: pty: error creating pty thread\n");
    return 1;
  }

  if(lf_windowing_init() != 0) return EXIT_FAILURE;

  lf_ui_core_set_window_hint(LF_WINDOWING_HINT_TRANSPARENT_FRAMEBUFFER, true);
  lf_window_t win = lf_ui_core_create_window(1280, 720, "hello leif");
  s.ui = lf_ui_core_init(win);


  lf_widget_set_font_family(s.ui, s.ui->root, "JetBrains Mono Nerd Font");
  FT_Face face = lf_asset_manager_request_font(s.ui, "JetBrains Mono Nerd Font",
                                               LF_FONT_STYLE_REGULAR, 28).font->face;
  float cell_width = face->size->metrics.max_advance / 64.0f;
  printf("Cell w: %f\n", cell_width);


  lf_win_set_typing_char_cb(win, charcb);
  lf_win_set_key_cb(win, keycb);

  s.ui->root->props.color.r = 0; 
  s.ui->root->props.color.g  = 0; 
  s.ui->root->props.color.b = 0; 
  s.ui->root->props.color.a = 125; 


  lf_component(s.ui, uiterminal);

  while(s.ui->running) {
    lf_ui_core_next_event(s.ui);
  }
  lf_ui_core_terminate(s.ui);

  cleanup();

  return 0;
}

