// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * BlueZ - Bluetooth protocol stack for Linux
 *
 * Test disconnect completion through a real MGMT instance on a socket pair.
 */

#include "src/adapter.c"

struct test_context {
	struct btd_adapter adapter;
	int peer;
	unsigned int deadline;
	unsigned int replies;
	uint8_t status;
};

static bool deadline_expired(void *data)
{
	g_error("MGMT disconnect test timed out");
	return false;
}

static void setup(struct test_context *ctx, gconstpointer data)
{
	int fds[2];

	g_assert_cmpint(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fds), ==, 0);
	ctx->adapter.ref_count = 1;
	ctx->adapter.dev_id = 7;
	ctx->adapter.mgmt = mgmt_new(fds[0]);
	mgmt_set_close_on_unref(ctx->adapter.mgmt, true);
	ctx->peer = fds[1];
	ctx->deadline = timeout_add_seconds(3, deadline_expired, NULL, NULL);
}

static void teardown(struct test_context *ctx, gconstpointer data)
{
	g_assert_cmpint(ctx->adapter.ref_count, ==, 1);
	g_assert_null(ctx->adapter.disconnects);
	mgmt_unref(ctx->adapter.mgmt);
	close(ctx->peer);
	timeout_remove(ctx->deadline);
}

static void disconnected(uint8_t status, void *data)
{
	struct test_context *ctx = data;

	ctx->status = status;
	ctx->replies++;
}

static ssize_t read_command(struct test_context *ctx, void *packet, size_t size)
{
	ssize_t len;

	while ((len = recv(ctx->peer, packet, size, MSG_DONTWAIT)) < 0) {
		g_assert_cmpint(errno, ==, EAGAIN);
		g_main_context_iteration(NULL, TRUE);
	}

	return len;
}

static void send_disconnect(struct test_context *ctx)
{
	struct {
		struct mgmt_hdr hdr;
		struct mgmt_cp_disconnect cp;
	} __packed command;
	bdaddr_t addr = {{ 1, 2, 3, 4, 5, 6 }};
	ssize_t len;
	int err;

	err = btd_adapter_disconnect_device_full(&ctx->adapter, &addr,
					BDADDR_LE_RANDOM, disconnected, ctx);
	g_assert_cmpint(err, ==, 0);
	g_assert_cmpint(ctx->adapter.ref_count, ==, 2);

	len = read_command(ctx, &command, sizeof(command));
	g_assert_cmpint(len, ==, sizeof(command));
	g_assert_cmpint(btohs(command.hdr.opcode), ==, MGMT_OP_DISCONNECT);
	g_assert_cmpint(btohs(command.hdr.index), ==, 7);
	g_assert_cmpint(command.cp.addr.type, ==, BDADDR_LE_RANDOM);
	g_assert_cmpint(bacmp(&command.cp.addr.bdaddr, &addr), ==, 0);
}

static void test_response(struct test_context *ctx, gconstpointer data)
{
	uint8_t status = GPOINTER_TO_UINT(data);
	struct {
		struct mgmt_hdr hdr;
		struct mgmt_ev_cmd_complete ev;
		struct mgmt_rp_disconnect rp;
	} __packed response = {
		.hdr.opcode = htobs(MGMT_EV_CMD_COMPLETE),
		.hdr.index = htobs(7),
		.hdr.len = htobs(sizeof(response.ev) + sizeof(response.rp)),
		.ev.opcode = htobs(MGMT_OP_DISCONNECT),
		.ev.status = status,
		.rp.addr.type = BDADDR_LE_RANDOM,
	};

	send_disconnect(ctx);
	g_assert_cmpint(write(ctx->peer, &response, sizeof(response)), ==,
							sizeof(response));

	while (!ctx->replies)
		g_main_context_iteration(NULL, TRUE);

	g_assert_cmpuint(ctx->replies, ==, 1);
	g_assert_cmpint(ctx->status, ==, status);
}

static void test_short_response(struct test_context *ctx, gconstpointer data)
{
	struct {
		struct mgmt_hdr hdr;
		struct mgmt_ev_cmd_complete ev;
	} __packed response = {
		.hdr.opcode = htobs(MGMT_EV_CMD_COMPLETE),
		.hdr.index = htobs(7),
		.hdr.len = htobs(sizeof(response.ev)),
		.ev.opcode = htobs(MGMT_OP_DISCONNECT),
		.ev.status = MGMT_STATUS_SUCCESS,
	};

	send_disconnect(ctx);
	g_assert_cmpint(write(ctx->peer, &response, sizeof(response)), ==,
							sizeof(response));

	while (!ctx->replies)
		g_main_context_iteration(NULL, TRUE);

	g_assert_cmpuint(ctx->replies, ==, 1);
	g_assert_cmpint(ctx->status, ==, MGMT_STATUS_FAILED);
}

