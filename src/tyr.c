#define __USE_XOPEN
#define _XOPEN_SOURCE 600
#define _GNU_SOURCE
#include <assert.h>
#include <leif/color.h>
#include <leif/util.h>
#include <leif/widget.h>
#include <locale.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <pty.h>
#include <utmp.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <wchar.h>
#include <errno.h>

#include <leif/ui_core.h>
#include <leif/win.h>
#include <leif/ez_api.h>
#include <leif/task.h>
#include <runara/runara.h>


#define CLAMP(val, min, max) ((val) < (min) ? (min) : ((val) > (max) ? (max) : (val)))

#define BUF_SIZE 65535

#define CLR_FOREGROUND 30
#define CLR_FOREGROUND_BRIGHT 90

#define CLR_BACKGROUND 40
#define CLR_BACKGROUND_BRIGHT 100

#define BEGIN_ESCAPE_SEQ 27

#define UTF_SIZE        4
#define ESC_BUF_SIZE    (128*UTF_SIZE)
#define ESC_PARAM_SIZE  16

#define MAX_ROWS 4096

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
  int32_t x, y;
} cursor_t;

typedef enum {
  TERM_MODE_CURSOR_KEYS               = 1 << 0,
  TERM_MODE_REVERSE_VIDEO             = 1 << 1,
  TERM_MODE_AUTO_WRAP                 = 1 << 2,
  TERM_MODE_HIDE_CURSOR               = 1 << 3,
  TERM_MODE_MOUSE                     = 1 << 4,
  TERM_MODE_MOUSE_X10                 = 1 << 5,
  TERM_MODE_MOUSE_REPORT_BTN          = 1 << 6,
  TERM_MODE_MOUSE_REPORT_MOTION       = 1 << 7,
  TERM_MODE_MOUSE_REPORT_ALL_EVENTS   = 1 << 8,
  TERM_MODE_MOUSE_REPORT_SGR          = 1 << 9,
  TERM_MODE_REPORT_FOCUS              = 1 << 10,
  TERM_MODE_8BIT                      = 1 << 11,
  TERM_MODE_ALTSCREEN                 = 1 << 12,
  TERM_MODE_BRACKETED_PASTE           = 1 << 13,
  TERM_MODE_INSERT                    = 1 << 14,
  TERM_MODE_LOCK_KEYBOARD             = 1 << 15,
  TERM_MODE_ECHO                      = 1 << 16,
  TERM_MODE_CR_AND_LF                 = 1 << 17,
  TERM_MODE_UTF8                      = 1 << 17,
} termmode_t;

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
} term_color_16_t;

typedef enum {
  ESC_STATE_ON_ESC     = 1,
  ESC_STATE_CSI        = 2,
  ESC_STATE_STR        = 4,  
  ESC_STATE_ALTCHARSET = 8,
  ESC_STATE_STR_END    = 16, 
  ESC_STATE_TEST       = 32, 
  ESC_STATE_UTF8       = 64,
} escape_state_t;

typedef enum {
  CURSOR_STATE_NORMAL   = 0,
  CURSOR_STATE_ONWRAP   = 1,
  CURSOR_STATE_ORIGIN   = 2
} cursor_state_t;

typedef enum {
  CURSOR_ACTION_STORE   = 0,
  CURSOR_ACTION_RESTORE = 1,
} cursor_action_t;

typedef struct {
  char buf[ESC_BUF_SIZE];
  size_t len;            
  char prefix; // . '?', '>', '!' or '\0' if none
  int params[ESC_PARAM_SIZE];
  uint32_t nparams;
  char cmd[2];
} escape_seq_t;

typedef enum {
  FONT_NORM             = 0,
  FONT_BOLD             = 1,
  FONT_DIM              = 2, 
  FONT_ITALIC           = 3,
  FONT_UNDERLINED       = 4,
  FONT_BLINK            = 5,
  FONT_REVERSE          = 6,
  FONT_HIDDEN           = 7,
} term_font_style_t;

typedef struct {
  term_color_16_t bg, fg;
  uint32_t codepoint;
  term_font_style_t font_style;
} cell_t;

