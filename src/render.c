#include <runara/runara.h>
#include <leif/leif.h>

#include "tyr.h"
#include "term.h"

typedef struct {
  uint32_t begin, end;
  lf_mapped_font_t font;
} rendering_range_t;

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

RnTextProps rendertextranged(
  RnState* state, 
  const char* text, 
  RnFont* font, 
  vec2s pos, 
  RnColor color, 
  bool render,
  uint32_t rbegin,
  uint32_t rend) {
  // Get the harfbuzz text information for the string
  RnHarfbuzzText* hb_text = rn_hb_text_from_str(state, *font, text);

  // Retrieve highest bearing if 
  hb_text->highest_bearing = font->size; 
  vec2s start_pos = (vec2s){.x = pos.x, .y = pos.y};

  // New line characters
  const int32_t line_feed       = 0x000A;
  const int32_t carriage_return = 0x000D;
  const int32_t line_seperator  = 0x2028;
  const int32_t paragraph_seperator = 0x2029;

  float textheight = 0;

  float scale = 1.0f;
  float w =0.0f;
  if (font->selected_strike_size)
    scale = ((float)font->size / (float)font->selected_strike_size);
  for (unsigned int i = rbegin; i < rend; i++) {
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
    // Check if the unicode codepoint is a new line and advance 
    // to the next line if so
    if(codepoint == line_feed || codepoint == carriage_return ||
      codepoint == line_seperator || codepoint == paragraph_seperator) {
      float font_height = font->face->size->metrics.height / 64.0f;
      pos.x = start_pos.x;
      pos.y += font_height;
      textheight +=  font_height;
      continue;
    }

    // Advance the x position by the tab width if 
    // we iterate a tab character
    if(codepoint == '\t') {
      pos.x += font->tab_w * font->space_w;
      continue;
    }
    float x_advance = (hb_text->glyph_pos[i].x_advance / 64.0f) * scale;
    float y_advance = (hb_text->glyph_pos[i].y_advance / 64.0f) * scale;
    float x_offset  = (hb_text->glyph_pos[i].x_offset / 64.0f) * scale;
    float y_offset  = (hb_text->glyph_pos[i].y_offset / 64.0f) * scale;

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
    pos.x += (font->selected_strike_size != 0 ?  x_advance / 2 : x_advance); 
    pos.y += y_advance;

    w += 16.796875;
  }

  return (RnTextProps){
    .width = w, 
    .height = textheight,
    .paragraph_pos = pos
  };
}

bool rn_font_has_codepoint(RnFont* font, uint32_t codepoint) {
  if (!font || !font->face) {
    return false;
  }
  
  FT_UInt glyph_index = FT_Get_Char_Index(font->face, codepoint);

  return glyph_index != 0;
}

uint32_t rn_utf8_to_codepoint_and_advance(const char* str, uint32_t max_len, uint32_t* out_codepoint) {
    if (max_len == 0 || str == NULL) {
        if (out_codepoint) *out_codepoint = 0xFFFD;
        return 0;
    }

    const uint8_t* s = (const uint8_t*)str;
    uint8_t byte1 = s[0];

    if (byte1 < 0x80) {
        if (out_codepoint) *out_codepoint = byte1;
        return 1;
    } else if ((byte1 & 0xE0) == 0xC0 && max_len >= 2) {
        uint8_t byte2 = s[1];
        if ((byte2 & 0xC0) != 0x80) goto invalid;
        uint32_t cp = ((byte1 & 0x1F) << 6) | (byte2 & 0x3F);
        if (cp < 0x80) goto invalid;
        if (out_codepoint) *out_codepoint = cp;
        return 2;
    } else if ((byte1 & 0xF0) == 0xE0 && max_len >= 3) {
        uint8_t byte2 = s[1];
        uint8_t byte3 = s[2];
        if ((byte2 & 0xC0) != 0x80 || (byte3 & 0xC0) != 0x80) goto invalid;
        uint32_t cp = ((byte1 & 0x0F) << 12) | ((byte2 & 0x3F) << 6) | (byte3 & 0x3F);
        if (cp < 0x800) goto invalid;
        if (cp >= 0xD800 && cp <= 0xDFFF) goto invalid; // Surrogates
        if (out_codepoint) *out_codepoint = cp;
        return 3;
    } else if ((byte1 & 0xF8) == 0xF0 && max_len >= 4) {
        uint8_t byte2 = s[1];
        uint8_t byte3 = s[2];
        uint8_t byte4 = s[3];
        if ((byte2 & 0xC0) != 0x80 || (byte3 & 0xC0) != 0x80 || (byte4 & 0xC0) != 0x80) goto invalid;
        uint32_t cp = ((byte1 & 0x07) << 18) | ((byte2 & 0x3F) << 12) | ((byte3 & 0x3F) << 6) | (byte4 & 0x3F);
        if (cp < 0x10000 || cp > 0x10FFFF) goto invalid;
        if (out_codepoint) *out_codepoint = cp;
        return 4;
    }

invalid:
    if (out_codepoint) *out_codepoint = 0xFFFD;
    return 1;
}