static void test_cancel(struct test_context *ctx, gconstpointer data)
{
	int err;

	if (GPOINTER_TO_INT(data))
		send_disconnect(ctx);
	else {
		err = btd_adapter_disconnect_device_full(&ctx->adapter,
					BDADDR_ANY, BDADDR_LE_PUBLIC,
					disconnected, ctx);
		g_assert_cmpint(err, ==, 0);
	}

	mgmt_cancel_index(ctx->adapter.mgmt, ctx->adapter.dev_id);
	g_assert_cmpuint(ctx->replies, ==, 1);
	g_assert_cmpint(ctx->status, ==, MGMT_STATUS_CANCELLED);
}

static void test_send_failure(struct test_context *ctx, gconstpointer data)
{
	int err;

	mgmt_unref(ctx->adapter.mgmt);
	ctx->adapter.mgmt = NULL;
	err = btd_adapter_disconnect_device_full(&ctx->adapter, BDADDR_ANY,
					BDADDR_LE_PUBLIC, disconnected, ctx);
	g_assert_cmpint(err, ==, -EIO);
	g_assert_cmpuint(ctx->replies, ==, 0);
}

static void test_priority(struct test_context *ctx, gconstpointer data)
{
	struct mgmt_hdr command;
	unsigned int id;

	/* Leave an ordinary command pending: cancellation must bypass it. */
	id = mgmt_send(ctx->adapter.mgmt, MGMT_OP_READ_INFO, 7, 0, NULL,
							NULL, NULL, NULL);
	g_assert_cmpuint(id, >, 0);
	g_assert_cmpint(read_command(ctx, &command, sizeof(command)), ==,
							sizeof(command));
	g_assert_cmpuint(btohs(command.opcode), ==, MGMT_OP_READ_INFO);
	send_disconnect(ctx);
	mgmt_cancel_index(ctx->adapter.mgmt, 7);
	g_assert_cmpuint(ctx->replies, ==, 1);
	g_assert_cmpuint(ctx->status, ==, MGMT_STATUS_CANCELLED);
}

static void test_remove(struct test_context *ctx, gconstpointer data)
{
	int err;

	if (GPOINTER_TO_INT(data))
		send_disconnect(ctx);
	else {
		err = btd_adapter_disconnect_device_full(&ctx->adapter,
					BDADDR_ANY, BDADDR_LE_PUBLIC,
					disconnected, ctx);
		g_assert_cmpint(err, ==, 0);
	}

	adapter_remove(&ctx->adapter);
	g_assert_cmpuint(ctx->replies, ==, 1);
	g_assert_cmpuint(ctx->status, ==, MGMT_STATUS_CANCELLED);
	g_assert_cmpint(ctx->adapter.ref_count, ==, 1);
	g_assert_null(ctx->adapter.disconnects);

	err = btd_adapter_disconnect_device_full(&ctx->adapter, BDADDR_ANY,
					BDADDR_LE_PUBLIC, disconnected, ctx);
	g_assert_cmpint(err, ==, -ENODEV);
	g_assert_cmpuint(ctx->replies, ==, 1);
}

static void notification(uint16_t index, uint16_t length,
					const void *param, void *user_data)
{
}

static void test_replacement(struct test_context *ctx, gconstpointer data)
{
	struct btd_adapter *old = g_new0(struct btd_adapter, 1);
	unsigned int id;
	int err;

	old->ref_count = 1;
	old->dev_id = 7;
	old->mgmt = mgmt_ref(ctx->adapter.mgmt);
	old->auths = g_queue_new();
	err = btd_adapter_disconnect_device_full(old, BDADDR_ANY,
					BDADDR_LE_PUBLIC, disconnected, ctx);
	g_assert_cmpint(err, ==, 0);

	adapter_remove(old);
	g_assert_cmpuint(ctx->replies, ==, 1);
	g_assert_cmpint(old->ref_count, ==, 1);
	btd_adapter_unref(old);

	/* Old requests must not retain the removed adapter or its index. */
	id = mgmt_register(ctx->adapter.mgmt, MGMT_EV_NEW_SETTINGS, 7,
						notification, NULL, NULL);
	g_assert_cmpuint(id, >, 0);

	while (g_main_context_iteration(NULL, FALSE))
		;

	g_assert_true(mgmt_unregister(ctx->adapter.mgmt, id));
}

int main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

#define TEST(name, data, func) \
	g_test_add(name, struct test_context, GUINT_TO_POINTER(data), \
			setup, func, teardown)

	TEST("/adapter/disconnect/success", MGMT_STATUS_SUCCESS, test_response);
	TEST("/adapter/disconnect/not-connected", MGMT_STATUS_NOT_CONNECTED,
								test_response);
	TEST("/adapter/disconnect/failed", MGMT_STATUS_FAILED, test_response);
	TEST("/adapter/disconnect/short-response", 0, test_short_response);
	TEST("/adapter/disconnect/cancel-queued", 0, test_cancel);
	TEST("/adapter/disconnect/cancel-pending", 1, test_cancel);
	TEST("/adapter/disconnect/send-failure", 0, test_send_failure);
	TEST("/adapter/disconnect/priority", 0, test_priority);
	TEST("/adapter/disconnect/remove-queued", 0, test_remove);
	TEST("/adapter/disconnect/remove-pending", 1, test_remove);
	TEST("/adapter/disconnect/replacement", 0, test_replacement);

	return g_test_run();
}
