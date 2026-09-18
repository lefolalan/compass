/*
 * State of the LiPo battery, read from the fuel gauge of the board.
 */

#ifndef BATTERY_H_
#define BATTERY_H_

#include <stdint.h>

#include <zephyr/device.h>

struct battery_reading {
	uint16_t voltage_mv;
	uint8_t charge_pct;
};

/*
 * Read the battery through its fuel gauge.
 *
 * Returns -ENODEV when the gauge did not answer during its init, and the
 * error of the driver when a read fails.
 */
int battery_read(const struct device *gauge, struct battery_reading *reading);

#endif /* BATTERY_H_ */
