#pragma once 

#include <leif/asset_manager.h>
#include <leif/leif.h>
#include <leif/util.h>
#include <stdint.h>
#include <termio.h>

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
  int shutdown_pipe[2];
  int notify_pipe[2];
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
  bool dirty;
} cell_t;

typedef enum {
  CHARSET_ASCII = 0,
  CHARSET_ALT   = 1, // DEC Special Graphics
} charset_mode_t;


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

  lf_widget_t* textwidget;

  charset_mode_t charset;

  GLuint fbo;
  GLuint fbo_texture;
  vec2s fbo_size;

  lf_mapped_font_t font;
  float fontadvance;

  _Atomic bool needrender;

  pthread_mutex_t celllock, renderlock;

  bool fullrerender;
} state_t;

extern state_t s;

