#include <freetype/freetype.h>
#include <leif/asset_manager.h>
#include <leif/task.h>
#include <leif/util.h>
#include <pthread.h>
#include <runara/runara.h>
#include <leif/leif.h>
#include <stdatomic.h>
#include <string.h>

#include "tyr.h"
#include "term.h"
#include "render.h"

#define STB_DS_IMPLEMENTATION
#include "../vendor/stb_ds.h"

static RnColor getrendercolor(term_color_16_t color, term_attr_t attr, bool fg);
typedef struct {
  uint32_t begin, end;
  lf_mapped_font_t font;
  RnHarfbuzzText* hb_text;
} rendering_range_t;

typedef struct {
  uint32_t key;
  char* value;
} _fallback_family_hm_element;
  
static _fallback_family_hm_element* fallback_fonts = NULL; 

char* fallbackfamily(uint32_t unicode) {
  FcPattern *pattern = FcPatternCreate();
  FcCharSet *charset = FcCharSetCreate();

  FcCharSetAddChar(charset, unicode);
  FcPatternAddCharSet(pattern, FC_CHARSET, charset);

  FcConfigSubstitute(NULL, pattern, FcMatchPattern);
  FcDefaultSubstitute(pattern);

  FcResult result;
  FcPattern *match = FcFontMatch(NULL, pattern, &result);

  char *font_family = NULL;

  if (match) {
    FcChar8 *family = NULL;
    if (FcPatternGetString(match, FC_FAMILY, 0, &family) == FcResultMatch) {
      font_family = strdup((char*)family); 
    }
    FcPatternDestroy(match);
  }

  FcPatternDestroy(pattern);
  FcCharSetDestroy(charset);

  return font_family; 
}

char* getfallbackfamily(uint32_t codepoint) {
  int index = hmgeti(fallback_fonts, codepoint);
  if (index >= 0) {
    return fallback_fonts[index].value;
  } else {
    char* family = fallbackfamily(codepoint);
    if (family) {
      hmput(fallback_fonts, codepoint, family); // already duplicated inside fallbackfamily
    }
    return family;
  }
}
typedef struct {
  RnTextProps props;
  float occupied_w;
} text_props_t;


text_props_t rendertextranged(
  RnState* state, 
  const char* text, 
  RnFont* font, 
  vec2s pos, 
  RnColor color, 
  bool render,
  uint32_t rbegin,
  int32_t rend,
  int32_t rowidx) {
  // Get the harfbuzz text information for the string
  RnHarfbuzzText* hb_text = rn_hb_text_from_str(state, *font, text);

  // Retrieve highest bearing if 
  hb_text->highest_bearing = font->size; 
  vec2s start_pos = (vec2s){.x = pos.x, .y = pos.y};

  float scale = 1.0f;
  float w = 0.0f;
  float max_top = 0, min_bottom = 0;
  int32_t charx = 0;
  if (font->selected_strike_size)
    scale = ((float)font->size / (float)font->selected_strike_size);
  for (uint32_t i = rbegin; i < (rend == -1 ? hb_text->glyph_count : 
  (uint32_t)rend); i++) {
    // Get the glyph from the glyph index
    // If the glyph is not within the font, dont render it
    if(!hb_text->glyph_info[i].codepoint) {
      hb_text->glyph_info[i].codepoint = ' ';
    }
    RnGlyph glyph =  rn_glyph_from_codepoint(
      state, font,
      hb_text->glyph_info[i].codepoint); 

    uint32_t text_length = strlen(text);
    uint32_t codepoint = rn_utf8_to_codepoint(text, hb_text->glyph_info[i].cluster, text_length);

    // Advance the x position by the tab width if 
    // we iterate a tab character
    if(codepoint == '\t') {
      pos.x += font->tab_w * font->space_w;
      continue;
    }
    float x_advance = (hb_text->glyph_pos[i].x_advance / 64.0f) * scale;
    if(font == s.font.font && s.fontadvance == 0) {
      s.fontadvance = x_advance;
    }
    float x_offset  = (hb_text->glyph_pos[i].x_offset / 64.0f) * scale;

    vec2s glyph_pos = {
      .x = pos.x + x_offset,
      .y = pos.y + hb_text->highest_bearing  
    };
    float offset = (pos.y + (hb_text->highest_bearing - glyph.bearing_y)) - pos.y;
    charx++;
        FT_Face face = s.font.font->face;
        float line_height = face->size->metrics.height / 64.0f;
        float cw = face->size->metrics.max_advance / 64.0f;
    if(render) {
      bool oncursor = charx == s.cursor.x && rowidx == s.cursor.y;

      if(oncursor && !lf_flag_exists(&s.termmode, TERM_MODE_HIDE_CURSOR)) {
        s.last_cursor_row = rowidx;
        float glyphw = glyph.width == 0 ? cw : glyph.width;
        if(s.cells[rowidx * s.cols + charx + 1].codepoint == ' ') {
          glyphw = cw;
        }
        rn_rect_render(
          state, 
          (vec2s){
            .x = glyph_pos.x + glyph.bearing_x + cw, 
            .y = rowidx * s.font.font->line_h }, 
          (vec2s)
            {
              .x = glyphw, .y = line_height },
          getrendercolor(s.cells[rowidx * s.cols + charx].attr.fg, 
                         s.cells[rowidx * s.cols + charx].attr,
                         true
                         ));

      }
      term_color_16_t charclr = s.cells[rowidx * s.cols + charx - 1].attr.fg;
      if(charx == s.cursor.x + 1&& rowidx == s.cursor.y &&  !lf_flag_exists(&s.termmode, TERM_MODE_HIDE_CURSOR)) {
        charclr = s.cells[rowidx * s.cols + charx].attr.bg;
      }

      const term_attr_t* cellattr = &s.cells[rowidx * s.cols + charx - 1].attr;

      bool has_bg = (cellattr->bg_r != -1) || (cellattr->bg != CLR_BLACK);

      if (has_bg) {
        rn_rect_render(state, 
                       (vec2s){.x = (charx - 1) * cw, .y = rowidx * s.font.font->line_h},
                       (vec2s){.x = cw, .y = s.font.font->line_h},
                       getrendercolor(cellattr->bg,
                                      *cellattr,
                                      false));
      }
      rn_glyph_render(state, glyph, *font, glyph_pos, 
                      getrendercolor(charclr, 
                                     s.cells[rowidx * s.cols + charx - 1].attr,
                                     true
                                     ));
    }

    if (glyph.glyph_top + offset > max_top) {
      max_top = glyph.glyph_top + offset;
    }
    if (glyph.glyph_bottom < min_bottom) {
      min_bottom = glyph.glyph_bottom;
    }

    // Advance to the next glyph
    pos.x += (font->selected_strike_size != 0 ?  cw / 2 : cw); 

    w += s.fontadvance;
  }

  return (text_props_t){
    .occupied_w = w,
    .props = (RnTextProps){
      .width = pos.x - start_pos.x, 
      .height = max_top - min_bottom,
      .paragraph_pos = pos
    }
  };
}

