/*
 * The frame buffer uses the RAM layout shared by the UC8176 and the SSD1683, so
 * it goes to the panel as is. It is row-major, MSB first, and a set bit is
 * white.
 */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/drivers/display.h>
#include <zephyr/sys/util.h>

#include "canvas.h"

#define ROW_BYTES (CANVAS_WIDTH / 8)

#define REPLACEMENT_CODEPOINT 0xfffdU
#define FALLBACK_CODEPOINT    '?'

BUILD_ASSERT(CANVAS_WIDTH % 8 == 0, "The display driver only accepts byte-aligned rows");

static uint8_t framebuffer[ROW_BYTES * CANVAS_HEIGHT];

void canvas_clear(enum canvas_color color)
{
	memset(framebuffer, (color == CANVAS_WHITE) ? 0xff : 0x00, sizeof(framebuffer));
}

void canvas_set_pixel(int x, int y, enum canvas_color color)
{
	if (x < 0 || x >= CANVAS_WIDTH || y < 0 || y >= CANVAS_HEIGHT) {
		return;
	}

	if (IS_ENABLED(CONFIG_APP_DISPLAY_FLIP_180)) {
		x = CANVAS_WIDTH - 1 - x;
		y = CANVAS_HEIGHT - 1 - y;
	}

	uint8_t *byte = &framebuffer[y * ROW_BYTES + x / 8];
	const uint8_t mask = 0x80 >> (x % 8);

	if (color == CANVAS_WHITE) {
		*byte |= mask;
	} else {
		*byte &= ~mask;
	}
}

void canvas_fill_rect(int x, int y, int width, int height, enum canvas_color color)
{
	for (int row = 0; row < height; row++) {
		for (int col = 0; col < width; col++) {
			canvas_set_pixel(x + col, y + row, color);
		}
	}
}

static bool bitmap_bit(const uint8_t *data, uint16_t width, int col, int row)
{
	const size_t stride = DIV_ROUND_UP(width, 8);

	return (data[row * stride + col / 8] & (0x80 >> (col % 8))) != 0;
}

void canvas_draw_bitmap(int x, int y, const struct canvas_bitmap *bitmap)
{
	for (int row = 0; row < bitmap->height; row++) {
		for (int col = 0; col < bitmap->width; col++) {
			const bool ink = bitmap_bit(bitmap->data, bitmap->width, col, row);

			canvas_set_pixel(x + col, y + row, ink ? CANVAS_BLACK : CANVAS_WHITE);
		}
	}
}

/*
 * Decode one UTF-8 sequence and advance *text past it. A malformed sequence
 * yields REPLACEMENT_CODEPOINT and never reads past the string terminator.
 */
static uint32_t utf8_next(const char **text)
{
	const uint8_t *s = (const uint8_t *)*text;
	uint32_t codepoint;
	size_t continuation_bytes;

	if (s[0] < 0x80) {
		codepoint = s[0];
		continuation_bytes = 0;
	} else if ((s[0] & 0xe0) == 0xc0) {
		codepoint = s[0] & 0x1f;
		continuation_bytes = 1;
	} else if ((s[0] & 0xf0) == 0xe0) {
		codepoint = s[0] & 0x0f;
		continuation_bytes = 2;
	} else if ((s[0] & 0xf8) == 0xf0) {
		codepoint = s[0] & 0x07;
		continuation_bytes = 3;
	} else {
		*text += 1;
		return REPLACEMENT_CODEPOINT;
	}

	s++;
	for (size_t i = 0; i < continuation_bytes; i++) {
		if ((*s & 0xc0) != 0x80) {
			*text = (const char *)s;
			return REPLACEMENT_CODEPOINT;
		}
		codepoint = (codepoint << 6) | (*s & 0x3f);
		s++;
	}

	*text = (const char *)s;
	return codepoint;
}

static const struct font_glyph *search_glyph(const struct font *font, uint32_t codepoint)
{
	size_t low = 0;
	size_t high = font->glyph_count;

	while (low < high) {
		const size_t mid = low + (high - low) / 2;

		if (font->glyphs[mid].codepoint == codepoint) {
			return &font->glyphs[mid];
		}
		if (font->glyphs[mid].codepoint < codepoint) {
			low = mid + 1;
		} else {
			high = mid;
		}
	}

	return NULL;
}

static const struct font_glyph *find_glyph(const struct font *font, uint32_t codepoint)
{
	const struct font_glyph *glyph = search_glyph(font, codepoint);

	return (glyph != NULL) ? glyph : search_glyph(font, FALLBACK_CODEPOINT);
}

static void draw_glyph(int x, int y, const struct font *font, const struct font_glyph *glyph,
		       enum canvas_color color)
{
	const uint8_t *data = &font->bitmaps[glyph->offset];

	for (int row = 0; row < font->height; row++) {
		for (int col = 0; col < glyph->advance; col++) {
			if (bitmap_bit(data, glyph->advance, col, row)) {
				canvas_set_pixel(x + col, y + row, color);
			}
		}
	}
}

int canvas_draw_text(int x, int y, const struct font *font, enum canvas_color color,
		     const char *text)
{
	while (*text != '\0') {
		const struct font_glyph *glyph = find_glyph(font, utf8_next(&text));

		if (glyph == NULL) {
			continue;
		}

		draw_glyph(x, y, font, glyph, color);
		x += glyph->advance;
	}

	return x;
}

int canvas_text_width(const struct font *font, const char *text)
{
	int width = 0;

	while (*text != '\0') {
		const struct font_glyph *glyph = find_glyph(font, utf8_next(&text));

		if (glyph != NULL) {
			width += glyph->advance;
		}
	}

	return width;
}

int canvas_flush(const struct device *display)
{
	const struct display_buffer_descriptor desc = {
		.buf_size = sizeof(framebuffer),
		.width = CANVAS_WIDTH,
		.height = CANVAS_HEIGHT,
		.pitch = CANVAS_WIDTH,
	};
	int err;

	/*
	 * With blanking on, the driver only fills the controller RAM. Turning
	 * blanking off then starts a single full refresh.
	 */
	err = display_blanking_on(display);
	if (err < 0) {
		return err;
	}

	err = display_write(display, 0, 0, &desc, framebuffer);
	if (err < 0) {
		return err;
	}

	return display_blanking_off(display);
}
