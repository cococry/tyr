#include <GL/glx.h>
#include <GLFW/glfw3.h>
#include <leif/color.h>
#include <leif/event.h>
#include <leif/ez_api.h>
#include <leif/render.h>
#include <leif/task.h>
#include <leif/ui_core.h>
#include <leif/util.h>
#include <leif/win.h>
#include <runara/runara.h>
#include <stdint.h>
#include <locale.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/select.h>
#include <time.h>
#include <errno.h>

#include "render.h"
#include "tyr.h"
#include "term.h"
#include "pty.h"

state_t s;

static void resizeterm(int32_t w, int32_t h, int32_t cw, int32_t ch);

void cleanup() {
  if (!s.pty) return;
  kill(s.pty->childpid, SIGTERM);
  close(s.pty->masterfd);
  free(s.pty->buf);
  free(s.pty);
  s.pty = NULL;
  free(s.cells);
  free(s.altcells);
  free(s.tabs);
}

void siginthandler(int sig) {
  (void)sig;
  cleanup();
  exit(0);
}

void charcb(lf_ui_state_t* ui, lf_window_t win, char* utf8, uint32_t utf8len) {
  (void)ui; (void)win;
  if(
    strcmp(utf8, "\n") == 0 || 
    strcmp(utf8, "\r") == 0  
  ) return;
  termwrite(utf8, utf8len, false);
}

void keycb(lf_ui_state_t* ui, lf_window_t win, int32_t key, int32_t scancode, int32_t action, int32_t mods) {
  (void)ui; (void)win; (void)scancode; (void)mods;
  if (action != LF_KEY_ACTION_PRESS) return;
  if (key == KeyEnter) {
    char cr = '\r';
    termwrite(&cr, 1, false);
  }
}


void resizecb(lf_ui_state_t* ui, lf_window_t win, uint32_t w, uint32_t h) {
  (void)win;
  ui->render_resize_display(ui->render_state, h, w);

  FT_Face face = s.font.font->face; 
  int line_height = face->size->metrics.height >> 6; 
  int x_advance = face->size->metrics.max_advance >> 6;

  s.fullrerender = true;
  int32_t new_cols = h / x_advance;
  int32_t new_rows = w / line_height;
  if(new_cols != s.cols || new_rows != s.rows)
    resizeterm(h, w, x_advance, line_height);
}

void sendwinsize(int fd, int rows, int cols, int pixelw, int pixelh) {
  struct winsize ws = { .ws_row = rows, .ws_col = cols, .ws_xpixel = pixelw, .ws_ypixel = pixelh };
  ioctl(fd, TIOCSWINSZ, &ws);
}

cell_t* reallocbuf(cell_t* old, int old_w, int old_h, int new_w, int new_h) {
  cell_t* new = malloc(sizeof(cell_t) * new_w * new_h);
  for (int r = 0; r < new_h; ++r) {
    for (int c = 0; c < new_w; ++c) {
      size_t idx = r * new_w + c;
      new[idx] = (r < old_h && c < old_w) ? old[r * old_w + c] : (cell_t){ .codepoint = ' ' };
    }
  }
  free(old);
  return new;
}

void resizeterm(int32_t w, int32_t h, int32_t cw, int32_t ch) {
  int32_t new_cols = w / cw;
  int32_t new_rows = h / ch;
  if (new_cols <= 0 || new_rows <= 0) return;
  int32_t old_cols = s.cols;
  int32_t old_rows = s.rows;
  s.cells = reallocbuf(s.cells, old_cols, old_rows, new_cols, new_rows);
  s.altcells = reallocbuf(s.altcells, old_cols, old_rows, new_cols, new_rows);
  free(s.tabs);
  s.tabs = malloc(sizeof(*s.tabs) * new_cols);
  for (int32_t i = 0; i < new_cols; i++) 
    s.tabs[i] = (i % 8 == 0);
  s.tabs[0] = 0;
  s.fullrerender = true;
  s.cols = new_cols;
  s.rows = new_rows;
  s.cursor.x = s.cursor.x < new_cols ? s.cursor.x : new_cols - 1;
  s.cursor.y = s.cursor.y < new_rows ? s.cursor.y : new_rows - 1;
  s.scrolltop = 0;
  s.scrollbottom = new_rows - 1;
  s.rowsunicode = malloc(sizeof(char*) * s.rows);
  for (int32_t i = 0; i < s.rows; i++) 
    s.rowsunicode[i] = malloc((s.cols * 4) + 1);
  handlealtcursor(CURSOR_ACTION_STORE);
  handlealtcursor(CURSOR_ACTION_RESTORE);
  s.dirty = realloc(s.dirty, new_rows * sizeof(uint8_t));
  sendwinsize(s.pty->masterfd, s.rows, s.cols, w, h);

  s.rowsunicode = realloc(s.rowsunicode, sizeof(char*) * s.rows);
  for(int32_t i = 0; i < s.rows; i++) {
    s.rowsunicode[i] = realloc(s.rowsunicode[i], (s.cols * 4) + 1);
  }
}


