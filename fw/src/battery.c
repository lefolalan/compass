/*
 * The MAX17048 of the Feather sits on the I2C bus behind the power domain of
 * the board, which the board defaults switch on before the bus. The Zephyr
 * driver checks the version register during its init, so a gauge that is not
 * ready never answered.
 */

#include <errno.h>

#include <zephyr/drivers/fuel_gauge.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include "battery.h"

LOG_MODULE_REGISTER(battery, LOG_LEVEL_INF);

int battery_read(const struct device *gauge, struct battery_reading *reading)
{
	const fuel_gauge_prop_t props[] = {
		FUEL_GAUGE_VOLTAGE_UV,
		FUEL_GAUGE_RELATIVE_STATE_OF_CHARGE_PCT,
	};
	union fuel_gauge_prop_val vals[ARRAY_SIZE(props)];
	int err;

	if (!device_is_ready(gauge)) {
		LOG_ERR("The fuel gauge did not answer during its init");
		return -ENODEV;
	}

	err = fuel_gauge_get_props(gauge, props, vals, ARRAY_SIZE(props));
	if (err < 0) {
		LOG_ERR("Cannot read the fuel gauge (%d)", err);
		return err;
	}

	/* The gauge tops out at 5.12 V, so the clamp only guards the narrowing. */
	reading->voltage_mv = CLAMP(vals[0].voltage_uv / 1000, 0, UINT16_MAX);
	reading->charge_pct = vals[1].relative_state_of_charge_pct;

	return 0;
}
