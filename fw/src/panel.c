/*
 * One worker thread runs one display call at a time. The caller hands it over
 * and waits on a semaphore with a timeout, which is the only way to bound a
 * driver call that cannot be interrupted.
 */

#include <errno.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "canvas.h"
#include "panel.h"

LOG_MODULE_REGISTER(panel, LOG_LEVEL_INF);

/*
 * A healthy panel finishes its init in 0.1 s and a full refresh in 5 s. The
 * slowest refresh that still completed on the hardware took 13 s.
 */
#define PANEL_CALL_TIMEOUT_S 20

#define WORKER_STACK_SIZE 2048

/* Far longer than the time an internal pull needs to move an undriven wire. */
#define PULL_SETTLE_US 100

/* Owned by the display driver. Only observed here, to qualify a stall. */
static const struct gpio_dt_spec busy_pin = GPIO_DT_SPEC_GET(CANVAS_DISPLAY_NODE, busy_gpios);

static const struct device *panel;
static bool panel_ready;

static int (*job)(const struct device *display);
static int job_result;
static atomic_t job_running;
/* Set when the caller stopped waiting, so that the worker reports the end. */
static atomic_t job_abandoned;
static K_SEM_DEFINE(job_start, 0, 1);
static K_SEM_DEFINE(job_done, 0, 1);

static void run_jobs(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		k_sem_take(&job_start, K_FOREVER);

		job_result = job(panel);

		if (atomic_get(&job_abandoned) != 0) {
			LOG_WRN("The stalled panel call ended (%d), the panel accepts calls again",
				job_result);
		}

		atomic_clear(&job_running);
		k_sem_give(&job_done);
	}
}

K_THREAD_DEFINE(panel_worker, WORKER_STACK_SIZE, run_jobs, NULL, NULL, NULL, K_PRIO_PREEMPT(0), 0,
		0);

/*
 * A line that follows the weak internal pulls is driven by no one, which means
 * a broken BUSY connection and not a busy panel. Interrupts stay locked so
 * that the driver, which polls this pin, never sees the forced level, and the
 * opposite pull then puts the line back to the busy level it was found at.
 *
 * Returns 1 when the line floats, 0 when something drives it, or a negative
 * errno when the pin could not be probed.
 */
static int busy_line_floats(void)
{
	const bool active_low = (busy_pin.dt_flags & GPIO_ACTIVE_LOW) != 0;
	const gpio_flags_t release_pull = active_low ? GPIO_PULL_UP : GPIO_PULL_DOWN;
	const gpio_flags_t restore_pull = active_low ? GPIO_PULL_DOWN : GPIO_PULL_UP;
	const unsigned int key = irq_lock();
	int released = 0;
	int err;
	int restore_err;

	err = gpio_pin_configure_dt(&busy_pin, GPIO_INPUT | release_pull);
	if (err == 0) {
		k_busy_wait(PULL_SETTLE_US);
		released = gpio_pin_get_dt(&busy_pin);

		err = gpio_pin_configure_dt(&busy_pin, GPIO_INPUT | restore_pull);
		k_busy_wait(PULL_SETTLE_US);
	}

	/* Attempted in every case, the driver needs its plain input back. */
	restore_err = gpio_pin_configure_dt(&busy_pin, GPIO_INPUT);

	irq_unlock(key);

	if (err == 0) {
		err = restore_err;
	}
	if (err < 0) {
		return err;
	}
	if (released < 0) {
		return released;
	}

	return released == 0;
}

static void report_stall(const char *call_name)
{
	const int busy = gpio_pin_get_dt(&busy_pin);
	int floats;

	if (busy < 0) {
		LOG_ERR("Panel %s not finished after %d s, and the BUSY pin cannot be read (%d)",
			call_name, PANEL_CALL_TIMEOUT_S, busy);
		return;
	}

	if (busy == 0) {
		LOG_ERR("Panel %s not finished after %d s although BUSY is released, so the call "
			"is stuck on the SPI side and not in the panel",
			call_name, PANEL_CALL_TIMEOUT_S);
		return;
	}

	floats = busy_line_floats();
	if (floats < 0) {
		LOG_ERR("Panel %s not finished after %d s with BUSY active, and the line could "
			"not be probed (%d)",
			call_name, PANEL_CALL_TIMEOUT_S, floats);
	} else if (floats > 0) {
		LOG_ERR("Panel %s not finished after %d s because the BUSY line floats. It "
			"follows the internal pull resistors, so nothing drives it. Check the "
			"BUSY wire and its contacts between the module and the board",
			call_name, PANEL_CALL_TIMEOUT_S);
	} else {
		LOG_ERR("Panel %s not finished after %d s and the panel really drives BUSY. "
			"Check the panel supply and flat cable, the RST wiring, and that the "
			"PANEL build option matches the panel revision",
			call_name, PANEL_CALL_TIMEOUT_S);
	}
}

static int run_bounded(int (*call)(const struct device *display), const char *call_name)
{
	if (!atomic_cas(&job_running, 0, 1)) {
		return -EBUSY;
	}

	atomic_clear(&job_abandoned);
	k_sem_reset(&job_done);
	job = call;
	k_sem_give(&job_start);

	if (k_sem_take(&job_done, K_SECONDS(PANEL_CALL_TIMEOUT_S)) < 0) {
		atomic_set(&job_abandoned, 1);
		report_stall(call_name);
		return -ETIMEDOUT;
	}

	return job_result;
}

static int init_display(const struct device *display)
{
	const int err = device_init(display);

	panel_ready = (err == 0);

	return err;
}

int panel_init(const struct device *display)
{
	panel = display;

	return run_bounded(init_display, "init");
}

int panel_refresh(void)
{
	if (panel_is_busy()) {
		return -EBUSY;
	}

	/* The display API must not run on a device whose init failed. */
	if (!panel_ready) {
		return -ENODEV;
	}

	return run_bounded(canvas_flush, "refresh");
}

bool panel_is_busy(void)
{
	return atomic_get(&job_running) != 0;
}