void rendertextui(
  lf_ui_state_t* ui,
  const char* text, 
  RnFont* font, 
  vec2s pos, 
  RnColor color, 
  bool render
) {
  lf_mapped_font_t mapped_font = {0};
  for(uint32_t i = 0; i < lf_ui_core_get_asset_manager()->fonts.size; i++) {
    lf_mapped_font_t f = lf_ui_core_get_asset_manager()->fonts.items[i];
    if(f.font == font) {
      mapped_font = f;
      break;
    }
  }

  if(!mapped_font.font) {
    fprintf(stderr, "tyr: trying to render with unregistered font.\n");
    return;
  }

  RnHarfbuzzText* hb_text = rn_hb_text_from_str(ui->render_state, *font, text);

  rendering_range_t rendering_ranges[hb_text->glyph_count];
  memset(rendering_ranges, 0, sizeof(rendering_ranges));
  uint32_t nranges = 0;
  uint32_t iranges = 0;

  rendering_ranges[nranges].font = mapped_font;
  rendering_ranges[nranges].begin = 0;
  rendering_ranges[nranges].end = 0;
  nranges++;

  uint32_t text_length = strlen(text);

  for (unsigned int i = 0; i < hb_text->glyph_count; i++) {
    uint32_t unicode_codepoint = rn_utf8_to_codepoint(text, hb_text->glyph_info[i].cluster, text_length);

    // Check if the current font can render this unicode codepoint
    if (!hb_text->glyph_info[i].codepoint) {
      // Try to find fallback font family
      const char* fallback_family = fallbackfamily(unicode_codepoint);
      if (fallback_family) {
        lf_mapped_font_t fallback_font = lf_asset_manager_request_font(ui, fallback_family, mapped_font.style.style, mapped_font.pixel_size);
        if (fallback_font.font) {
          if (rendering_ranges[iranges].font.font != fallback_font.font) {
            iranges++;
            rendering_ranges[iranges].begin = i;
            rendering_ranges[iranges].font = fallback_font;
            nranges++;
          }
        } else {
          fprintf(stderr, "tyr: failed to load fallback font for unicode %u.\n", unicode_codepoint);
        }
        free((void*)fallback_family);
      }
    } else {
      if (rendering_ranges[iranges].font.font != mapped_font.font) {
        iranges++;
        rendering_ranges[iranges].begin = i;
        rendering_ranges[iranges].font = mapped_font;
        nranges++;
      }
    }
    rendering_ranges[iranges].end = i + 1;
  }

  float posx = pos.x; 
  for(uint32_t i = 0; i < nranges; i++) {
    rendering_range_t range = rendering_ranges[i]; 
    float w = rendertextranged(
      ui->render_state, text, range.font.font, 
      (vec2s){.x = posx, .y = pos.y}, 
      color, render, range.begin, range.end).width;
    posx += w;
  }
}

void 
renderterminalrow(lf_ui_state_t* ui, lf_widget_t* widget) {
  lf_text_t* text = (lf_text_t*)widget;
  float y = 0;
  for(uint32_t i = s.head; i < (uint32_t)s.rows + s.head; i++) {
    char* row = getrowutf8(i);
    rendertextui(
      ui, 
      row,
      text->font.font,
      (vec2s){.x = 0, .y = y},
      RN_WHITE, true);
    y += text->font.pixel_size;
    free(row);
  }

}

void 
uiterminal(lf_ui_state_t* ui) {
  lf_text_h2(ui, s.pty->buf)->base.render = renderterminalrow;
}

void 
taskrerender(void* data) {
  if(!data) return;
  task_data_t* task = (task_data_t*)data;
  lf_component_rerender(task->ui, uiterminal);
  free(data);
}
