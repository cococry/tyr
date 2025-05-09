#include <freetype/freetype.h>
#include <leif/asset_manager.h>
#include <leif/task.h>
#include <pthread.h>
#include <runara/runara.h>
#include <leif/leif.h>
#include <stdatomic.h>
#include <string.h>

#include "tyr.h"
#include "term.h"

#define STB_DS_IMPLEMENTATION
#include "../vendor/stb_ds.h"

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
  int32_t rend) {
  // Get the harfbuzz text information for the string
  RnHarfbuzzText* hb_text = rn_hb_text_from_str(state, *font, text);

  // Retrieve highest bearing if 
  hb_text->highest_bearing = font->size; 
  vec2s start_pos = (vec2s){.x = pos.x, .y = pos.y};

  float scale = 1.0f;
  float w = 0.0f;
  float max_top = 0, min_bottom = 0;
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
    if(render) {
      rn_glyph_render(state, glyph, *font, glyph_pos, color);
    }

    if (glyph.glyph_top + offset > max_top) {
      max_top = glyph.glyph_top + offset;
    }
    if (glyph.glyph_bottom < min_bottom) {
      min_bottom = glyph.glyph_bottom;
    }

    // Advance to the next glyph
    pos.x += (font->selected_strike_size != 0 ?  x_advance / 2 : x_advance); 

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

float rendertextui(
  lf_ui_state_t* ui,
  const char* text, 
  lf_mapped_font_t mapped_font, 
  vec2s pos, 
  RnColor color, 
  bool render
) {
  if (!mapped_font.font) {
    fprintf(stderr, "tyr: trying to render with unregistered font.\n");
    return 0.0f;
  }
  nrenders++;

  // we assume that every main terminal font contains all 
  // ASCII characters. To save performace and not bother with 
  // fallback fonts.
  if(!containsnonascii(text)) {
    text_props_t props = rendertextranged(
      ui->render_state, text, mapped_font.font,
      (vec2s){.x = pos.x, .y = pos.y},
      color, render, 0,-1 
    );
    return props.props.height;
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

    uint32_t charidx = FT_Get_Char_Index(current_font.font->face, unicode_codepoint);
    if (charidx == 0
      || (charidx != 0 &&
      current_font.font != mapped_font.font)) {
      // current font cannot render this codepoint
      if (charidx != 0) {
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
  float height = 0.0f;
  for (uint32_t i = 0; i < nranges; i++) {
    rendering_range_t range = rendering_ranges[i];
    if (range.font.font == mapped_font.font) {
      posx += spacing;
      spacing = 0;
    }

    text_props_t props = rendertextranged(
      ui->render_state, text, range.font.font,
      (vec2s){.x = posx, .y = pos.y},
      color, render, range.begin, range.end
    );

    if(props.props.height > height) height = props.props.height;

    posx += props.props.width;
    if (range.font.font != mapped_font.font) {
      spacing += props.occupied_w - props.props.width;
    }
  }
  return height; 
}


void renderterminalrows(void) {
  uint32_t start = s.smallestdirty;
  uint32_t end   = s.largestdirty;

  float y = start * s.font.font->line_h;

  for (uint32_t i = start; i <= end; ++i) {
    char* row = malloc((s.cols * 4) + 1);
    char* ptr = row;

    for (uint32_t j = 0; j < (uint32_t)s.cols; j++) {
      uint32_t cp = s.cells[i * s.cols + j].codepoint;
      ptr += utf8encode(cp, ptr);
    }
    *ptr = '\0';

    rendertextui(
      s.ui, 
      row,
      s.font,
      (vec2s){.x = 0, .y = y},
      RN_WHITE, true);

    y += s.font.font->line_h;
    free(row);
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