static int nrenders = 0;

bool containsnonascii(const char *str) {
  const unsigned char *p = (const unsigned char *)str;
  while (*p) {
    if (*p > 127) return true;
    p++;
  }
  return false;
}

void rendertextui(
  lf_ui_state_t* ui,
  const char* text, 
  lf_mapped_font_t mapped_font, 
  vec2s pos, 
  RnColor color, 
  bool render,
  uint32_t rowidx
) {
  if (!mapped_font.font) {
    fprintf(stderr, "tyr: trying to render with unregistered font.\n");
    return;
  }

  RnHarfbuzzText* hb_text = rn_hb_text_from_str(ui->render_state, *mapped_font.font, text);

  rendering_range_t rendering_ranges[hb_text->glyph_count];
  memset(rendering_ranges, 0, sizeof(rendering_ranges));

  uint32_t nranges = 0;
  uint32_t iranges = 0;

  rendering_ranges[nranges].font = mapped_font;
  rendering_ranges[nranges].begin = 0;
  rendering_ranges[nranges].end = 0;
  rendering_ranges[nranges].hb_text = hb_text;
  nranges++;

  uint32_t text_length = strlen(text);

  for (unsigned int i = 0; i < hb_text->glyph_count; i++) {
    hb_glyph_info_t inf = hb_text->glyph_info[i];
    uint32_t unicode_codepoint = rn_utf8_to_codepoint(text, inf.cluster, text_length);
    lf_mapped_font_t current_font = rendering_ranges[iranges].font;
    lf_mapped_font_t next_font = current_font;

    if (FT_Get_Char_Index(current_font.font->face, unicode_codepoint) == 0
      || (FT_Get_Char_Index(mapped_font.font->face, unicode_codepoint) != 0 &&
      current_font.font != mapped_font.font)) {
      // current font cannot render this codepoint
      if (FT_Get_Char_Index(mapped_font.font->face, unicode_codepoint) != 0) {
        // mapped font can render -> switch back
        next_font = mapped_font;
      } else {
        // mapped font also cannot render: find fallback
        const char* fallback_family = getfallbackfamily(unicode_codepoint);
        if (fallback_family) {
          lf_mapped_font_t fallback_font = lf_asset_manager_request_font(
            ui, fallback_family, mapped_font.style.style, 
            mapped_font.pixel_size);

          if (fallback_font.font) {
            next_font = fallback_font;
          } else {
            fprintf(stderr, "tyr: failed to load fallback font for unicode %u.\n", unicode_codepoint);
          }
        }
      }

      // only switch font if needed
      if (next_font.font != current_font.font) {
        iranges++;
        rendering_ranges[iranges].begin = i;
        rendering_ranges[iranges].font = next_font;
        rendering_ranges[iranges].hb_text = hb_text;
        nranges++;
      }
    }
    rendering_ranges[iranges].end = i + 1;
  }

  float posx = pos.x;
  float spacing = 0;
  for (uint32_t i = 0; i < nranges; i++) {
    rendering_range_t range = rendering_ranges[i];
    if (range.font.font == mapped_font.font) {
      posx += spacing;
      spacing = 0;
    }

    text_props_t props = rendertextranged(
      ui->render_state, text, range.font.font,
      (vec2s){.x = posx, .y = pos.y},
      color, render, range.begin, range.end,
      rowidx
    );

    posx += props.props.width;
    if (range.font.font != mapped_font.font) {
      spacing += props.occupied_w - props.props.width;
    }
  }
}


