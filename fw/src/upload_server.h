/*
 * TCP server of the upload protocol defined in proto/epaper.proto.
 */

#ifndef UPLOAD_SERVER_H_
#define UPLOAD_SERVER_H_

#include <stdint.h>

#include "canvas.h"

/*
 * Receives a full screen frame that already passed validation. Returns 0 once
 * the frame is on the panel, or a negative errno.
 */
typedef int (*upload_show_frame_t)(const struct canvas_bitmap *frame);

/*
 * Serve clients one at a time, forever. Only returns, with a negative errno,
 * when the listening socket cannot be set up.
 */
int upload_server_run(uint16_t port, upload_show_frame_t show_frame);

#endif /* UPLOAD_SERVER_H_ */
