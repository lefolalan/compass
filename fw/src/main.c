/*
 * Joins the Wi-Fi network, shows a boot screen that gives the address of the
 * board and the state of its battery, then displays every frame a client
 * uploads. Until the first upload, the boot screen follows the address of the
 * board.
 */

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "battery.h"
#include "canvas.h"
#include "panel.h"
#include "picture.h"
#include "upload_server.h"
#include "wifi_link.h"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

#define MARGIN        16
#define HEADER_HEIGHT 64
#define LINE_SPACING  8

/*
 * Joining plus DHCP takes about 5 s. Past this delay the boot screen goes up
 * without an address, so that a network fault does not leave the panel blank.
 */
#define WIFI_ADDRESS_TIMEOUT K_SECONDS(30)

/*
 * The vendor asks for 180 s between two full refreshes. An upload is the call
 * of the user, whereas a redraw of the boot screen can wait.
 */
#define REDRAW_SPACING K_SECONDS(180)

#define WATCHER_STACK_SIZE 2048

#define WIFI_OFFLINE_TEXT "Wi-Fi hors ligne"

#define BATTERY_UNKNOWN_TEXT "Batterie inconnue"

static const struct device *const display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static const struct device *const gauge = DEVICE_DT_GET(DT_ALIAS(fuel_gauge0));

/*
 * The upload callback and the boot screen watcher draw and refresh from two
 * threads, so this lock covers the canvas, the panel and frame_uploaded.
 */
static K_MUTEX_DEFINE(screen_lock);

/* Set by the first upload, after which the boot screen never comes back. */
static bool frame_uploaded;

/*
 * Address on the boot screen, empty when it reads WIFI_OFFLINE_TEXT or when
 * the boot screen never went up. main() owns it until the watcher starts.
 */
static char shown_address[NET_IPV4_ADDR_LEN];

static void draw_centered_text(int y, const struct font *font, enum canvas_color color,
			       const char *text)
{
	const int x = (CANVAS_WIDTH - canvas_text_width(font, text)) / 2;

	canvas_draw_text(x, y, font, color, text);
}

/* battery is NULL when the fuel gauge could not be read. */
static void compose_boot_screen(const char *network_line, const struct battery_reading *battery)
{
	char port_line[sizeof("Port TCP 65535")];
	char charge_line[sizeof("Batterie 255 %")];
	char voltage_line[sizeof("Tension 99,99 V")];
	/* One line would not fit next to the picture, hence two for the battery. */
	const char *lines[6] = {
		"Zephyr RTOS",
		"ESP32-C6 Feather",
		network_line,
		port_line,
	};
	size_t line_count = 4;
	const int body_top = HEADER_HEIGHT;
	const int body_height = CANVAS_HEIGHT - HEADER_HEIGHT;
	const int text_x = MARGIN + picture.width + MARGIN;
	int text_height;
	int y;

	snprintk(port_line, sizeof(port_line), "Port TCP %d", CONFIG_APP_UPLOAD_PORT);

	if (battery != NULL) {
		snprintk(charge_line, sizeof(charge_line), "Batterie %u %%", battery->charge_pct);
		snprintk(voltage_line, sizeof(voltage_line), "Tension %d,%02d V",
			 battery->voltage_mv / 1000, battery->voltage_mv % 1000 / 10);
		lines[line_count++] = charge_line;
		lines[line_count++] = voltage_line;
	} else {
		lines[line_count++] = BATTERY_UNKNOWN_TEXT;
	}

	text_height = line_count * font_body.height + (line_count - 1) * LINE_SPACING;

	canvas_clear(CANVAS_WHITE);

	canvas_fill_rect(0, 0, CANVAS_WIDTH, HEADER_HEIGHT, CANVAS_BLACK);
	draw_centered_text((HEADER_HEIGHT - font_title.height) / 2, &font_title, CANVAS_WHITE,
			   "Bonjour Maïa !");

	canvas_draw_bitmap(MARGIN, body_top + (body_height - picture.height) / 2, &picture);

	y = body_top + (body_height - text_height) / 2;
	for (size_t i = 0; i < line_count; i++) {
		canvas_draw_text(text_x, y, &font_body, CANVAS_BLACK, lines[i]);
		y += font_body.height + LINE_SPACING;
	}
}

static int show_uploaded_frame(const struct canvas_bitmap *frame)
{
	int err;

	k_mutex_lock(&screen_lock, K_FOREVER);

	if (panel_is_busy()) {
		err = -EBUSY;
	} else {
		/* The canvas belongs to the frame from here on, even if the refresh fails. */
		frame_uploaded = true;
		canvas_draw_bitmap(0, 0, frame);

		LOG_INF("Refreshing the panel, this takes a few seconds");

		err = panel_refresh();
	}

	k_mutex_unlock(&screen_lock);

	return err;
}