void 
renderterminalrows(void) {
  float y = 0;
  for (uint32_t i = 0; i < (uint32_t)s.rows; i++) {
    if (s.dirty[i] == 0) {
      y += s.font.font->line_h;
      continue;
    }

    char* row = s.rowsunicode[i]; 
    char* ptr = row;
    for (int32_t j = 0; j < s.cols; j++)
      ptr += utf8encode(s.cells[i * s.cols + j].codepoint, ptr);
    *ptr = '\0';

    rendertextui(s.ui, row, s.font, (vec2s){.x = 0, .y = y}, RN_WHITE, true, i);

    y += s.font.font->line_h;
    s.dirty[i] = 0;
  }
  nrenders = 0;
}


void renderterminalrows_range(uint32_t from, uint32_t to) {
  float y = from * s.font.font->line_h;
  for (uint32_t i = from; i <= to; i++) {

    char* row = s.rowsunicode[i]; 
    char* ptr = row;
    for (int32_t j = 0; j < s.cols; j++)
      ptr += utf8encode(s.cells[i * s.cols + j].codepoint, ptr);
    *ptr = '\0';

    rendertextui(s.ui, row, s.font, (vec2s){.x = 0, .y = y}, RN_WHITE, true, i);

    y += s.font.font->line_h;
  }
}

void 
taskrender(void* data) {
  lf_ui_state_t* ui = ((task_data_t*)data)->ui;
  ui->needs_render = true;
  free(data);
}

void 
enquerender() {
  task_data_t* task_data = malloc(sizeof(task_data_t));
  task_data->ui = s.ui;
  lf_task_enqueue(taskrender, task_data);
}

static RnColor colorfrom256palette(int idx) {
  if(idx < 0 || idx > 255)
    return (RnColor){0, 0, 0, 255}; // fallback

  if(idx < 16) {
    return getrendercolor((term_color_16_t)idx, (term_attr_t){0}, true);
  }

  if(idx >= 16 && idx <= 231) {
    int c = idx - 16;
    int r = (c / 36) % 6;
    int g = (c / 6) % 6;
    int b = c % 6;

    return (RnColor){
      .r = r == 0 ? 0 : 55 + r * 40,
      .g = g == 0 ? 0 : 55 + g * 40,
      .b = b == 0 ? 0 : 55 + b * 40,
      .a = 255
    };
  }

  // grayscale 232–255
  int gray = 8 + (idx - 232) * 10;
  return (RnColor){ gray, gray, gray, 255 };
}

RnColor getrendercolor(term_color_16_t color, term_attr_t attr, bool fg) {
  if(fg && attr.fg_r != -1) {
    return (RnColor){attr.fg_r, attr.fg_g, attr.fg_b, 255};
  } else if(!fg && attr.bg_r != -1) {
    return (RnColor){attr.bg_r, attr.bg_g, attr.bg_b, 255};
  }

  // handle 256-color indices
  if(color >= 16 && color <= 255) {
    return colorfrom256palette(color);
  }

  // fallback to ANSI base
  switch (color) {
    case CLR_BLACK:           return (RnColor){ 0,   0,   0,   255 };
    case CLR_RED:             return (RnColor){ 205, 0,   0,   255 };
    case CLR_GREEN:           return (RnColor){ 0,   205, 0,   255 };
    case CLR_YELLOW:          return (RnColor){ 205, 205, 0,   255 };
    case CLR_BLUE:            return (RnColor){ 0,   0,   238, 255 };
    case CLR_MAGENTA:         return (RnColor){ 205, 0,   205, 255 };
    case CLR_CYAN:            return (RnColor){ 0,   205, 205, 255 };
    case CLR_WHITE:           return (RnColor){ 229, 229, 229, 255 };

    case CLR_BRIGHT_BLACK:    return (RnColor){ 127, 127, 127, 255 };
    case CLR_BRIGHT_RED:      return (RnColor){ 255, 0,   0,   255 };
    case CLR_BRIGHT_GREEN:    return (RnColor){ 0,   255, 0,   255 };
    case CLR_BRIGHT_YELLOW:   return (RnColor){ 255, 255, 0,   255 };
    case CLR_BRIGHT_BLUE:     return (RnColor){ 92,  92,  255, 255 };
    case CLR_BRIGHT_MAGENTA:  return (RnColor){ 255, 0,   255, 255 };
    case CLR_BRIGHT_CYAN:     return (RnColor){ 0,   255, 255, 255 };
    case CLR_BRIGHT_WHITE:    return (RnColor){ 255, 255, 255, 255 };

    default:                  return (RnColor){ 0, 0, 0, 255 };
  }
}
