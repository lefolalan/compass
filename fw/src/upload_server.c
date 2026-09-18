/*
 * One request and one response per connection, both in the protobuf delimited
 * format (a varint length, then the message). The request is decoded straight
 * from the socket, so no buffer holds the encoded frame.
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include <pb_decode.h>
#include <pb_encode.h>

#include "proto/epaper.pb.h"
#include "upload_server.h"

LOG_MODULE_REGISTER(upload_server, LOG_LEVEL_INF);

/* A client that stalls mid-request must not hold the only server slot. */
#define CLIENT_RECEIVE_TIMEOUT_S 10

#define ACCEPT_RETRY_DELAY K_SECONDS(1)

/* A varint that encodes a 32 bit length. */
#define LENGTH_PREFIX_MAX_BYTES 5

#define FRAME_BYTES (CANVAS_WIDTH / 8 * CANVAS_HEIGHT)

BUILD_ASSERT(sizeof(((epaper_ShowFrame *)0)->pixels.bytes) == FRAME_BYTES,
	     "pixels max_size in proto/epaper.options must hold exactly one canvas frame");

/* Too large for a stack, and the server handles one client at a time. */
static epaper_Request request;

static bool read_socket(pb_istream_t *stream, pb_byte_t *buf, size_t count)
{
	const int sock = POINTER_TO_INT(stream->state);

	while (count > 0) {
		const ssize_t received = zsock_recv(sock, buf, count, 0);

		if (received <= 0) {
			/*
			 * Closed by the peer, timed out or failed. Marking the
			 * stream empty keeps nanopb from draining the rest of
			 * the message, which would wait for a second timeout.
			 */
			stream->bytes_left = 0;
			return false;
		}

		buf += received;
		count -= received;
	}

	return true;
}

static int send_all(int sock, const uint8_t *buf, size_t len)
{
	while (len > 0) {
		const ssize_t sent = zsock_send(sock, buf, len, 0);

		if (sent < 0) {
			return -errno;
		}

		buf += sent;
		len -= sent;
	}

	return 0;
}

static void reply(int client, epaper_Response_Status status, const char *detail)
{
	uint8_t encoded[epaper_Response_size + LENGTH_PREFIX_MAX_BYTES];
	pb_ostream_t stream = pb_ostream_from_buffer(encoded, sizeof(encoded));
	epaper_Response response = epaper_Response_init_zero;
	int err;

	response.status = status;
	snprintk(response.detail, sizeof(response.detail), "%s", detail);

	if (!pb_encode_ex(&stream, epaper_Response_fields, &response, PB_ENCODE_DELIMITED)) {
		LOG_ERR("Response encoding failed (%s)", PB_GET_ERROR(&stream));
		return;
	}

	err = send_all(client, encoded, stream.bytes_written);
	if (err < 0) {
		LOG_WRN("Response not delivered (%d)", err);
	}
}

static const char *display_error_detail(int err)
{
	switch (err) {
	case -EBUSY:
		return "the panel is still busy with an earlier call, retry later";
	case -ETIMEDOUT:
		return "the panel did not finish the refresh in time";
	default:
		return "the panel refused the frame";
	}
}

static void handle_show_frame(int client, const epaper_ShowFrame *frame,
			      upload_show_frame_t show_frame)
{
	const struct canvas_bitmap bitmap = {
		.width = CANVAS_WIDTH,
		.height = CANVAS_HEIGHT,
		.data = frame->pixels.bytes,
	};
	char detail[sizeof(((epaper_Response *)0)->detail)];
	int err;

	if (frame->width != CANVAS_WIDTH || frame->height != CANVAS_HEIGHT ||
	    frame->pixels.size != FRAME_BYTES) {
		snprintk(detail, sizeof(detail), "expected %dx%d and %d bytes, got %ux%u and %u",
			 CANVAS_WIDTH, CANVAS_HEIGHT, FRAME_BYTES, (unsigned int)frame->width,
			 (unsigned int)frame->height, (unsigned int)frame->pixels.size);
		LOG_WRN("Frame rejected, %s", detail);
		reply(client, epaper_Response_Status_STATUS_INVALID_REQUEST, detail);
		return;
	}

	LOG_INF("Frame received, refreshing the panel");

	err = show_frame(&bitmap);
	if (err < 0) {
		LOG_ERR("Frame not shown (%d)", err);
		reply(client, epaper_Response_Status_STATUS_DISPLAY_ERROR,
		      display_error_detail(err));
		return;
	}

	reply(client, epaper_Response_Status_STATUS_OK, "");
}

