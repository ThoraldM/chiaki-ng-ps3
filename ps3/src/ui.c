// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include "ui.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline uint32_t mix(uint32_t dst, uint32_t src, int a)
{
	if(a <= 0)
		return dst;
	if(a >= 255)
		return src & 0xffffff;
	uint32_t r = (((src >> 16) & 0xff) * a + ((dst >> 16) & 0xff) * (255 - a)) / 255;
	uint32_t g = (((src >> 8) & 0xff) * a + ((dst >> 8) & 0xff) * (255 - a)) / 255;
	uint32_t b = ((src & 0xff) * a + (dst & 0xff) * (255 - a)) / 255;
	return (r << 16) | (g << 8) | b;
}

static bool clip(Canvas *c, int *x, int *y, int *w, int *h)
{
	if(*x < 0) { *w += *x; *x = 0; }
	if(*y < 0) { *h += *y; *y = 0; }
	if(*x + *w > c->w) *w = c->w - *x;
	if(*y + *h > c->h) *h = c->h - *y;
	return *w > 0 && *h > 0;
}

void ui_fill(Canvas *c, int x, int y, int w, int h, uint32_t rgb)
{
	if(!clip(c, &x, &y, &w, &h))
		return;
	for(int j = 0; j < h; j++)
	{
		uint32_t *p = c->px + (y + j) * c->w + x;
		for(int i = 0; i < w; i++)
			p[i] = rgb;
	}
}

void ui_blend(Canvas *c, int x, int y, int w, int h, uint32_t rgb, int alpha)
{
	if(!clip(c, &x, &y, &w, &h))
		return;
	for(int j = 0; j < h; j++)
	{
		uint32_t *p = c->px + (y + j) * c->w + x;
		for(int i = 0; i < w; i++)
			p[i] = mix(p[i], rgb, alpha);
	}
}

void ui_vgradient(Canvas *c, int x, int y, int w, int h, uint32_t top, uint32_t bottom)
{
	int h0 = h > 1 ? h - 1 : 1;
	int y0 = y;
	if(!clip(c, &x, &y, &w, &h))
		return;
	for(int j = 0; j < h; j++)
	{
		int a = (y + j - y0) * 255 / h0;
		uint32_t col = mix(top, bottom, a);
		uint32_t *p = c->px + (y + j) * c->w + x;
		for(int i = 0; i < w; i++)
			p[i] = col;
	}
}

// coverage (0..255) of a pixel whose centre is at signed distance d from an edge (d < 0 inside)
static inline int coverage(float d)
{
	if(d <= -0.5f)
		return 255;
	if(d >= 0.5f)
		return 0;
	return (int)((0.5f - d) * 255.0f);
}

void ui_round_rect(Canvas *c, int x, int y, int w, int h, int r, uint32_t rgb, int alpha)
{
	if(r * 2 > w) r = w / 2;
	if(r * 2 > h) r = h / 2;
	for(int j = 0; j < h; j++)
	{
		int py = y + j;
		if(py < 0 || py >= c->h)
			continue;
		for(int i = 0; i < w; i++)
		{
			int px = x + i;
			if(px < 0 || px >= c->w)
				continue;
			// distance to the rounded rectangle
			float fx = i + 0.5f, fy = j + 0.5f;
			float qx = fabsf(fx - w / 2.0f) - (w / 2.0f - r);
			float qy = fabsf(fy - h / 2.0f) - (h / 2.0f - r);
			float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
			float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0.0f) - r;
			int a = coverage(d) * alpha / 255;
			uint32_t *p = c->px + py * c->w + px;
			*p = mix(*p, rgb, a);
		}
	}
}

void ui_wave(Canvas *c, int base, int amp, float freq, float phase, int thickness, uint32_t rgb, int alpha)
{
	float half = thickness / 2.0f;
	for(int x = 0; x < c->w; x++)
	{
		float yc = base + amp * sinf(x * freq + phase) + amp * 0.3f * sinf(x * freq * 2.3f + phase * 1.7f);
		int y0 = (int)(yc - half), y1 = (int)(yc + half);
		for(int y = y0; y <= y1; y++)
		{
			if(y < 0 || y >= c->h)
				continue;
			float t = 1.0f - fabsf(y + 0.5f - yc) / half; // 1 in the middle, 0 at the edges
			if(t <= 0)
				continue;
			int a = (int)(alpha * t * t);
			uint32_t *p = c->px + y * c->w + x;
			*p = mix(*p, rgb, a);
		}
	}
}

