#pragma once

#include <stdint.h>

void st7735_hold(void);
void st7735_init(void);
void st7735_draw_bins(const uint8_t *bins, unsigned n,
                      unsigned mhz, unsigned span_mhz,
                      const char *footer);
int st7735_button(void);
