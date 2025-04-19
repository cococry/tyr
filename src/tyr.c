#include <leif/color.h>
#include <leif/widget.h>
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
#include <leif/ez_api.h>
#include <leif/task.h>
#include <runara/runara.h>

#define BUF_SIZE 65536

#define CLR_FOREGROUND 30
#define CLR_FOREGROUND_BRIGHT 90

#define CLR_BACKGROUND 40
#define CLR_BACKGROUND_BRIGHT 100

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
} task_data_t;

typedef struct {
  lf_ui_state_t* ui;
  pty_data_t* pty;
} state_t;

typedef enum {
  CLR_BLACK             = 0,
  CLR_RED               = 1,
  CLR_GREEN             = 2,
  CLR_YELLOW            = 3,
  CLR_BLUE              = 4,
  CLR_MAGENTA           = 5,
  CLR_CYAN              = 6,
  CLR_WHITE             = 7,
  CLR_BRIGHT_BLACK      = 8,
  CLR_BRIGHT_RED        = 9,
  CLR_BRIGHT_GREEN      = 10,
  CLR_BRIGHT_YELLOW     = 11,
  CLR_BRIGHT_BLUE       = 12,
  CLR_BRIGHT_MAGENTA    = 13,
  CLR_BRIGHT_CYAN       = 14,
  CLR_BRIGHT_WHITE      = 15,
} term_color_16;

typedef enum {
  FONT_NORM             = 0,
  FONT_BOLD             = 1,
  FONT_DIM              = 2, 
  FONT_ITALIC           = 3,
  FONT_UNDERLINED       = 4,
  FONT_BLINK            = 5,
  FONT_REVERSE          = 6,
  FONT_HIDDEN           = 7,
} term_font_style;

static state_t s;

RnTextProps 
render_text_internal(RnState* state, 
                     const char* text, 
                     RnFont* font, 
                     vec2s pos, 
                     RnColor color, 
                     bool render) {

  // Get the harfbuzz text information for the string
  RnHarfbuzzText* hb_text = rn_hb_text_from_str(state, *font, text);

  // Retrieve highest bearing if 
  // it was not retrived yet.
  if(!hb_text->highest_bearing) {
    for (unsigned int i = 0; i < hb_text->glyph_count; i++) {
      // Get the glyph from the glyph index 
      RnGlyph glyph =  rn_glyph_from_codepoint(
        state, font,
        hb_text->glyph_info[i].codepoint);
      // Check if the glyph's bearing is higher 
      // than the current highest bearing
      if(glyph.bearing_y > hb_text->highest_bearing) {
        hb_text->highest_bearing = glyph.bearing_y;
      }
    }
  }

  vec2s start_pos = (vec2s){.x = pos.x, .y = pos.y};

  // New line characters
  const int32_t line_feed       = 0x000A;
  const int32_t line_seperator  = 0x2028;
  const int32_t paragraph_seperator = 0x2029;

  float textheight = 0;

  for (unsigned int i = 0; i < hb_text->glyph_count; i++) {
    // Get the glyph from the glyph index
    // Get the unicode codepoint of the currently iterated glyph
    uint32_t codepoint = text[hb_text->glyph_info[i].cluster];

    RnGlyph glyph =  rn_glyph_from_codepoint(
      state, font,
      hb_text->glyph_info[i].codepoint); 

    // Check if the unicode codepoint is a new line and advance 
    // to the next line if so
    if(codepoint == line_feed || 
      codepoint == line_seperator || codepoint == paragraph_seperator) {
      float font_height = font->face->size->metrics.height / 64.0f;
      pos.x = start_pos.x;
      pos.y += font_height;
      textheight += font_height;
      continue;
    }



    // Advance the x position by the tab width if 
    // we iterate a tab character
    if(codepoint == '\t') {
      pos.x += font->tab_w * font->space_w;
      continue;
    }

    // If the glyph is not within the font, dont render it
    if(!hb_text->glyph_info[i].codepoint) {
      continue;
    }

    // Calculate position
    float x_advance = hb_text->glyph_pos[i].x_advance / 64.0f; 
    float y_advance = hb_text->glyph_pos[i].y_advance / 64.0f;
    float x_offset = hb_text->glyph_pos[i].x_offset / 64.0f;
    float y_offset = hb_text->glyph_pos[i].y_offset / 64.0f;

    vec2s glyph_pos = {
      .x = pos.x + x_offset,
      .y = pos.y + hb_text->highest_bearing - y_offset 
    };

    // Render the glyph
    if(render) {
      rn_glyph_render(state, glyph, *font, glyph_pos, color);
    }

    if(glyph.height > textheight) {
      textheight = glyph.height;
    }

    // Advance to the next glyph
    pos.x += x_advance;
    pos.y += y_advance;
  }

  return (RnTextProps){
    .width = pos.x - start_pos.x, 
    .height = textheight,
    .paragraph_pos = pos
  };
}


void render_text(lf_ui_state_t* ui, lf_widget_t* widget) {
  lf_text_t* text = (lf_text_t*)widget;
  render_text_internal(
    ui->render_state, 
    s.pty->buf,
    text->font.font,
    (vec2s){.x = 0, .y = 0},
    RN_WHITE, true
  );
}
void termcomp(lf_ui_state_t* ui) {
  lf_text_h1(ui, s.pty->buf)->base.render = render_text;
} 

void task_rerender_term(void* data) {
  if(!data) return;
  task_data_t* task = (task_data_t*)data;
  lf_component_rerender(task->ui, termcomp);
  free(data);
}

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
        if(s.ui) {
          task_data_t* data = malloc(sizeof(*data));
          data->ui = s.ui;
          lf_task_enqueue(task_rerender_term, (void*)data);
        }
      }
    }
  }

  return NULL;
}

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
    write(s.pty->masterfd, "\r", 2);
  }
}
void* waitforchild(void* arg) {
  pty_data_t* pty = (pty_data_t*)arg;
  int status;
  waitpid(pty->childpid, &status, 0);
  s.ui->running = 0; 
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

  lf_ui_core_set_window_hint(LF_WINDOWING_HINT_TRANSPARENT_FRAMEBUFFER, true);
  lf_window_t win = lf_ui_core_create_window(1280, 720, "hello leif");
  s.ui = lf_ui_core_init(win);


  lf_win_set_typing_char_cb(win, charcb);
  lf_win_set_key_cb(win, keycb);
  
  s.ui->root->props.color = LF_NO_COLOR;
  lf_widget_submit_props(s.ui->root);

  lf_component(s.ui, termcomp);

  while(s.ui->running) {
    lf_ui_core_next_event(s.ui);
  }
  lf_ui_core_terminate(s.ui);

  cleanup();

  return 0;
}