void ui_disc(Canvas *c, int cx, int cy, int r, uint32_t rgb, int alpha)
{
	ui_round_rect(c, cx - r, cy - r, r * 2, r * 2, r, rgb, alpha);
}

static const FontGlyph *glyph(const Font *f, char ch)
{
	unsigned char u = (unsigned char)ch;
	if(u < f->first || u > f->last)
		u = '?';
	return &f->glyphs[u - f->first];
}

int ui_text_width(const Font *f, const char *s)
{
	int w = 0;
	for(; *s; s++)
		w += glyph(f, *s)->advance;
	return w;
}

int ui_text(Canvas *c, const Font *f, int x, int y, const char *s, uint32_t rgb)
{
	int pen = x;
	for(; *s; s++)
	{
		const FontGlyph *g = glyph(f, *s);
		const uint8_t *a = f->alpha + g->offset;
		for(int j = 0; j < g->h; j++)
		{
			int py = y + g->top + j;
			if(py < 0 || py >= c->h)
				continue;
			for(int i = 0; i < g->w; i++)
			{
				int px = pen + g->left + i;
				if(px < 0 || px >= c->w)
					continue;
				int al = a[j * g->w + i];
				if(al)
				{
					uint32_t *p = c->px + py * c->w + px;
					*p = mix(*p, rgb, al);
				}
			}
		}
		pen += g->advance;
	}
	return pen - x;
}

int ui_text_fit(Canvas *c, const Font *f, int x, int y, const char *s, uint32_t rgb, int max_w)
{
	if(ui_text_width(f, s) <= max_w)
		return ui_text(c, f, x, y, s, rgb);
	char buf[256];
	size_t n = strlen(s);
	if(n >= sizeof(buf) - 4)
		n = sizeof(buf) - 4;
	while(n > 0)
	{
		memcpy(buf, s, n);
		memcpy(buf + n, "...", 4);
		if(ui_text_width(f, buf) <= max_w)
			break;
		n--;
	}
	return ui_text(c, f, x, y, n ? buf : "...", rgb);
}

// --- button glyphs -----------------------------------------------------------

// Anti-aliased thick line segment.
static void line(Canvas *c, float x0, float y0, float x1, float y1, float thick, uint32_t rgb)
{
	int minx = (int)floorf(fminf(x0, x1) - thick), maxx = (int)ceilf(fmaxf(x0, x1) + thick);
	int miny = (int)floorf(fminf(y0, y1) - thick), maxy = (int)ceilf(fmaxf(y0, y1) + thick);
	float dx = x1 - x0, dy = y1 - y0;
	float len2 = dx * dx + dy * dy;
	for(int py = miny; py <= maxy; py++)
	{
		if(py < 0 || py >= c->h)
			continue;
		for(int px = minx; px <= maxx; px++)
		{
			if(px < 0 || px >= c->w)
				continue;
			float fx = px + 0.5f - x0, fy = py + 0.5f - y0;
			float t = len2 > 0 ? (fx * dx + fy * dy) / len2 : 0;
			t = fminf(fmaxf(t, 0.0f), 1.0f);
			float ex = fx - t * dx, ey = fy - t * dy;
			float d = sqrtf(ex * ex + ey * ey) - thick / 2;
			int a = coverage(d);
			if(a)
			{
				uint32_t *p = c->px + py * c->w + px;
				*p = mix(*p, rgb, a);
			}
		}
	}
}

static void ring(Canvas *c, float cx, float cy, float r, float thick, uint32_t rgb)
{
	int x0 = (int)(cx - r - thick), x1 = (int)(cx + r + thick);
	int y0 = (int)(cy - r - thick), y1 = (int)(cy + r + thick);
	for(int py = y0; py <= y1; py++)
	{
		if(py < 0 || py >= c->h)
			continue;
		for(int px = x0; px <= x1; px++)
		{
			if(px < 0 || px >= c->w)
				continue;
			float dx = px + 0.5f - cx, dy = py + 0.5f - cy;
			float d = fabsf(sqrtf(dx * dx + dy * dy) - r) - thick / 2;
			int a = coverage(d);
			if(a)
			{
				uint32_t *p = c->px + py * c->w + px;
				*p = mix(*p, rgb, a);
			}
		}
	}
}

