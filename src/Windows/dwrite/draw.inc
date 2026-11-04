/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/* dwrite.dll: drawing a text layout through the program's IDWriteTextRenderer: glyph runs, underlines, strikethroughs, inline objects and the trimming sign. Included by dwrite.c. */

/* The clusters [first, end) of a line that share one face, size, effect and decorations, from `first` on: returns the end of that run. */
static UINT32 draw_run_end(const Layout *L, const Line *ln, UINT32 first, UINT32 limit) {
  const Cluster *a = &L->clusters[first];
  const CharAttr *aa = &L->attrs[a->pos];
  UINT32 j = first + 1;
  for (; j < limit; ++j) {
    const Cluster *b = &L->clusters[j];
    const CharAttr *ba = &L->attrs[b->pos];
    if ((b->flags & (CL_NOGLYPH | CL_INLINE | CL_NEWLINE)) || b->face != a->face || b->size != a->size || ba->effect != aa->effect || ba->flags != aa->flags ||
        ba->locale != aa->locale)
      break;
  }
  (void)ln;
  return j;
}

static HRESULT WINAPI layout_Draw(IDWriteTextLayout *This, void *context, IDWriteTextRenderer *renderer, FLOAT ox, FLOAT oy) {
  Layout *L = (Layout *)This;
  if (!renderer) return E_INVALIDARG;
  HRESULT hr = layout_ensure(L);
  if (FAILED(hr)) return hr;
  for (UINT32 li = 0; li < L->line_count && SUCCEEDED(hr); ++li) {
    const Line *ln = &L->lines[li];
    const float baseline_y = oy + L->top + ln->y + ln->baseline;
    const UINT32 limit = ln->trim_at < ln->first + ln->count ? ln->trim_at : ln->first + ln->count;
    float shift = 0;      /* justification moves what follows a space */
    for (UINT32 i = ln->first; i < limit && SUCCEEDED(hr);) {
      const Cluster *k = &L->clusters[i];
      if (k->flags & CL_NEWLINE) { ++i; continue; }
      if (k->flags & CL_INLINE) {
        IDWriteInlineObject *o = L->attrs[k->pos].inline_object;
        DWRITE_INLINE_OBJECT_METRICS m;
        memset(&m, 0, sizeof(m));
        o->lpVtbl->GetMetrics(o, &m);
        hr = renderer->lpVtbl->DrawInlineObject(renderer, context, ox + ln->x + k->x + shift, baseline_y - m.baseline, o, FALSE, L->p.reading == DWRITE_READING_DIRECTION_RIGHT_TO_LEFT,
                                                L->attrs[k->pos].effect);
        ++i;
        continue;
      }
      if (k->flags & CL_NOGLYPH) {
        ++i;
        continue;
      }
      const UINT32 end = draw_run_end(L, ln, i, limit);
      const UINT32 count = end - i;
      UINT32 units = 0;
      for (UINT32 j = i; j < end; ++j) units += L->clusters[j].length;
      UINT16 *glyphs = mem_array(count, sizeof(UINT16));
      float *advances = mem_array(count, sizeof(float));
      UINT16 *map = mem_array(units ? units : 1, sizeof(UINT16));
      if (!glyphs || !advances || !map) { mem_free(glyphs); mem_free(advances); mem_free(map); hr = E_OUTOFMEMORY; break; }
      float run_width = 0;
      UINT32 u = 0;
      for (UINT32 j = i; j < end; ++j) {
        const Cluster *c = &L->clusters[j];
        glyphs[j - i] = c->glyph;
        advances[j - i] = c->advance + (((c->flags & CL_SPACE) && !(c->flags & CL_NOBREAK_SPACE)) ? ln->justify : 0);
        run_width += advances[j - i];
        for (UINT32 m = 0; m < c->length; ++m) map[u++] = (UINT16)(j - i);
      }
      const CharAttr *attr = &L->attrs[k->pos];
      IDWriteFontFace *face = NULL;
      hr = face_create(k->face, DWRITE_FONT_SIMULATIONS_NONE, &face);
      if (SUCCEEDED(hr)) {
        const float x = ox + ln->x + k->x + shift;
        DWRITE_GLYPH_RUN run;
        DWRITE_GLYPH_RUN_DESCRIPTION desc;
        memset(&run, 0, sizeof(run));
        run.fontFace = face;
        run.fontEmSize = k->size;
        run.glyphCount = count;
        run.glyphIndices = glyphs;
        run.glyphAdvances = advances;
        run.bidiLevel = L->p.reading == DWRITE_READING_DIRECTION_RIGHT_TO_LEFT ? 1 : 0;
        desc.localeName = layout_locale(L, attr);
        desc.string = L->text + k->pos;
        desc.stringLength = units;
        desc.clusterMap = map;
        desc.textPosition = k->pos;
        hr = renderer->lpVtbl->DrawGlyphRun(renderer, context, x, baseline_y, DWRITE_MEASURING_MODE_NATURAL, &run, &desc, attr->effect);
        DWRITE_FONT_METRICS fm;
        if (SUCCEEDED(hr) && (attr->flags & 3) && face_info(k->face, NULL, &fm)) {
          const float s = k->size / (float)fm.designUnitsPerEm;
          if (attr->flags & 1) {
            DWRITE_UNDERLINE u1;
            memset(&u1, 0, sizeof(u1));
            u1.width = run_width;
            u1.thickness = (float)fm.underlineThickness * s;
            u1.offset = -(float)fm.underlinePosition * s;
            u1.runHeight = k->ascent + k->descent;
            u1.readingDirection = L->p.reading;
            u1.flowDirection = L->p.flow;
            u1.localeName = desc.localeName;
            u1.measuringMode = DWRITE_MEASURING_MODE_NATURAL;
            hr = renderer->lpVtbl->DrawUnderline(renderer, context, x, baseline_y, &u1, attr->effect);
          }
          if (SUCCEEDED(hr) && (attr->flags & 2)) {
            DWRITE_STRIKETHROUGH st;
            memset(&st, 0, sizeof(st));
            st.width = run_width;
            st.thickness = (float)fm.strikethroughThickness * s;
            st.offset = -(float)fm.strikethroughPosition * s;
            st.readingDirection = L->p.reading;
            st.flowDirection = L->p.flow;
            st.localeName = desc.localeName;
            st.measuringMode = DWRITE_MEASURING_MODE_NATURAL;
            hr = renderer->lpVtbl->DrawStrikethrough(renderer, context, x, baseline_y, &st, attr->effect);
          }
        }
        release(face);
      }
      mem_free(glyphs);
      mem_free(advances);
      mem_free(map);
      for (UINT32 j = i; j < end; ++j)
        if ((L->clusters[j].flags & CL_SPACE) && !(L->clusters[j].flags & CL_NOBREAK_SPACE)) shift += ln->justify;
      i = end;
    }
    if (SUCCEEDED(hr) && ln->trimmed && L->sign) {
      DWRITE_INLINE_OBJECT_METRICS m;
      memset(&m, 0, sizeof(m));
      L->sign->lpVtbl->GetMetrics(L->sign, &m);
      hr = L->sign->lpVtbl->Draw(L->sign, context, renderer, ox + ln->x + ln->trim_x, baseline_y - m.baseline, FALSE, FALSE, NULL);
    }
  }
  return hr;
}