/*
 * Puts the boot screen up with a fresh battery reading and the given address,
 * empty for none.
 *
 * Returns -ECANCELED once an upload took the screen over, -EBUSY while the
 * panel is held by an earlier call, and the error of the refresh otherwise.
 */
static int show_boot_screen(const char *address)
{
	struct battery_reading battery;
	bool battery_known;
	int err;

	k_mutex_lock(&screen_lock, K_FOREVER);

	if (frame_uploaded) {
		err = -ECANCELED;
	} else if (panel_is_busy()) {
		err = -EBUSY;
	} else {
		battery_known = battery_read(gauge, &battery) == 0;
		if (battery_known) {
			LOG_INF("Battery at %u mV, %u %%", battery.voltage_mv, battery.charge_pct);
		}

		compose_boot_screen(address[0] != '\0' ? address : WIFI_OFFLINE_TEXT,
				    battery_known ? &battery : NULL);

		/*
		 * Holds even when the refresh times out, because the flush that
		 * resumes reads the canvas.
		 */
		snprintk(shown_address, sizeof(shown_address), "%s", address);

		LOG_INF("Refreshing the panel, this takes a few seconds");

		err = panel_refresh();
	}

	k_mutex_unlock(&screen_lock);

	return err;
}

/*
 * Redraws the boot screen when the board receives an address that the screen
 * does not show, which covers a link that comes up after the boot screen and
 * a new DHCP lease after an outage. A lost link alone redraws nothing, since
 * the driver rejoins and a refresh flashes the panel for seconds.
 */
static void keep_boot_screen_current(void *p1, void *p2, void *p3)
{
	char address[NET_IPV4_ADDR_LEN];
	int err;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		/*
		 * This thread starts right after the boot screen refresh, and an
		 * upload ends it, so every refresh before this point is either
		 * that one or its own. The sleep also paces the retries while
		 * the panel is stalled.
		 */
		k_sleep(REDRAW_SPACING);

		err = wifi_link_wait_for_address(shown_address, K_FOREVER, address,
						 sizeof(address));
		if (err < 0) {
			LOG_ERR("Boot screen no longer follows the address (%d)", err);
			return;
		}

		err = show_boot_screen(address);
		if (err == -ECANCELED) {
			return;
		}

		if (err < 0) {
			LOG_WRN("Boot screen not redrawn with %s (%d)", address, err);
		} else {
			LOG_INF("Boot screen redrawn with %s", address);
		}
	}
}

K_THREAD_DEFINE(boot_screen_watcher, WATCHER_STACK_SIZE, keep_boot_screen_current, NULL, NULL,
		NULL, K_PRIO_PREEMPT(1), 0, SYS_FOREVER_MS);

/*
 * Writes the IPv4 address into address, which stays empty when the link did
 * not come up in time. Returns false when no join is under way, in which case
 * no address will ever come.
 */
static bool bring_up_network(char *address, size_t address_size)
{
	int err;

	address[0] = '\0';

	if (strlen(CONFIG_APP_WIFI_SSID) == 0) {
		LOG_ERR("No Wi-Fi credentials in this build. Copy fw/wifi.conf.example to "
			"fw/wifi.conf, fill it in and rebuild");
		return false;
	}

	if (wifi_link_join(CONFIG_APP_WIFI_SSID, CONFIG_APP_WIFI_PSK) < 0) {
		return false;
	}

	err = wifi_link_wait_for_address("", WIFI_ADDRESS_TIMEOUT, address, address_size);
	if (err == -EAGAIN) {
		LOG_WRN("No IPv4 address yet, the boot screen follows once it comes");
	} else if (err < 0) {
		LOG_ERR("Cannot read the IPv4 address (%d)", err);
	}

	return true;
}

int main(void)
{
	char address[NET_IPV4_ADDR_LEN];
	bool joining;
	int err;

	LOG_INF("Initializing the panel");

	err = panel_init(display);
	if (err < 0 && err != -ETIMEDOUT) {
		LOG_ERR("Panel initialization failed (%d)", err);
		return err;
	}

	joining = bring_up_network(address, sizeof(address));

	err = show_boot_screen(address);
	if (err == -EBUSY) {
		LOG_ERR("Boot screen skipped, the panel init is still stalled");
	} else if (err < 0) {
		LOG_ERR("Boot screen not shown (%d)", err);
	}

	if (joining) {
		k_thread_start(boot_screen_watcher);
	}

	/*
	 * A stalled panel must not take the network service down with it, so
	 * the server starts either way and tells its clients about the panel.
	 */
	return upload_server_run(CONFIG_APP_UPLOAD_PORT, show_uploaded_frame);
}
