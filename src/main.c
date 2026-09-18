/*
 * MVP for the Waveshare 4.2" e-paper module. Composes one screen with text and
 * a picture, then refreshes the panel once.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "canvas.h"
#include "picture.h"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

#define MARGIN        16
#define HEADER_HEIGHT 64
#define LINE_SPACING  8

/* A healthy panel shows its first screen in about 10 s. */
#define PANEL_STALL_WARNING_DELAY K_SECONDS(20)

static void draw_centered_text(int y, const struct font *font, enum canvas_color color,
			       const char *text)
{
	const int x = (CANVAS_WIDTH - canvas_text_width(font, text)) / 2;

	canvas_draw_text(x, y, font, color, text);
}

static void compose_screen(void)
{
	static const char *const lines[] = {
		"Zephyr RTOS",
		"ESP32-C6 Feather",
		"Écran 4,2 pouces",
		"400 × 300 pixels",
	};
	const int body_top = HEADER_HEIGHT;
	const int body_height = CANVAS_HEIGHT - HEADER_HEIGHT;
	const int text_height = ARRAY_SIZE(lines) * font_body.height +
				(ARRAY_SIZE(lines) - 1) * LINE_SPACING;
	const int text_x = MARGIN + picture.width + MARGIN;
	int y;

	canvas_clear(CANVAS_WHITE);

	canvas_fill_rect(0, 0, CANVAS_WIDTH, HEADER_HEIGHT, CANVAS_BLACK);
	draw_centered_text((HEADER_HEIGHT - font_title.height) / 2, &font_title, CANVAS_WHITE,
			   "Bonjour Maïa !");

	canvas_draw_bitmap(MARGIN, body_top + (body_height - picture.height) / 2, &picture);

	y = body_top + (body_height - text_height) / 2;
	for (size_t i = 0; i < ARRAY_SIZE(lines); i++) {
		canvas_draw_text(text_x, y, &font_body, CANVAS_BLACK, lines[i]);
		y += font_body.height + LINE_SPACING;
	}
}

static void panel_stall_warning(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_WRN("Panel still busy. Check the panel supply and flat cable, the BUSY and RST "
		"wiring, and that the PANEL build option matches the panel revision");
}

static K_WORK_DELAYABLE_DEFINE(panel_stall_warning_work, panel_stall_warning);

/*
 * The e-paper drivers wait on BUSY without a timeout, so a hardware fault
 * stalls the caller forever. The display node is marked zephyr,deferred-init
 * to move that wait out of the boot sequence, where it would be silent.
 */
static int show_screen(const struct device *display)
{
	int err;

	LOG_INF("Initializing the panel");

	err = device_init(display);
	if (err < 0) {
		LOG_ERR("Panel initialization failed (%d)", err);
		return err;
	}

	compose_screen();

	LOG_INF("Refreshing the panel, this takes a few seconds");

	err = canvas_flush(display);
	if (err < 0) {
		LOG_ERR("Panel update failed (%d)", err);
		return err;
	}

	return 0;
}

int main(void)
{
	const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	int err;

	k_work_schedule(&panel_stall_warning_work, PANEL_STALL_WARNING_DELAY);
	err = show_screen(display);
	k_work_cancel_delayable(&panel_stall_warning_work);

	if (err == 0) {
		LOG_INF("Frame sent to the panel");
	}

	return err;
}