static void tri(Canvas *c, float cx, float cy, float s, float angle_deg, uint32_t rgb, bool filled)
{
	// equilateral triangle pointing towards angle (0 = up)
	float pts[3][2];
	for(int k = 0; k < 3; k++)
	{
		float a = (angle_deg + k * 120.0f) * (float)M_PI / 180.0f;
		pts[k][0] = cx + sinf(a) * s;
		pts[k][1] = cy - cosf(a) * s;
	}
	if(!filled)
	{
		for(int k = 0; k < 3; k++)
			line(c, pts[k][0], pts[k][1], pts[(k + 1) % 3][0], pts[(k + 1) % 3][1], s * 0.28f, rgb);
		return;
	}
	for(float t = 0; t <= 1.0f; t += 0.08f)
	{
		// filled: shrink outline towards the centre
		float k2 = 1.0f - t;
		for(int k = 0; k < 3; k++)
			line(c, cx + (pts[k][0] - cx) * k2, cy + (pts[k][1] - cy) * k2,
					cx + (pts[(k + 1) % 3][0] - cx) * k2, cy + (pts[(k + 1) % 3][1] - cy) * k2, 2.0f, rgb);
	}
}

#define COL_CROSS    0x7fb0f0
#define COL_CIRCLE   0xff6f6f
#define COL_TRIANGLE 0x4fe0b4
#define COL_SQUARE   0xf27fe6
#define COL_ICON_BG  0x101418

int ui_icon(Canvas *c, int x, int y, int size, UiIcon icon)
{
	float cx = x + size / 2.0f, cy = y + size / 2.0f;
	float r = size / 2.0f;
	switch(icon)
	{
		case UI_ICON_START:
		case UI_ICON_SELECT:
		{
			const char *label = icon == UI_ICON_START ? "START" : "SELECT";
			int tw = ui_text_width(&font_small, label);
			int w = tw + 16;
			ui_round_rect(c, x, y + size / 2 - 13, w, 26, 13, 0xe8e8e8, 255);
			ui_text(c, &font_small, x + 8, y + size / 2 - 12, label, 0x202428);
			return w;
		}
		case UI_ICON_UPDOWN:
		case UI_ICON_LEFTRIGHT:
		{
			ui_disc(c, (int)cx, (int)cy, (int)r, 0xe8e8e8, 255);
			float s = r * 0.32f;
			if(icon == UI_ICON_UPDOWN)
			{
				tri(c, cx, cy - r * 0.42f, s, 0, COL_ICON_BG, true);
				tri(c, cx, cy + r * 0.42f, s, 180, COL_ICON_BG, true);
			}
			else
			{
				tri(c, cx - r * 0.42f, cy, s, 270, COL_ICON_BG, true);
				tri(c, cx + r * 0.42f, cy, s, 90, COL_ICON_BG, true);
			}
			return size;
		}
		default:
			break;
	}

	ui_disc(c, (int)cx, (int)cy, (int)r, COL_ICON_BG, 255);
	ring(c, cx, cy, r - 1.0f, 1.5f, 0x505860);
	float k = r * 0.48f;
	float th = size * 0.11f;
	switch(icon)
	{
		case UI_ICON_CROSS:
			line(c, cx - k, cy - k, cx + k, cy + k, th, COL_CROSS);
			line(c, cx - k, cy + k, cx + k, cy - k, th, COL_CROSS);
			break;
		case UI_ICON_CIRCLE:
			ring(c, cx, cy, k * 1.05f, th, COL_CIRCLE);
			break;
		case UI_ICON_TRIANGLE:
			tri(c, cx, cy + k * 0.12f, k * 1.15f, 0, COL_TRIANGLE, false);
			break;
		case UI_ICON_SQUARE:
			line(c, cx - k, cy - k, cx + k, cy - k, th, COL_SQUARE);
			line(c, cx + k, cy - k, cx + k, cy + k, th, COL_SQUARE);
			line(c, cx + k, cy + k, cx - k, cy + k, th, COL_SQUARE);
			line(c, cx - k, cy + k, cx - k, cy - k, th, COL_SQUARE);
			break;
		default:
			break;
	}
	return size;
}

int ui_hint(Canvas *c, int x, int y, UiIcon icon, const char *label)
{
	int w = ui_icon(c, x, y, 32, icon);
	w += 10;
	w += ui_text(c, &font_small, x + w, y + 4, label, 0xe0e4ea);
	return w;
}
