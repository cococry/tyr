#include <stdint.h>

#include "tyr.h"

int32_t utf8decode(const char *s, uint32_t *out_cp);

int32_t utf8encode(uint32_t codepoint, char *out);

char* getrowutf8(uint32_t idx);

cell_t* getphysrow(int32_t logicalrow);

bool isctrl(uint32_t c);

bool isctrlc1(uint32_t c);

void handletab(int32_t count);

cell_t* cellat(int32_t x, int32_t y);

void setcell(int32_t x, int32_t y, uint32_t codepoint);

void togglealtscreen(void);

void handlealtcursor(cursor_action_t action);

void moveto(int32_t x, int32_t y);

void movetodecom(int32_t x, int32_t y);

void toggleflag(bool set, uint32_t* flags, uint32_t flag);

void deletecells(int32_t ncells);

void insertblankchars(int32_t nchars);

void scrollup(int32_t start, int32_t scrolls);

void scrolldown(int32_t start, int32_t scrolls);

void newline(bool setx);

void settermmode(
  bool isprivate, 
  bool toggle, 
  int32_t* params, 
  uint32_t nparams);

bool handleescseq(uint32_t c);

void parsecsi(void);

void handlecsi(void);

void handlectrl(uint32_t c);

void handlechar(uint32_t c);

void setdirty(uint32_t rowidx, bool dirty);
