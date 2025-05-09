#include <GLFW/glfw3.h>
#include <leif/color.h>
#include <leif/ez_api.h>
#include <leif/task.h>
#include <leif/ui_core.h>
#include <leif/util.h>
#include <leif/win.h>
#include <runara/runara.h>
#include <stdatomic.h>
#include <stdint.h>
#define __USE_XOPEN
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

static void resizeterm(int32_t w, int32_t h, int32_t cw, int32_t ch);


void cleanup() {
  if (!s.pty) return;

  printf("cleanup(): shutting down...\n");

  // tell PTY thread to stop
  write(s.pty->shutdown_pipe[1], "x", 1); 

  // tell child to terminate
  if (kill(s.pty->childpid, SIGTERM) == -1) {
    perror("Failed to send SIGTERM to child");
  }

  // join PTY thread safely
  pthread_join(s.pty->ptythread, NULL);

  // close and free PTY
  close(s.pty->masterfd);
  free(s.pty->buf);
  free(s.pty);
  s.pty = NULL;

  // free terminal memory
  free(s.cells);
  free(s.altcells);
  free(s.tabs);
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


void resizecb(
  lf_ui_state_t* ui,
  lf_window_t win,
  uint32_t w, uint32_t h) {
  (void)win;
  (void)ui;
  ui->render_resize_display(ui->render_state, w, h);
  FT_Face face = s.font.font->face; 
  int line_height = face->size->metrics.height >> 6; 
  int x_advance = face->size->metrics.max_advance >> 6; 
  pthread_mutex_lock(&s.renderlock);
  pthread_mutex_lock(&s.celllock);
  resizeterm(w, h, x_advance, line_height);
  atomic_store(&s.needrender, true);
  pthread_mutex_unlock(&s.celllock);
  pthread_mutex_unlock(&s.renderlock);
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

cell_t* reallocbuf(cell_t* old, int old_w, int old_h, int new_w, int new_h) {
  cell_t* new = malloc(sizeof(cell_t) * new_w * new_h);
  for (int r = 0; r < new_h; ++r) {
    for (int c = 0; c < new_w; ++c) {
      size_t idx = r * new_w + c;
      if (r < old_h && c < old_w) {
        new[idx] = old[r * old_w + c];
      } else {
        new[idx].codepoint = ' ';
      }
    }
  }
  free(old);
  return new;
}

void resizeterm(int32_t w, int32_t h, int32_t cw, int32_t ch) {
  int32_t new_cols = w / cw;
  int32_t new_rows = h / ch;

  if (new_cols <= 0 || new_rows <= 0) return;

  // Save old dimensions
  int32_t old_cols = s.cols;
  int32_t old_rows = s.rows;

  s.cells = reallocbuf(s.cells, old_cols, old_rows, new_cols, new_rows);
  s.altcells  = reallocbuf(s.altcells, old_cols, old_rows, new_cols, new_rows);

  s.smallestdirty = 0;
  s.largestdirty = s.rows - 1;

  // Reallocate tab stops
  if (s.tabs) free(s.tabs);
  s.tabs = malloc(sizeof(*s.tabs) * new_cols);
  for (int32_t i = 0; i < new_cols; i++) {
    s.tabs[i] = (i % 8 == 0);
  }
  s.tabs[0] = 0;
  s.fullrerender = true;

  // Update dimensions
  s.cols = new_cols;
  s.rows = new_rows;

  // Clamp cursor
  s.cursor.x = s.cursor.x < new_cols ? s.cursor.x : new_cols - 1;
  s.cursor.y = s.cursor.y < new_rows ? s.cursor.y : new_rows - 1;

  // Adjust margins
  s.scrolltop = 0;
  s.scrollbottom = new_rows - 1;

  handlealtcursor(CURSOR_ACTION_STORE);
  handlealtcursor(CURSOR_ACTION_RESTORE);

  sendwinsize(s.pty->masterfd, s.rows, s.cols, w, h);
}

void nextevent(lf_ui_state_t* ui) {
  lf_task_flush_all_tasks();
  glfwWaitEvents();

  for (uint32_t i = 0; i < ui->timers.size; i++) {
    if(!ui->timers.items[i].paused)
      lf_timer_tick(ui, &ui->timers.items[i], ui->delta_time, false);
  }

  for (uint32_t i = 0; i < ui->timers.size; i++) {
    if (ui->timers.items[i].elapsed >= 
      ui->timers.items[i].duration && !ui->timers.items[i].paused) {
      ui->timers.items[i].expired = true; 
    }
  }

  float cur_time = lf_ui_core_get_elapsed_time();
  ui->delta_time = cur_time - ui->_last_time;
  ui->_last_time = cur_time;


  lf_ui_core_shape_widgets_if_needed(ui, ui->root, false);

  pthread_mutex_lock(&s.renderlock);
  s.fullrerender = true;
  if (atomic_load(&s.needrender)) {
    vec2s winsize = lf_win_get_size(ui->win);
    pthread_mutex_lock(&s.celllock);
    if(s.fullrerender) {
      s.smallestdirty = 0;
      s.largestdirty = s.rows - 1;
    }

    lf_container_t area;
    if(s.fullrerender) {
      area = LF_SCALE_CONTAINER(winsize.x, winsize.y);
      ui->render_clear_color_area(
        ui->root->props.color, 
        area, winsize.y);
      ui->render_begin(ui->render_state);
      renderterminalrows();

      ui->render_end(ui->render_state);
      s.fullrerender = false;
    } else if (s.smallestdirty <= s.largestdirty && s.smallestdirty != INT32_MAX) {
      uint32_t renderheight = (s.largestdirty - s.smallestdirty + 1) * s.font.font->line_h;
      uint32_t renderstart = s.smallestdirty * s.font.font->line_h;

      area = (lf_container_t){
        .pos = (vec2s){.x = 0, .y = renderstart},
        .size = (vec2s){.x = winsize.x, .y = renderheight}
      };

      ui->render_clear_color_area(
        ui->root->props.color, 
        area, winsize.y);
      ui->render_begin(ui->render_state);
      renderterminalrows();
      ui->render_end(ui->render_state);

      // Reset dirty region AFTER drawing
      s.smallestdirty = UINT32_MAX;
      s.largestdirty = 0;
    }

    lf_win_swap_buffers(ui->win);
    atomic_store(&s.needrender, false);
    pthread_mutex_unlock(&s.celllock);
    pthread_mutex_unlock(&s.renderlock);
  } else {
    pthread_mutex_unlock(&s.renderlock);
  }

  lf_windowing_update();
  // Remove expired timers
  for (uint32_t i = 0; i < ui->timers.size;) {
    if (ui->timers.items[i].expired && 
      !ui->timers.items[i].looping && !ui->timers.items[i].paused) {
      lf_vector_remove_by_idx(&ui->timers, i);
    } else {
      i++;
    }
  }

  for (uint32_t i = 0; i < ui->timers.size; i++) {
    if(ui->timers.items[i].expired && 
      ui->timers.items[i].looping && !ui->timers.items[i].paused) {
      ui->timers.items[i].expired = false;
      ui->timers.items[i].elapsed = 0.0f;
    }
  }
}


int main() {
  static const uint32_t initwinw = 1280;
  static const uint32_t initwinh = 720;

  signal(SIGINT, siginthandler);
  memset(&s, 0, sizeof(s));
  s.cursorstate = CURSOR_STATE_NORMAL;
  s.pty = setuppty();
  setlocale(LC_CTYPE, "");
  if(!s.pty) return 1;

  atomic_store(&s.needrender, false);

  pthread_mutex_init(&s.celllock, NULL);
  pthread_mutex_init(&s.renderlock, NULL);

  if(lf_windowing_init() != 0) return EXIT_FAILURE;
  lf_ui_core_set_window_hint(LF_WINDOWING_HINT_TRANSPARENT_FRAMEBUFFER, true);
  lf_window_t win = lf_ui_core_create_window(initwinw, initwinh, "hello leif");
  s.ui = lf_ui_core_init(win);
  lf_widget_set_font_family(s.ui, s.ui->root, "JetBrains Mono Nerd Font");

  lf_win_set_typing_char_cb(win, charcb);
  lf_win_set_key_cb(win, keycb);
  lf_win_set_resize_cb(win, resizecb);


  s.font = lf_asset_manager_request_font(
    s.ui, "JetBrains Mono Nerd Font",
    LF_FONT_STYLE_REGULAR,
    28);
  FT_Face face = s.font.font->face; 
  int line_height = face->size->metrics.height >> 6; 
  int x_advance = face->size->metrics.max_advance >> 6; 
  resizeterm(1280, 720, x_advance, line_height);

  s.scrolltop = 0;
  s.scrollbottom = s.rows - 1;
  s.escflags = 0;
  s.saved_scrollbottom = s.scrollbottom;
  s.saved_scrolltop = s.scrolltop;
  s.saved_head = s.head;
  s.fullrerender = true;
  s.fontadvance = 0;
  s.smallestdirty = 0;
  s.largestdirty = s.rows - 1;

  if (pthread_create(&s.pty->ptythread, NULL, ptyhandler, (void *)s.pty) != 0) {
    fprintf(stderr, "twr: pty: error creating pty thread\n");
    return 1;
  }


  s.ui->root->props.color.r = 0; 
  s.ui->root->props.color.g  = 0; 
  s.ui->root->props.color.b = 0; 
  s.ui->root->props.color.a = 125; 
  s.ui->root->container = (lf_container_t){
    .pos = (vec2s){.x = 0, .y = 0},
    .size = (vec2s){.x = initwinw, .y = initwinh}
  };


  while (s.ui->running) {
    nextevent(s.ui);
  }
  cleanup();

  return 0;
}

