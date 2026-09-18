/*
 * Bounded access to the e-paper panel.
 *
 * Neither e-paper driver has a timeout on BUSY, so a panel fault blocks the
 * calling thread forever. This module runs the blocking display calls on a
 * worker thread and makes its caller wait for a bounded time only.
 *
 * Both calls return -ETIMEDOUT when the panel does not finish in time. The
 * call then keeps running on the worker, and every call returns -EBUSY until
 * it ends, which it does on its own if the panel recovers.
 */

#ifndef PANEL_H_
#define PANEL_H_

#include <stdbool.h>

#include <zephyr/device.h>

/*
 * Initialize the display device, whose node carries zephyr,deferred-init so
 * that nothing waits on the panel before the console is up.
 */
int panel_init(const struct device *display);

/* Send the canvas to the panel and run one full refresh. */
int panel_refresh(void);

/*
 * True while a call that timed out is still running. It may read the canvas
 * when it resumes, so do not draw while this holds.
 */
bool panel_is_busy(void);

#endif /* PANEL_H_ */
