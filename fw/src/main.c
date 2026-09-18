/*
 * Joins the Wi-Fi network, shows a boot screen that gives the address of the
 * board and the state of its battery, then displays every frame a client
 * uploads.
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

#define WIFI_OFFLINE_TEXT "Wi-Fi hors ligne"

#define BATTERY_UNKNOWN_TEXT "Batterie inconnue"

static const struct device *const display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static const struct device *const gauge = DEVICE_DT_GET(DT_ALIAS(fuel_gauge0));

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
	if (panel_is_busy()) {
		return -EBUSY;
	}

	canvas_draw_bitmap(0, 0, frame);

	LOG_INF("Refreshing the panel, this takes a few seconds");

	return panel_refresh();
}

/*
 * Writes the text the boot screen shows for the network, which is the IPv4
 * address when the link came up in time.
 */
static void bring_up_network(char *network_line, size_t network_line_size)
{
	int err;

	snprintk(network_line, network_line_size, "%s", WIFI_OFFLINE_TEXT);

	if (strlen(CONFIG_APP_WIFI_SSID) == 0) {
		LOG_ERR("No Wi-Fi credentials in this build. Copy fw/wifi.conf.example to "
			"fw/wifi.conf, fill it in and rebuild");
		return;
	}

	if (wifi_link_join(CONFIG_APP_WIFI_SSID, CONFIG_APP_WIFI_PSK) < 0) {
		return;
	}

	err = wifi_link_wait_for_address(WIFI_ADDRESS_TIMEOUT, network_line, network_line_size);
	if (err == -EAGAIN) {
		LOG_WRN("No IPv4 address yet, the boot screen goes up without it");
	} else if (err < 0) {
		LOG_ERR("Cannot read the IPv4 address (%d)", err);
	}
}

int main(void)
{
	char network_line[MAX(NET_IPV4_ADDR_LEN, sizeof(WIFI_OFFLINE_TEXT))];
	struct battery_reading battery;
	bool battery_known;
	int err;

	LOG_INF("Initializing the panel");

	err = panel_init(display);
	if (err < 0 && err != -ETIMEDOUT) {
		LOG_ERR("Panel initialization failed (%d)", err);
		return err;
	}

	bring_up_network(network_line, sizeof(network_line));

	battery_known = battery_read(gauge, &battery) == 0;
	if (battery_known) {
		LOG_INF("Battery at %u mV, %u %%", battery.voltage_mv, battery.charge_pct);
	}

	/*
	 * A stalled panel must not take the network service down with it, so
	 * the server starts either way and tells its clients about the panel.
	 */
	if (panel_is_busy()) {
		LOG_ERR("Boot screen skipped, the panel init is still stalled");
	} else {
		compose_boot_screen(network_line, battery_known ? &battery : NULL);

		LOG_INF("Refreshing the panel, this takes a few seconds");

		err = panel_refresh();
		if (err < 0) {
			LOG_ERR("Boot screen not shown (%d)", err);
		}
	}

	return upload_server_run(CONFIG_APP_UPLOAD_PORT, show_uploaded_frame);
}
