/*
 * Joins the Wi-Fi network, shows a boot screen that gives the address of the
 * board, then displays every frame a client uploads.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "canvas.h"
#include "picture.h"
#include "upload_server.h"
#include "wifi_link.h"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

#define MARGIN        16
#define HEADER_HEIGHT 64
#define LINE_SPACING  8

/* A healthy panel finishes its init or a full refresh in about 5 s. */
#define PANEL_STALL_WARNING_DELAY K_SECONDS(20)

/*
 * Joining plus DHCP takes about 5 s. Past this delay the boot screen goes up
 * without an address, so that a network fault does not leave the panel blank.
 */
#define WIFI_ADDRESS_TIMEOUT K_SECONDS(30)

#define WIFI_OFFLINE_TEXT "Wi-Fi hors ligne"

static const struct device *const display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

static void draw_centered_text(int y, const struct font *font, enum canvas_color color,
			       const char *text)
{
	const int x = (CANVAS_WIDTH - canvas_text_width(font, text)) / 2;

	canvas_draw_text(x, y, font, color, text);
}

static void compose_boot_screen(const char *network_line)
{
	char port_line[sizeof("Port TCP 65535")];
	const char *lines[] = {
		"Zephyr RTOS",
		"ESP32-C6 Feather",
		network_line,
		port_line,
	};
	const int body_top = HEADER_HEIGHT;
	const int body_height = CANVAS_HEIGHT - HEADER_HEIGHT;
	const int text_height = ARRAY_SIZE(lines) * font_body.height +
				(ARRAY_SIZE(lines) - 1) * LINE_SPACING;
	const int text_x = MARGIN + picture.width + MARGIN;
	int y;

	snprintk(port_line, sizeof(port_line), "Port TCP %d", CONFIG_APP_UPLOAD_PORT);

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
 * stalls the caller forever. Every blocking display call goes through here, so
 * that the stall at least shows up on the console.
 */
static int run_with_stall_warning(int (*panel_call)(const struct device *panel))
{
	int err;

	k_work_schedule(&panel_stall_warning_work, PANEL_STALL_WARNING_DELAY);
	err = panel_call(display);
	k_work_cancel_delayable(&panel_stall_warning_work);

	return err;
}

static int refresh_panel(void)
{
	int err;

	LOG_INF("Refreshing the panel, this takes a few seconds");

	err = run_with_stall_warning(canvas_flush);
	if (err < 0) {
		LOG_ERR("Panel update failed (%d)", err);
	}

	return err;
}

static int show_uploaded_frame(const struct canvas_bitmap *frame)
{
	canvas_draw_bitmap(0, 0, frame);

	return refresh_panel();
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
	int err;

	LOG_INF("Initializing the panel");

	/*
	 * The display node is marked zephyr,deferred-init, which moves the
	 * unbounded wait on BUSY out of the boot sequence, where it would be
	 * silent.
	 */
	err = run_with_stall_warning(device_init);
	if (err < 0) {
		LOG_ERR("Panel initialization failed (%d)", err);
		return err;
	}

	bring_up_network(network_line, sizeof(network_line));

	compose_boot_screen(network_line);
	err = refresh_panel();
	if (err < 0) {
		return err;
	}

	return upload_server_run(CONFIG_APP_UPLOAD_PORT, show_uploaded_frame);
}
