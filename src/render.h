#pragma once 

#include <leif/leif.h>

void renderterminalrows(void);

void renderterminalrows_range(uint32_t from, uint32_t to);

void taskrender(void* data);

void enquerender();