void nextevent(lf_ui_state_t* ui) {
  lf_task_flush_all_tasks();


  for (uint32_t i = 0; i < ui->timers.size; i++) {
    if(!ui->timers.items[i].paused)
      lf_timer_tick(ui, &ui->timers.items[i], ui->delta_time, false);
  }

  // Mark expired timers for deletion
  for (uint32_t i = 0; i < ui->timers.size; i++) {
    if (ui->timers.items[i].elapsed >= 
      ui->timers.items[i].duration && !ui->timers.items[i].paused) {
      ui->timers.items[i].expired = true; 
    }
  }


  float cur_time = lf_ui_core_get_elapsed_time();
  ui->delta_time = cur_time - ui->_last_time;
  ui->_last_time = cur_time;

  lf_widget_t* animated = NULL;
  if (lf_widget_animate(ui, ui->root, &animated)) {
    if(animated->_changed_size) {
      lf_widget_shape(ui, lf_widget_flag_for_layout(ui, animated));
    }
  }

  bool rendered = lf_windowing_get_current_event() == LF_EVENT_WINDOW_REFRESH;

  lf_ui_core_shape_widgets_if_needed(ui, ui->root, false);

  vec2s winsize = lf_win_get_size(ui->win);
  if(s.fullrerender) {
    for(int32_t i = 0; i < s.rows; i++) {
      s.dirty[i] = true;
    }
  }
  int32_t largest = 0, smallest = -1;
  for(int32_t i = 0; i < s.rows; i++) {
    if(s.dirty[i]) {
      if(smallest == -1) smallest = i;
      if(i > largest) largest = i;
    }
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
  } else if (smallest != -1) {
    if(smallest > s.last_cursor_row)
      smallest = s.last_cursor_row;
    uint32_t renderheight = (largest - smallest + 1) * s.font.font->line_h;
    uint32_t renderstart = smallest * s.font.font->line_h;
    printf("Rendering from %i to %i.\n", renderstart, renderstart + renderheight);
    for(int32_t i = smallest; i <= largest; i++) {
      s.dirty[i] = 1;
    }
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
  }

  lf_win_swap_buffers(ui->win);
  if (!rendered) {
    ui->_idle_delay_func(ui);
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


void mainloop(void) {
  const int xfd = ConnectionNumber(lf_win_get_x11_display());
  const int ttyfd = s.pty->masterfd;
  const int maxfd = (xfd > ttyfd ? xfd : ttyfd) + 1;

  fd_set rfd;

  while (s.ui->running) {
    FD_ZERO(&rfd);
    FD_SET(ttyfd, &rfd);
    FD_SET(xfd, &rfd);

    int ret = select(maxfd, &rfd, NULL, NULL, NULL);
    if (ret < 0) {
      if (errno == EINTR) continue;
      perror("select");
      break;
    }

    bool should_render = false;

    if (FD_ISSET(ttyfd, &rfd)) {
      readfrompty();
      should_render = true;
    }

    if (FD_ISSET(xfd, &rfd)) {
      lf_windowing_next_event();
      lf_event_type_t e = lf_windowing_get_current_event();

      // Only render for meaningful X events
      if (e == LF_EVENT_KEY_PRESS ||
        e == LF_EVENT_TYPING_CHAR ||
        e == LF_EVENT_WINDOW_REFRESH ||
        e == LF_EVENT_WINDOW_CLOSE ||
        e == LF_EVENT_WINDOW_RESIZE) {
        should_render = true;
      }
    }

    if (should_render) {
      nextevent(s.ui);
    }
  }

  cleanup();
}

typedef GLXContext (*glXCreateContextAttribsARBProc)(
  Display*, GLXFBConfig, GLXContext, Bool, const int*);

Window createxwin(uint32_t w, uint32_t h) {
  static int fbAttribs[] = {
    GLX_X_RENDERABLE,  True,
    GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
    GLX_RENDER_TYPE,   GLX_RGBA_BIT,
    GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
    GLX_RED_SIZE,      8,
    GLX_GREEN_SIZE,    8,
    GLX_BLUE_SIZE,     8,
    GLX_ALPHA_SIZE,    8,
    GLX_DEPTH_SIZE,    24,
    GLX_STENCIL_SIZE,  8,
    GLX_DOUBLEBUFFER,  True,
    None
  };

  int fbcount;
  GLXFBConfig* fbc = glXChooseFBConfig(lf_win_get_x11_display(), DefaultScreen(lf_win_get_x11_display()), fbAttribs, &fbcount);
  if (!fbc) {
    fprintf(stderr, "Failed to get FBConfig\n");
    return 1;
  }

  XVisualInfo* vi = glXGetVisualFromFBConfig(lf_win_get_x11_display(), fbc[0]);
  if (!vi) {
    fprintf(stderr, "No appropriate visual found\n");
    return 1;
  }

  XSetWindowAttributes swa;
  swa.colormap = XCreateColormap(lf_win_get_x11_display(), RootWindow(lf_win_get_x11_display(), vi->screen), vi->visual, AllocNone);
  swa.event_mask = ExposureMask | KeyPressMask | StructureNotifyMask;

  Window win = XCreateWindow(lf_win_get_x11_display(), RootWindow(lf_win_get_x11_display(), vi->screen), 
                             0, 0, w, h, 0, vi->depth, InputOutput,
                             vi->visual,
                             CWColormap | CWEventMask, &swa);

  XStoreName(lf_win_get_x11_display(), win, "tyr");
  XMapWindow(lf_win_get_x11_display(), win);

  // Get modern GL context creation function
  glXCreateContextAttribsARBProc glXCreateContextAttribsARB =
    (glXCreateContextAttribsARBProc)
    glXGetProcAddressARB((const GLubyte *)"glXCreateContextAttribsARB");

  if (!glXCreateContextAttribsARB) {
    fprintf(stderr, "glXCreateContextAttribsARB not supported\n");
    return 1;
  }

  int context_attribs[] = {
    GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
    GLX_CONTEXT_MINOR_VERSION_ARB, 3,
    GLX_CONTEXT_PROFILE_MASK_ARB,  GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
    None
  };

  GLXContext ctx = glXCreateContextAttribsARB(lf_win_get_x11_display(), fbc[0], 0, True, context_attribs);
  if (!ctx) {
    fprintf(stderr, "Failed to create OpenGL context\n");
    return 1;
  }

  XFree(vi);
  XFree(fbc);

  glXMakeCurrent(lf_win_get_x11_display(), win, ctx);

  lf_win_register(win, ctx, 0);


  return win;
}

int main() {
  signal(SIGINT, siginthandler);
  memset(&s, 0, sizeof(s));
  s.cursorstate = CURSOR_STATE_NORMAL;
  s.rowsunicode = NULL;
  s.pty = setuppty();
  setlocale(LC_CTYPE, "");
  if (!s.pty) return 1;
  if (lf_windowing_init() != 0) return EXIT_FAILURE;

  Window win = createxwin(1280, 720);

  s.ui = lf_ui_core_init(win);
  lf_widget_set_font_family(s.ui, s.ui->root, "JetBrains Mono Nerd Font");
  lf_win_set_typing_char_cb(win, charcb);
  lf_win_set_key_cb(win, keycb);
  lf_win_set_resize_cb(win, resizecb);
  s.font = lf_asset_manager_request_font(s.ui, "JetBrains Mono Nerd Font", LF_FONT_STYLE_REGULAR, 28);;
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
  s.ui->root->props.color = (lf_color_t){0, 0, 0, 255};
  s.ui->root->container = (lf_container_t){ .pos = {.x = 0, .y = 0}, .size = {.x = 1280, .y = 720} };

  mainloop();
  return 0;
}
