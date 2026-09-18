/*
 * The ESP32 driver does the heavy lifting. It starts the DHCP client once
 * associated (CONFIG_WIFI_STA_AUTO_DHCPV4) and rejoins after a loss
 * (CONFIG_ESP32_WIFI_STA_RECONNECT), so this module only issues the request
 * and reports what happens.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>

#include "wifi_link.h"

LOG_MODULE_REGISTER(wifi_link, LOG_LEVEL_INF);

/* Wi-Fi and IPv4 events belong to different layers, hence two callbacks. */
static struct net_mgmt_event_callback wifi_events;
static struct net_mgmt_event_callback ipv4_events;

/* Only wakes the waiter up, the interface stays the source of truth. */
static K_SEM_DEFINE(address_added, 0, 1);

static void log_join_failure(enum wifi_conn_status status)
{
	switch (status) {
	case WIFI_STATUS_CONN_WRONG_PASSWORD:
		LOG_WRN("Access point refused the key, check CONFIG_APP_WIFI_PSK in wifi.conf");
		break;
	case WIFI_STATUS_CONN_AP_NOT_FOUND:
		LOG_WRN("Access point not found, check CONFIG_APP_WIFI_SSID in wifi.conf and "
			"the 2.4 GHz coverage");
		break;
	default:
		LOG_WRN("Joining the access point failed (status %d)", status);
		break;
	}
}

static void on_wifi_event(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	const struct wifi_status *status = cb->info;

	ARG_UNUSED(iface);

	switch (event) {
	case NET_EVENT_WIFI_CONNECT_RESULT:
		if (status->conn_status == WIFI_STATUS_CONN_SUCCESS) {
			LOG_INF("Joined the access point");
		} else {
			log_join_failure(status->conn_status);
		}
		break;
	case NET_EVENT_WIFI_DISCONNECT_RESULT:
		LOG_WRN("Access point lost (reason %d), the driver rejoins", status->disconn_reason);
		break;
	default:
		break;
	}
}

static void on_ipv4_event(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	char text[NET_IPV4_ADDR_LEN];
	const struct net_in_addr *address;

	ARG_UNUSED(cb);

	if (event != NET_EVENT_IPV4_ADDR_ADD) {
		return;
	}

	address = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
	if (address != NULL) {
		LOG_INF("IPv4 address %s", net_addr_ntop(NET_AF_INET, address, text, sizeof(text)));
	}

	k_sem_give(&address_added);
}

int wifi_link_join(const char *ssid, const char *psk)
{
	struct net_if *iface = net_if_get_first_wifi();
	struct wifi_connect_req_params params = {
		.ssid = (const uint8_t *)ssid,
		.ssid_length = strlen(ssid),
		.psk = (const uint8_t *)psk,
		.psk_length = strlen(psk),
		/* A threshold for the ESP32 driver, WPA3 access points pass too. */
		.security = WIFI_SECURITY_TYPE_PSK,
		.channel = WIFI_CHANNEL_ANY,
		.band = WIFI_FREQ_BAND_2_4_GHZ,
		.mfp = WIFI_MFP_OPTIONAL,
	};
	int err;

	if (iface == NULL) {
		LOG_ERR("No Wi-Fi interface");
		return -ENODEV;
	}

	net_mgmt_init_event_callback(&wifi_events, on_wifi_event,
				     NET_EVENT_WIFI_CONNECT_RESULT |
					     NET_EVENT_WIFI_DISCONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_events);

	net_mgmt_init_event_callback(&ipv4_events, on_ipv4_event, NET_EVENT_IPV4_ADDR_ADD);
	net_mgmt_add_event_callback(&ipv4_events);

	LOG_INF("Joining \"%s\"", ssid);

	err = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
	if (err < 0) {
		LOG_ERR("Wi-Fi connect request failed (%d)", err);
	}

	return err;
}

int wifi_link_wait_for_address(k_timeout_t timeout, char *address, size_t address_size)
{
	const k_timepoint_t deadline = sys_timepoint_calc(timeout);
	struct net_if *iface = net_if_get_first_wifi();

	if (iface == NULL) {
		return -ENODEV;
	}

	for (;;) {
		const struct net_in_addr *current =
			net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);

		if (current != NULL) {
			if (net_addr_ntop(NET_AF_INET, current, address, address_size) == NULL) {
				return -ENOSPC;
			}

			return 0;
		}

		if (k_sem_take(&address_added, sys_timepoint_timeout(deadline)) < 0) {
			return -EAGAIN;
		}
	}
}