typedef struct {
  lf_ui_state_t* ui;
  pty_data_t* pty;
  cursor_t cursor, altcursor;
  cell_t* cells,  *altcells;
  int32_t rows, cols;
  int32_t head, linecount;
  int32_t scrolltop, scrollbottom;
  cursor_t saved_cursor;
  int32_t saved_scrolltop;
  int32_t saved_scrollbottom;
  int32_t saved_head;

  int32_t* tabs;

  escape_seq_t csiseq;
  cursor_state_t cursorstate;
  uint32_t termmode;
  uint32_t escflags;

  uint32_t recentcodepoint;
} state_t;

static state_t s;

void handlechar(uint32_t c);

int utf8decode(const char *s, uint32_t *out_cp) {
  unsigned char c = s[0];
  if (c < 0x80) {
    *out_cp = c;
    return 1;
  } else if ((c >> 5) == 0x6) {
    *out_cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
    return 2;
  } else if ((c >> 4) == 0xE) {
    *out_cp = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    return 3;
  } else if ((c >> 3) == 0x1E) {
    *out_cp = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    return 4;
  }
  return -1; // invalid UTF-8
}

int utf8encode(uint32_t codepoint, char *out) {
  if (codepoint <= 0x7F) {
    out[0] = codepoint;
    return 1;
  } else if (codepoint <= 0x7FF) {
    out[0] = 0xC0 | (codepoint >> 6);
    out[1] = 0x80 | (codepoint & 0x3F);
    return 2;
  } else if (codepoint <= 0xFFFF) {
    out[0] = 0xE0 | (codepoint >> 12);
    out[1] = 0x80 | ((codepoint >> 6) & 0x3F);
    out[2] = 0x80 | (codepoint & 0x3F);
    return 3;
  } else if (codepoint <= 0x10FFFF) {
    out[0] = 0xF0 | (codepoint >> 18);
    out[1] = 0x80 | ((codepoint >> 12) & 0x3F);
    out[2] = 0x80 | ((codepoint >> 6) & 0x3F);
    out[3] = 0x80 | (codepoint & 0x3F);
    return 4;
  }
  return 0;
}

