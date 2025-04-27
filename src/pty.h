#pragma once

#include "tyr.h"

pty_data_t* setuppty(void);

void* ptyhandler(void* data);

void writetopty(const char* buf, size_t len);

size_t readfrompty(void);

void termwrite(const char* buf, size_t len, bool mayecho);

uint32_t termhandlecharstream(const char* buf, uint32_t buflen);

