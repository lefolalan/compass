/*
 * 1 bpp landscape frame buffer for the e-paper panel.
 *
 * Draw into the canvas, then push it to the panel with canvas_flush(). A
 * flush costs one full refresh of several seconds, so compose the whole
 * screen before flushing.
 */

#ifndef CANVAS_H_
#define CANVAS_H_

#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include "font.h"

#define CANVAS_DISPLAY_NODE DT_CHOSEN(zephyr_display)

/* Landscape resolution, 400x300 on both panel revisions. */
#if DT_NODE_HAS_COMPAT(CANVAS_DISPLAY_NODE, solomon_ssd1683)
/*
 * The SSD16xx binding names the gate lines "width" and the source lines
 * "height", hence the swap.
 */
#define CANVAS_WIDTH  DT_PROP(CANVAS_DISPLAY_NODE, height)
#define CANVAS_HEIGHT DT_PROP(CANVAS_DISPLAY_NODE, width)
#else
#define CANVAS_WIDTH  DT_PROP(CANVAS_DISPLAY_NODE, width)
#define CANVAS_HEIGHT DT_PROP(CANVAS_DISPLAY_NODE, height)
#endif

enum canvas_color {
	CANVAS_BLACK,
	CANVAS_WHITE,
};

struct canvas_bitmap {
	uint16_t width;
	uint16_t height;
	/* 1 bpp, MSB first, rows padded to a byte, a set bit is black. */
	const uint8_t *data;
};

void canvas_clear(enum canvas_color color);

void canvas_set_pixel(int x, int y, enum canvas_color color);

void canvas_fill_rect(int x, int y, int width, int height, enum canvas_color color);

/* Opaque blit, clear bits of the bitmap are drawn white. */
void canvas_draw_bitmap(int x, int y, const struct canvas_bitmap *bitmap);

/*
 * Draw UTF-8 text with (x, y) as the top left corner of the first glyph.
 * Characters missing from the font show as '?'.
 *
 * Returns the x coordinate that follows the last glyph.
 */
int canvas_draw_text(int x, int y, const struct font *font, enum canvas_color color,
		     const char *text);

int canvas_text_width(const struct font *font, const char *text);

/* Send the canvas to the panel and start one full refresh. */
int canvas_flush(const struct device *display);

#endif /* CANVAS_H_ */
