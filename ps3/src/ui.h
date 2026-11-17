// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Software 2D drawing for the menu (platform independent).
// Pixels are 0x00RRGGBB (X8R8G8B8), the RSX framebuffer format.
#ifndef CHIAKI_PS3_UI_H
#define CHIAKI_PS3_UI_H

#include "font.h"

#include <stdbool.h>
#include <stdint.h>

#define UI_W 1280
#define UI_H 720

typedef struct
{
	uint32_t *px;
	int w, h;
} Canvas;

typedef enum
{
	UI_ICON_CROSS,
	UI_ICON_CIRCLE,
	UI_ICON_TRIANGLE,
	UI_ICON_SQUARE,
	UI_ICON_START,
	UI_ICON_SELECT,
	UI_ICON_UPDOWN,
	UI_ICON_LEFTRIGHT
} UiIcon;

void ui_fill(Canvas *c, int x, int y, int w, int h, uint32_t rgb);
void ui_blend(Canvas *c, int x, int y, int w, int h, uint32_t rgb, int alpha);
void ui_vgradient(Canvas *c, int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void ui_round_rect(Canvas *c, int x, int y, int w, int h, int r, uint32_t rgb, int alpha);
// Soft horizontal ribbon following y = base + amp*sin(x*freq + phase), thickness px, peak alpha.
void ui_wave(Canvas *c, int base, int amp, float freq, float phase, int thickness, uint32_t rgb, int alpha);
void ui_disc(Canvas *c, int cx, int cy, int r, uint32_t rgb, int alpha);

// y is the top of the text line. Returns the width drawn.
int ui_text(Canvas *c, const Font *f, int x, int y, const char *s, uint32_t rgb);
int ui_text_width(const Font *f, const char *s);
// Truncates with "..." to fit max_w.
int ui_text_fit(Canvas *c, const Font *f, int x, int y, const char *s, uint32_t rgb, int max_w);

// Button glyph centred in a box of the given size at (x, y) top-left; returns its width.
int ui_icon(Canvas *c, int x, int y, int size, UiIcon icon);
// Icon followed by a label; returns the total width.
int ui_hint(Canvas *c, int x, int y, UiIcon icon, const char *label);

#endif