static void serve_client(int client, upload_show_frame_t show_frame)
{
	pb_istream_t stream = {
		.callback = read_socket,
		.state = INT_TO_POINTER(client),
		/*
		 * The largest well-formed request. A longer length prefix is
		 * then refused before any of its payload is read.
		 */
		.bytes_left = epaper_Request_size + LENGTH_PREFIX_MAX_BYTES,
	};

	if (!pb_decode_ex(&stream, epaper_Request_fields, &request, PB_DECODE_DELIMITED)) {
		LOG_WRN("Request rejected (%s)", PB_GET_ERROR(&stream));
		reply(client, epaper_Response_Status_STATUS_INVALID_REQUEST,
		      PB_GET_ERROR(&stream));
		return;
	}

	switch (request.which_command) {
	case epaper_Request_show_frame_tag:
		handle_show_frame(client, &request.command.show_frame, show_frame);
		break;
	default:
		LOG_WRN("Request rejected, no known command in it");
		reply(client, epaper_Response_Status_STATUS_INVALID_REQUEST, "unknown command");
		break;
	}
}

static int open_listening_socket(uint16_t port)
{
	const struct net_sockaddr_in address = {
		.sin_family = NET_AF_INET,
		.sin_port = net_htons(port),
		.sin_addr = {.s_addr = NET_INADDR_ANY},
	};
	const int reuse_address = 1;
	int server;
	int err;

	server = zsock_socket(NET_AF_INET, NET_SOCK_STREAM, NET_IPPROTO_TCP);
	if (server < 0) {
		return -errno;
	}

	/* Lets the port be bound again right after a reboot of the link. */
	if (zsock_setsockopt(server, ZSOCK_SOL_SOCKET, ZSOCK_SO_REUSEADDR, &reuse_address,
			     sizeof(reuse_address)) < 0) {
		LOG_WRN("Address reuse not enabled on the listening socket (%d)", -errno);
	}

	if (zsock_bind(server, (const struct net_sockaddr *)&address, sizeof(address)) < 0 ||
	    zsock_listen(server, 1) < 0) {
		err = -errno;
		zsock_close(server);
		return err;
	}

	return server;
}

int upload_server_run(uint16_t port, upload_show_frame_t show_frame)
{
	const struct zsock_timeval receive_timeout = {.tv_sec = CLIENT_RECEIVE_TIMEOUT_S};
	const int server = open_listening_socket(port);

	if (server < 0) {
		LOG_ERR("Cannot listen on TCP port %u (%d)", port, server);
		return server;
	}

	LOG_INF("Waiting for frames on TCP port %u", port);

	for (;;) {
		const int client = zsock_accept(server, NULL, NULL);

		if (client < 0) {
			LOG_ERR("Accept failed (%d)", -errno);
			k_sleep(ACCEPT_RETRY_DELAY);
			continue;
		}

		if (zsock_setsockopt(client, ZSOCK_SOL_SOCKET, ZSOCK_SO_RCVTIMEO, &receive_timeout,
				     sizeof(receive_timeout)) < 0) {
			LOG_WRN("No receive timeout on this client (%d)", -errno);
		}

		serve_client(client, show_frame);
		zsock_close(client);
	}
}
