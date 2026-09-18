/*
 * Station mode Wi-Fi link to one access point, with a DHCP address.
 */

#ifndef WIFI_LINK_H_
#define WIFI_LINK_H_

#include <stddef.h>

#include <zephyr/kernel.h>

/*
 * Start joining the access point and return without waiting for the result.
 * The driver retries on its own and rejoins after a loss, so one call lasts
 * for the whole uptime.
 */
int wifi_link_join(const char *ssid, const char *psk);

/*
 * Wait for an IPv4 address whose text differs from known and write it into
 * address. An empty known accepts any address, and a caller that passes the
 * address it already has sleeps until DHCP hands out another one.
 *
 * Returns -EAGAIN when the timeout expires first. The link may still come up
 * later, because the driver keeps trying. Returns -ENOSPC when address cannot
 * hold the text, which takes NET_IPV4_ADDR_LEN bytes. One thread at a time
 * may wait.
 */
int wifi_link_wait_for_address(const char *known, k_timeout_t timeout, char *address,
			       size_t address_size);

#endif /* WIFI_LINK_H_ */