RnTextProps 
render_text_internal(RnState* state, 
                     const char* text, 
                     RnFont* font, 
                     vec2s pos, 
                     RnColor color, 
                     bool render) {

  // Get the harfbuzz text information for the string
  RnHarfbuzzText* hb_text = rn_hb_text_from_str(state, *font, text);

  hb_text->highest_bearing = font->size;

  vec2s start_pos = (vec2s){.x = pos.x, .y = pos.y};

  // New line characters
  const int32_t line_feed       = 0x000A;
  const int32_t line_seperator  = 0x2028;
  const int32_t paragraph_seperator = 0x2029;

  float textheight = 0;

  for (uint32_t i = 0; i < hb_text->glyph_count; i++) {
    // Get the glyph from the glyph index
    // Get the unicode codepoint of the currently iterated glyph
    hb_glyph_info_t* inf = hb_text->glyph_info; 
    uint32_t codepoint = text[inf[i].cluster];

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

char* getrow(uint32_t idx) {
  char* row = malloc((s.cols * 4) + 1);
  char* ptr = row;

  for (uint32_t i = 0; i < (uint32_t)s.cols; i++) {
    uint32_t cp = s.cells[idx * s.cols + i].codepoint;
    ptr += utf8encode(cp, ptr);
  }

  *ptr = '\0'; // Null-terminate
  return row;
}

cell_t* getphysrow(int32_t logicalrow) {
  int32_t physrow = (s.head + logicalrow) % MAX_ROWS;
  return &s.cells[physrow * s.cols];
}

void render_text(lf_ui_state_t* ui, lf_widget_t* widget) {
  lf_text_t* text = (lf_text_t*)widget;
  float y = 0;
  for(uint32_t i = s.head; i < (uint32_t)s.rows + s.head; i++) {
    char* row = getrow(i);
    render_text_internal(
      ui->render_state, 
      row,
      text->font.font,
      (vec2s){.x = 0, .y = y},
      RN_WHITE, true
    );
    y += text->font.pixel_size;
    free(row);
  }
}
void termcomp(lf_ui_state_t* ui) {
  lf_text_h2(ui, s.pty->buf)->base.render = render_text;
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


static size_t readfrompty(void);

void writetopty(const char* buf, size_t len) {
  fd_set fdwrite, fdread;
  size_t writelimit = 256;

  while (len > 0) {
    FD_ZERO(&fdwrite);
    FD_ZERO(&fdread);
    FD_SET(s.pty->masterfd, &fdwrite);
    FD_SET(s.pty->masterfd, &fdread);
    if (pselect(s.pty->masterfd+1, &fdwrite, &fdread, NULL, NULL, NULL) < 0) {
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
      writetopty("\r\n", 2);
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
    lf_task_enqueue(task_rerender_term, (void*)data);
  }

  return n;
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

bool 
isctrl(uint32_t c) {
  // C0 range: 0x00 - 0x1F, plus DEL (0x7F)
  if (((int32_t)c >= 0x00 && c <= 0x1F) || c == 0x7F)
    return true;
  // C1 range: 0x80 - 0x9F
  if (c >= 0x80 && c <= 0x9F)
    return true;

  return false;
}

bool 
isctrlc1(uint32_t c) {
  // C1 range: 0x80 - 0x9F
  if (c >= 0x80 && c <= 0x9F)
    return true;

  return false;
}


void handletab(int count) {
  int32_t x = s.cursor.x;

  if (count > 0) {
    for (int32_t i = 0; i < count && x < s.cols - 1; ++i) {
      ++x;
      while (x < s.cols && !s.tabs[x]) {
        ++x;
      }
    }
  } else if (count < 0) {
    for (int i = 0; i < -count && x > 0; ++i) {
      --x;
      while (x > 0 && !s.tabs[x]) {
        --x;
      }
    }
  }
  if (x >= s.cols) x = s.cols - 1;
  if (x < 0) x = 0;
  s.cursor.x = x;
}

cell_t* cellat(int32_t x, int32_t y) {
  int32_t physrow = (s.head + y) % MAX_ROWS;
  return &s.cells[physrow * s.cols + x];
}
void setcell(int32_t x, int32_t y, uint32_t codepoint) {
  s.cells[y * s.cols + x].codepoint = codepoint;
}

void togglealtscreen(void) {
  cell_t* tmp = s.cells;
  s.cells = s.altcells;
  s.altcells = tmp;
  s.termmode ^= TERM_MODE_ALTSCREEN;
}

void moveto(int32_t x, int32_t y) {
  int miny = 0, maxy = s.rows - 1;
  if(lf_flag_exists(&s.cursorstate, CURSOR_STATE_ORIGIN)) {
    miny = s.scrolltop;
    maxy = s.scrollbottom;
  }
  s.cursor.x = CLAMP(x, 0, s.cols - 1);
  s.cursor.y = CLAMP(y, miny, maxy);
  lf_flag_unset(&s.cursorstate, CURSOR_STATE_ONWRAP);
}

void handlealtcursor(cursor_action_t action) {
  if(action == CURSOR_ACTION_STORE) {
    s.altcursor = s.cursor;
    s.saved_scrollbottom = s.scrollbottom;
    s.saved_scrolltop = s.scrolltop;
    s.saved_head = s.head;
  } else {
    s.cursor = s.altcursor;
    s.scrolltop = s.saved_scrolltop;
    s.scrollbottom = s.saved_scrollbottom;
    s.head = s.saved_head;
    moveto(s.cursor.x, s.cursor.y);
  }
}

void toggleflag(bool set, uint32_t* flags, uint32_t flag) {
  //if(!lf_flag_exists(flags, flag)) return;
  if(set) lf_flag_set(flags, flag);
  else lf_flag_unset(flags, flag);
}

void movetodecom(int32_t x, int32_t y) {
  bool cursororigin = lf_flag_exists(&s.cursorstate, CURSOR_STATE_ORIGIN); 
  moveto(
    x, y + (cursororigin ? s.scrolltop : 0));
}
void 
deletecells(int32_t ncells) {
  if (ncells <= 0 || s.cursor.x >= s.cols)
    return;

  if (s.cursor.x + ncells > s.cols)
    ncells = s.cols - s.cursor.x;

  cell_t* cursorrow = getphysrow(s.cursor.y);

  int32_t src = s.cursor.x + ncells; 
  int32_t dest = s.cursor.x;
  int32_t n = s.cols - src;
  memmove(
    &cursorrow[dest], 
    &cursorrow[src], n * sizeof(cell_t));

  // clear the trailing garbage characters after the move
  for(int32_t x = s.cols - ncells; x < s.cols; x++) {
    cursorrow[x].codepoint = ' ';
  }

}

void insertblankchars(int32_t nchars) {
  if (nchars <= 0 || s.cursor.x >= s.cols)
    return;

  if (s.cursor.x + nchars > s.cols)
    nchars = s.cols - s.cursor.x;

  cell_t* cursorrow = getphysrow(s.cursor.y);

  int32_t src  = s.cursor.x;
  int32_t dest = s.cursor.x + nchars;
  int32_t n = s.cols - dest;

  // Shift right
  memmove(
    &cursorrow[dest], 
    &cursorrow[src], n * sizeof(cell_t));

  // Insert blank cells
  for (int32_t x = src; x < dest; x++) {
    cursorrow[x].codepoint = ' ';
  }
}

void swaprows(uint32_t a, uint32_t b) {
  if (a == b)
    return;

  for (int col = 0; col < s.cols; ++col) {
    int index_a = a * s.cols + col;
    int index_b = b * s.cols + col;

    cell_t temp = s.cells[index_a];
    s.cells[index_a] = s.cells[index_b];
    s.cells[index_b] = temp;
  }
}

void scrollup(int32_t start, int32_t scrolls) {
  for (int32_t i = start; i <= s.scrollbottom - scrolls; i++) {
    swaprows(i, i + scrolls);
  }
  for (int32_t y = s.scrollbottom - scrolls + 1; y <= s.scrollbottom; y++) {
    for (int32_t x = 0; x < s.cols; x++) {
      cellat(x, y)->codepoint = ' '; 
    }
  }
}

void scrolldown(int32_t start, int32_t scrolls) {
  for (int32_t i = s.scrollbottom; i >= start + scrolls; i--) {
    swaprows(i, i - scrolls);
  }
  for (int32_t y = s.scrollbottom - scrolls + 1; y <= s.scrollbottom; y++) {
    for (int32_t x = 0; x < s.cols; x++) {
      cellat(x, y)->codepoint = ' '; 
    }
  }
}

void newline(bool setx) {
  int32_t x = setx ? 0 : s.cursor.x;
  int32_t y = s.cursor.y;
  if (y == s.scrollbottom) {
    scrollup(s.scrolltop, 1);
  } else {
    y++;
  }
  moveto(x, y);
}

void settermmode(
  bool isprivate, 
  bool toggle, 
  int32_t* params, 
  uint32_t nparams) {
  for(uint32_t i = 0; i < nparams; i++) {
    int32_t p = params[i];
    if(isprivate) {
      switch(p) {
        case 1:
          // Toogle cursor keys
          toggleflag(toggle, &s.termmode, TERM_MODE_CURSOR_KEYS);
          break;
        case 5:
          // Toggle reverse video 
          toggleflag(toggle, &s.termmode, TERM_MODE_REVERSE_VIDEO);
          break;
        case 6:
          // Toggle cursor mode origin 
          toggleflag(toggle, &s.cursorstate, CURSOR_STATE_ORIGIN);
          movetodecom(0, 0);
          break;
        case 7:
          // Toggle auto wrap 
          toggleflag(toggle, &s.termmode, TERM_MODE_AUTO_WRAP);
          break;
        // From st.c
        case 0:  /* error (ignored) */
        case 2:  /* decanm -- ansi/vt52 (ignored) */
        case 3:  /* deccolm -- column  (ignored) */
        case 4:  /* decsclm -- scroll (ignored) */
        case 8:  /* decarm -- auto repeat (ignored) */
        case 18: /* decpff -- printer feed (ignored) */
        case 19: /* decpex -- printer extent (ignored) */
        case 42: /* decnrcm -- national characters (ignored) */
        case 12: /* att610 -- start blinking cursor (ignored) */
          break;
        case 25:
          // Toggle cursor 
          toggleflag(toggle, &s.termmode, TERM_MODE_HIDE_CURSOR);
          break;
        case 9:
          toggleflag(toggle, &s.termmode, TERM_MODE_MOUSE);
          toggleflag(toggle, &s.termmode, TERM_MODE_MOUSE_X10);
          break;
        case 1000: 
          toggleflag(toggle, &s.termmode, TERM_MODE_MOUSE);
          toggleflag(toggle, &s.termmode, TERM_MODE_MOUSE_REPORT_BTN);
          break;
        case 1002: 
          toggleflag(toggle, &s.termmode, TERM_MODE_MOUSE);
          toggleflag(toggle, &s.termmode, TERM_MODE_MOUSE_REPORT_MOTION);
          break;
        case 1003: 
          toggleflag(toggle, &s.termmode, TERM_MODE_MOUSE);
          toggleflag(toggle, &s.termmode, TERM_MODE_MOUSE_REPORT_ALL_EVENTS);
          break;
        case 1004: 
          // Report focus events to pty
          toggleflag(toggle, &s.termmode, TERM_MODE_REPORT_FOCUS);
          break;
        case 1006: 
          toggleflag(toggle, &s.termmode, TERM_MODE_MOUSE_REPORT_SGR);
          break;
        case 1034:
          toggleflag(toggle, &s.termmode, TERM_MODE_8BIT);
          break;
        case 1049:
          handlealtcursor(toggle ? CURSOR_ACTION_STORE : CURSOR_ACTION_RESTORE);
          [[fallthrough]]; // continue to 47/1047 to clear and toggle
        case 47: // Alternate screen
        case 1047: {
          bool inaltscreen = lf_flag_exists(&s.termmode, TERM_MODE_ALTSCREEN);
          if (inaltscreen) {
            for (int32_t y = 0; y < s.rows; y++) {
              for (int32_t x = 0; x < s.cols; x++) {
                cellat(x, y)->codepoint = ' ';
              }
            }
          }
          if (toggle != inaltscreen)
            togglealtscreen();

          break; // ✅ Always break here, don't fall into 1048 anymore
        }
        case 1048:
          handlealtcursor(toggle ? CURSOR_ACTION_STORE : CURSOR_ACTION_RESTORE);
          break;
        case 2004: 
          toggleflag(toggle, &s.termmode, TERM_MODE_BRACKETED_PASTE);
          break;
        default:
          break;
      }
    } else {
      switch(p) {
        case 0:  /* Error (IGNORED) */
          break;
        case 2:
          toggleflag(toggle, &s.termmode, TERM_MODE_LOCK_KEYBOARD);
          break;
        case 4:  
          toggleflag(toggle, &s.termmode, TERM_MODE_INSERT);
          break;
        case 12:
          toggleflag(toggle, &s.termmode, TERM_MODE_ECHO);
          break;
        case 20: 
          toggleflag(toggle, &s.termmode, TERM_MODE_CR_AND_LF);
          break;
        default:
          break;
      }
    }
  }
}

bool handleescseq(uint32_t c) {
  switch(c) {
    case '[':
      lf_flag_set(&s.escflags, ESC_STATE_CSI);
      return true;
    case '#':
      s.escflags |= ESC_STATE_TEST;
      return true;
    case '%':
      s.escflags |= ESC_STATE_UTF8;
      return true;
    case 'P':
    case '_':
    case '^':
    case ']':
    case 'k':
      // handleosc
      return true;
    case 'n': 
    case 'o':
      // locking shift
      break;
    case '(': 
    case ')':
    case '*':
    case '+':
      // set charset 
      return true;
    case 'D': 
      if (s.cursor.y == s.scrollbottom) {
        scrollup(s.scrolltop, 1);
      } else {
        moveto(s.cursor.x, s.cursor.y + 1);
      }
      break;
    case 'E': 
      newline(true); 
      break;
    case 'H': 
      s.tabs[s.cursor.x] = 1;
      break;
    case 'M': 
      if (s.cursor.y == s.scrolltop) {
        scrolldown(s.scrolltop, 1);
      } else {
        moveto(s.cursor.x, s.cursor.y - 1);
      }
      break;
    case 'Z': // Identify terminal  
      termwrite("\033[?6c", strlen("\033[?6c"), false);
      break;
    case 'c':
      printf("FULL RESET.\n");
      // TODO: Reset state
      break;
    case '=': 
      // TODO: APP keypad
      break;
    case '>': 
      // TODO: Normal keypad
      break;
    case '7': 
      handlealtcursor(CURSOR_ACTION_STORE);
      break;
    case '8': 
      handlealtcursor(CURSOR_ACTION_RESTORE);
      break;
    case '\\': 
      // TODO: String terminator
      break;
    default:
      break;
  }
  return false;
}

void parsecsi(void) {
  if (!s.csiseq.len) return;
  s.csiseq.buf[s.csiseq.len] = '\0';
  s.csiseq.nparams = 0;
  uint32_t i = 0;
  if (s.csiseq.buf[i] == '?') {
    s.csiseq.prefix = s.csiseq.buf[i];
    i++;
  } else {
    s.csiseq.prefix = '\0';
  }

  char argbuf[64] = {0};
  uint32_t argidx = 0;
  for (; i < s.csiseq.len; i++) {
    if (s.csiseq.nparams >= ESC_PARAM_SIZE) break;
    if (isdigit(s.csiseq.buf[i])) {
      if (argidx < sizeof(argbuf) - 1) {
        argbuf[argidx++] = s.csiseq.buf[i];
      }
    } else if (s.csiseq.buf[i] == ';') {
      argbuf[argidx] = '\0';
      s.csiseq.params[s.csiseq.nparams++] = atoi(argbuf);
      memset(argbuf, 0, sizeof(argbuf));
      argidx = 0;
    } else {
      break;
    }
  }

  if (argidx > 0 && s.csiseq.nparams < ESC_PARAM_SIZE) {
    argbuf[argidx] = '\0';
    s.csiseq.params[s.csiseq.nparams++] = atoi(argbuf);
  }

  s.csiseq.cmd[0] = s.csiseq.buf[i];
  if(i + 1 <= s.csiseq.len - 1) 
    s.csiseq.cmd[1] = s.csiseq.buf[i + 1];
}

void  
handlecsi(void) {
  uint32_t dp = s.csiseq.nparams > 0 ? s.csiseq.params[0] : 1;
  switch(s.csiseq.cmd[0]) {
    case 'b': {
      // print most recent character n times 
      uint32_t n = MIN(dp, SHRT_MAX);
      for(uint32_t i = 0; i < n; i++)
        handlechar(s.recentcodepoint);
      break;
    } 
    case '@': { 
      // Insert n blank chars 
      insertblankchars(dp);
      break;
    }
    case 'A': {
      // Cursor up 
      moveto(s.cursor.x, s.cursor.y - dp); 
      break;
    }
    case 'B':
    case 'e': 
      // Cursor down 
      moveto(s.cursor.x, s.cursor.y + dp);
      break;
    case 'C': 
    case 'a': 
      // Cursor forward 
      moveto(s.cursor.x + dp, s.cursor.y);
      break;
    case 'c': 
      if (s.csiseq.params[0] == 0)
        termwrite("\033[?6c", strlen("\033[?6c"), false);
      break;
    case 'D': 
      // Cursor backward
      moveto(s.cursor.x - dp, s.cursor.y);
      break;
    case 'E':
      // Cursor n down and first col
      moveto(0, s.cursor.y + dp); 
      break;
    case 'F': 
      // Cursor n up and first col
      moveto(0, s.cursor.y - dp); 
      break;
    case 'g': 
      switch (s.csiseq.params[0]) {
        case 0:
          // clear current tab stop
          s.tabs[s.cursor.x] = 0;
          break;
        case 3:
          // clear all tabs
          memset(s.tabs, 0, s.cols * sizeof(*s.tabs));
          break;
        default:
          break;
      }
      break;
    case 'G': 
    case '`': 
      // Move to col
      moveto(dp - 1, s.cursor.y);
      break;
    case 'H': 
    case 'f': {
      // Move to row, col
      uint32_t x = s.csiseq.nparams > 1 ? s.csiseq.params[1] : 1;
      uint32_t y = s.csiseq.nparams > 0 ? s.csiseq.params[0] : 1;
      movetodecom(x-1, y-1);
      break;
    }
    case 'I': 
      // Cursor forward n tabs
      handletab(dp);
      break;
    case 'K':  {
      // clear line
      int32_t op = s.csiseq.params[0]; 
      if(op == 0) {
        // clear line right of cursor
        for(int32_t x = s.cursor.x; x < s.cols; x++) {
          cellat(x, s.cursor.y)->codepoint = ' ';
        }
      } else if(op == 1) {
        // clear line left of cursor
        for(int32_t x = 0; x < s.cursor.x; x++) {
          cellat(x, s.cursor.y)->codepoint = ' ';
        }
      } else if(op == 2) {
        // entire line 
        for(int32_t x = 0; x < s.cols; x++) {
          cellat(x, s.cursor.y)->codepoint = ' ';
        }
      }
      break;
    }
    case 'J': {
      // Clear display
      int32_t op = s.csiseq.params[0]; 
      if(op == 0) {
        // From cursor to end of screen
        for(int32_t x = s.cursor.x; x < s.cols; x++) {
          cellat(x, s.cursor.y)->codepoint = ' ';
        }
        if(s.cursor.y >= s.rows - 1) break;
        for(int32_t y = s.cursor.y + 1; y < s.rows; y++) {
          for(int32_t x = 0; x < s.cols; x++) {
            cellat(x, y)->codepoint = ' ';
          }
        }
      } else if(op == 1) {
        // From begin of screen to cursor
        if(s.cursor.y > 1) {
          for(int32_t y = 0; y < s.cursor.y - 1; y++) {
            for(int32_t x = 0; x < s.cols; x++) {
              cellat(x, y)->codepoint = ' ';
            }
          }
        }
        for(int32_t x = 0; x < s.cursor.x; x++) {
          cellat(x, s.cursor.y)->codepoint = ' ';
        }
      } else if (op == 2) {
        // Entire screen
        for(int32_t y = 0; y < s.rows; y++) {
          for(int32_t x = 0; x < s.cols; x++) {
            cellat(x, y)->codepoint = ' ';
          }
        }
      }
      break;
    }
    case 'S':
      // scroll n lines up
      if (s.csiseq.prefix == '?') break;
      scrollup(s.scrolltop, dp);
      break;
    case 'T':
      // scroll n lines down
      scrolldown(s.scrolltop, dp);
      break;
    case 'L': 
      // insert n lines
      if(s.scrolltop <= s.cursor.y && 
        s.cursor.y <= s.scrollbottom) {
        scrolldown(s.cursor.y, dp);
      }
      break;
    case 'M':
      // delete n lines
      if(s.scrolltop <= s.cursor.y && 
        s.cursor.y <= s.scrollbottom) {
        scrollup(s.cursor.y, dp);
      }
      break;
    case 'X':
      // clear n cells
      for(int32_t x = s.cursor.x; x < s.cursor.x + (int32_t)dp && x < s.cols; x++) {
        cellat(x, s.cursor.y)->codepoint = ' ';
      }
      break;
    case 'h': 
      // Set terminal mode 
      settermmode(s.csiseq.prefix == '?', true, s.csiseq.params, s.csiseq.nparams);
      break;
    case 'l':
      // Reset terminal mode 
      settermmode(s.csiseq.prefix == '?', false, s.csiseq.params, s.csiseq.nparams);
      break;
    case 'P':
      // delete n cells
      deletecells(dp);
      break;
    case 'Z': 
      // Cursor backward n tabs
      handletab(-dp);
      break;
    case 'd':
      // Move to row 
      movetodecom(s.cursor.x, dp - 1);
      break;
    case 'r':
      // set scrolling region
      if (s.csiseq.prefix == '?') break; 
      uint32_t top = s.csiseq.nparams > 0 ? s.csiseq.params[0] - 1 : 0;
      uint32_t bottom = s.csiseq.nparams > 1 ? s.csiseq.params[1] - 1 : s.rows - 1;
      s.scrolltop = top;
      s.scrollbottom = bottom;
      movetodecom(0, 0);
      break;
    case 's':
      handlealtcursor(CURSOR_ACTION_STORE);
      break;
    case 'u': 
      handlealtcursor(CURSOR_ACTION_RESTORE);
      break;
  }
}

void 
handlectrl(uint32_t c) {
  switch(c) {
    case '\f': 
    case '\v':
    case '\n':
      newline(true);
      return;
    case '\t': {
      handletab(1);
      return;
    }
    case '\b': 
      moveto(s.cursor.x - 1, s.cursor.y);
      return;
    case '\r':   
      moveto(0, s.cursor.y);
      return;
    case 0x88:   
      s.tabs[s.cursor.x] = 1;
      break;
    case '\033': /* ESC */
      memset(&s.csiseq, 0, sizeof(s.csiseq));
      lf_flag_unset(&s.escflags, ESC_STATE_CSI|ESC_STATE_ALTCHARSET|ESC_STATE_TEST);
      lf_flag_set(&s.escflags, ESC_STATE_ON_ESC);
      return;
  }
}

void 
handlechar(uint32_t c) {
  lf_flag_unset(&s.cursorstate, CURSOR_STATE_ONWRAP);
  int32_t len = 1, w = 1;
  bool ctrl = isctrl(c);
  char utf8[4];
  if(c < 127 || !lf_flag_exists(&s.termmode, TERM_MODE_UTF8)) {
    utf8[0] = c;
  } else {
    len = utf8encode(c, utf8);
    if (!ctrl) {
      w = wcwidth(c);
      if(w == -1) w = 1;
    }
  }

  if (ctrl) {
    if (isctrlc1(c))
      return;

    handlectrl(c);
    if (s.escflags == 0)
      s.recentcodepoint = 0;
    return;
  } else if(lf_flag_exists(&s.escflags, ESC_STATE_ON_ESC)) {
    if(lf_flag_exists(&s.escflags, ESC_STATE_CSI)) {
      s.csiseq.buf[s.csiseq.len++] = c;
      // Reset escape sequence flags when CSI terminator is encountered
      if ((0x40 <= c && c <= 0x7E) || s.csiseq.len >= sizeof(s.csiseq.buf)-1) {
        parsecsi();
        handlecsi();
        s.escflags = 0;
      }
      return;
    }  else if(lf_flag_exists(&s.escflags, ESC_STATE_UTF8)) {
      //if (c == 'G') // SET UTF8
      //else if (ascii == '@') // UNSET UTF8
    } else if(lf_flag_exists(&s.escflags, ESC_STATE_ALTCHARSET)) {
      // handle alt charset 
    } else if(lf_flag_exists(&s.escflags, ESC_STATE_TEST)) {
      // handle test 
    } else {
      if(handleescseq(c)) return;
    }
    s.escflags = 0;
    return; 
  } 

  cell_t* cell = cellat(s.cursor.x, s.cursor.y); 
  if(lf_flag_exists(&s.termmode, TERM_MODE_AUTO_WRAP) &&
    (s.cursorstate & CURSOR_STATE_ONWRAP)){ 
    newline(true);
    cell = cellat(s.cursor.x, s.cursor.y);
  }


	setcell(s.cursor.x, s.cursor.y, c);
  s.recentcodepoint = c;

  if (s.cursor.x + w < s.cols) {
    moveto(s.cursor.x + w, s.cursor.y);
  } else {
    s.cursorstate |= CURSOR_STATE_ONWRAP;
  }
  (void)cell;
  (void)len;
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
  (void)mods;

  if (action != LF_KEY_ACTION_PRESS) return;

  switch (key) {
    case KeyEnter: {
      char cr = '\r';
      write(s.pty->masterfd, &cr, 1);
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

  lf_win_set_typing_char_cb(win, charcb);
  lf_win_set_key_cb(win, keycb);

  s.ui->root->props.color.a = 255; 


  lf_component(s.ui, termcomp);

  while(s.ui->running) {
    lf_ui_core_next_event(s.ui);
  }
  lf_ui_core_terminate(s.ui);

  cleanup();

  return 0;
}

