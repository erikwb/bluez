// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * BlueZ - Bluetooth protocol stack for Linux
 *
 * Exercise the real bearer methods and device cancellation with fake kernel
 * and D-Bus boundaries. Include device.c to construct pending ATT operations
 * without a controller, privileges, or a running bus.
 */

#include "src/device.c"

static const struct GDBusMethodTable *methods;
static GDBusDestroyFunction bearer_destroy;
static void *bearer_data;
static btd_disconnect_complete_t completion;
static void *completion_data;
static GQueue replies = G_QUEUE_INIT;
static unsigned int serial;
static int send_error;
static bool socket_error;
static int peer_fd = -1;
static uint8_t expected_address_type;
static unsigned int link_disconnects;

gboolean __wrap_g_dbus_register_interface(DBusConnection *conn,
		const char *path, const char *name,
		const GDBusMethodTable *method_table,
		const GDBusSignalTable *signals,
		const GDBusPropertyTable *properties, void *data,
		GDBusDestroyFunction destroy)
{
	methods = method_table;
	bearer_destroy = destroy;
	bearer_data = data;
	return TRUE;
}

gboolean __wrap_g_dbus_unregister_interface(DBusConnection *conn,
					const char *path, const char *name)
{
	bearer_destroy(bearer_data);
	bearer_data = NULL;
	return TRUE;
}

gboolean __wrap_g_dbus_send_message(DBusConnection *conn, DBusMessage *msg)
{
	g_assert_nonnull(msg);
	g_queue_push_tail(&replies, msg);
	return TRUE;
}

gboolean __wrap_g_dbus_send_reply(DBusConnection *conn, DBusMessage *msg,
							int type, ...)
{
	g_assert_cmpint(type, ==, DBUS_TYPE_INVALID);
	return __wrap_g_dbus_send_message(conn,
					dbus_message_new_method_return(msg));
}

void __wrap_g_dbus_emit_property_changed(DBusConnection *conn,
		const char *path, const char *interface, const char *property)
{
}

gboolean __wrap_g_dbus_emit_signal(DBusConnection *conn, const char *path,
		const char *interface, const char *name, int type, ...)
{
	return TRUE;
}

const bdaddr_t *__wrap_btd_adapter_get_address(struct btd_adapter *adapter)
{
	static const bdaddr_t address;

	return &address;
}

uint8_t __wrap_btd_adapter_get_address_type(struct btd_adapter *adapter)
{
	return BDADDR_LE_PUBLIC;
}

GIOChannel *__wrap_bt_io_connect(BtIOConnect connect, gpointer data,
				GDestroyNotify destroy, GError **err,
				BtIOOption opt, ...)
{
	GIOChannel *io;
	int fds[2];

	if (socket_error) {
		g_set_error(err, BT_IO_ERROR, EIO, "Test socket error");
		return NULL;
	}

	g_assert_cmpint(peer_fd, ==, -1);
	g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), ==, 0);
	io = g_io_channel_unix_new(fds[0]);
	g_io_channel_set_close_on_unref(io, TRUE);
	peer_fd = fds[1];
	return io;
}

int __wrap_btd_adapter_disconnect_device_full(struct btd_adapter *adapter,
				const bdaddr_t *addr, uint8_t type,
				btd_disconnect_complete_t callback, void *data)
{
	g_assert_cmpint(type, ==, expected_address_type);
	g_assert_null(completion);

	if (send_error)
		return send_error;

	completion = callback;
	completion_data = data;
	return 0;
}

int __wrap_btd_adapter_disconnect_device(struct btd_adapter *adapter,
					const bdaddr_t *addr, uint8_t type)
{
	g_assert_cmpint(type, ==, expected_address_type);
	link_disconnects++;
	return 0;
}

static DBusMessage *new_call(const char *interface, const char *method)
{
	DBusMessage *msg = dbus_message_new_method_call("org.bluez",
				"/org/bluez/hci0/dev_00_11_22_33_44_55",
				interface, method);

	dbus_message_set_serial(msg, ++serial);
	return msg;
}

static unsigned int call_bearer(struct btd_bearer *bearer, const char *method)
{
	DBusMessage *msg = new_call(BTD_BEARER_LE_INTERFACE, method);
	DBusMessage *reply;
	unsigned int call_serial = dbus_message_get_serial(msg);
	unsigned int i;

	for (i = 0; methods[i].name; i++) {
		if (strcmp(methods[i].name, method))
			continue;

		reply = methods[i].function(NULL, msg, bearer);
		if (reply)
			g_queue_push_tail(&replies, reply);

		dbus_message_unref(msg);
		return call_serial;
	}

	g_assert_not_reached();
}

static void expect_reply(unsigned int call_serial, const char *error_name)
{
	DBusMessage *reply = g_queue_pop_head(&replies);

	g_assert_nonnull(reply);
	g_assert_cmpuint(dbus_message_get_reply_serial(reply), ==, call_serial);
	g_assert_cmpstr(dbus_message_get_error_name(reply), ==, error_name);
	g_assert_cmpint(dbus_message_get_type(reply), ==,
			error_name ? DBUS_MESSAGE_TYPE_ERROR :
					DBUS_MESSAGE_TYPE_METHOD_RETURN);
	dbus_message_unref(reply);
}

static void complete(uint8_t status)
{
	btd_disconnect_complete_t callback = completion;
	void *data = completion_data;

	g_assert_nonnull(callback);
	completion = NULL;
	completion_data = NULL;
	callback(status, data);
}

static void setup(struct btd_device *dev, gconstpointer data)
{
	dev->ref_count = 1;
	dev->path = "/org/bluez/hci0/dev_00_11_22_33_44_55";
	dev->bdaddr_type = BDADDR_LE_PUBLIC;
	expected_address_type = BDADDR_LE_PUBLIC;
	link_disconnects = 0;
	/* Classic stays connected throughout all cancellation scenarios. */
	dev->bredr_state.connected = true;
	btd_opts.mode = BT_MODE_DUAL;
	dev->le = btd_bearer_new(dev, BDADDR_LE_PUBLIC);
}

static void teardown(struct btd_device *dev, gconstpointer data)
{
	g_assert_null(completion);
	g_assert_true(g_queue_is_empty(&replies));
	g_assert_cmpint(dev->ref_count, ==, 1);
	g_assert_true(dev->bredr_state.connected);
	g_assert_false(btd_bearer_is_disconnecting(dev->le));
	btd_bearer_destroy(dev->le);

	if (dev->att_io) {
		g_io_channel_shutdown(dev->att_io, FALSE, NULL);
		g_io_channel_unref(dev->att_io);
	}

	if (peer_fd >= 0) {
		close(peer_fd);
		peer_fd = -1;
	}

	send_error = 0;
	socket_error = false;
}

static void test_status(struct btd_device *dev, gconstpointer data)
{
	uint8_t status = GPOINTER_TO_UINT(data);
	unsigned int id = call_bearer(dev->le, "Disconnect");
	const char *error_name = NULL;

	g_assert_true(g_queue_is_empty(&replies));
	g_assert_true(btd_bearer_is_disconnecting(dev->le));
	g_assert_cmpint(dev->ref_count, ==, 2);
	complete(status);

	if (status == MGMT_STATUS_NOT_CONNECTED)
		error_name = ERROR_INTERFACE ".NotConnected";
	else if (status != MGMT_STATUS_SUCCESS &&
					status != MGMT_STATUS_DISCONNECTED)
		error_name = ERROR_INTERFACE ".Failed";

	expect_reply(id, error_name);
}

static void test_pending_connect(struct btd_device *dev, gconstpointer data)
{
	unsigned int conn = call_bearer(dev->le, "Connect");
	unsigned int disc;
	char byte;

	g_assert_nonnull(dev->att_io);
	g_assert_true(g_queue_is_empty(&replies));
	disc = call_bearer(dev->le, "Disconnect");
	expect_reply(conn, ERROR_INTERFACE ".Failed");
	g_assert_null(dev->att_io);
	/* The fake ATT socket was closed, not just forgotten. */
	g_assert_cmpint(read(peer_fd, &byte, 1), ==, 0);
	close(peer_fd);
	peer_fd = -1;

	/* Closing ATT can remove the connection before MGMT processes it. */
	complete(MGMT_STATUS_NOT_CONNECTED);
	expect_reply(disc, NULL);
	conn = call_bearer(dev->le, "Connect");
	g_assert_true(g_queue_is_empty(&replies));
	g_assert_nonnull(dev->att_io);
	btd_bearer_connected(dev->le, -ECONNABORTED);
	expect_reply(conn, ERROR_INTERFACE ".Failed");
}

static void test_device_connect(struct btd_device *dev, gconstpointer data)
{
	unsigned int disc;
	unsigned int conn;
	bool classic = GPOINTER_TO_INT(data);

	dev->connect = new_call(DEVICE_INTERFACE, "Connect");
	conn = dbus_message_get_serial(dev->connect);
	dev->connect_bdaddr_type = classic ? BDADDR_BREDR : BDADDR_LE_PUBLIC;
	disc = call_bearer(dev->le, "Disconnect");

	if (classic) {
		g_assert_nonnull(dev->connect);
		g_assert_true(g_queue_is_empty(&replies));
		dbus_message_unref(dev->connect);
		dev->connect = NULL;
	} else {
		g_assert_null(dev->connect);
		expect_reply(conn, ERROR_INTERFACE ".Failed");
	}

	complete(MGMT_STATUS_NOT_CONNECTED);
	expect_reply(disc, classic ? ERROR_INTERFACE ".NotConnected" : NULL);
}

static void test_busy(struct btd_device *dev, gconstpointer data)
{
	unsigned int disc = call_bearer(dev->le, "Disconnect");
	unsigned int id;

	id = call_bearer(dev->le, "Connect");
	expect_reply(id, ERROR_INTERFACE ".InProgress");
	id = call_bearer(dev->le, "Disconnect");
	expect_reply(id, ERROR_INTERFACE ".InProgress");
	g_assert_cmpint(device_connect_le(dev), ==, -EBUSY);
	g_assert_cmpint(device_browse_gatt(dev, NULL), ==, -EBUSY);
	complete(MGMT_STATUS_SUCCESS);
	expect_reply(disc, NULL);
}

static void test_event_before_reply(struct btd_device *dev, gconstpointer data)
{
	unsigned int disc = call_bearer(dev->le, "Disconnect");
	unsigned int id;

	btd_bearer_disconnected(dev->le, MGMT_DEV_DISCONN_LOCAL_HOST);
	g_assert_true(g_queue_is_empty(&replies));
	/* The old command's completion must not consume a new request. */
	id = call_bearer(dev->le, "Disconnect");
	expect_reply(id, ERROR_INTERFACE ".InProgress");
	complete(MGMT_STATUS_SUCCESS);
	expect_reply(disc, NULL);
	g_assert_true(g_queue_is_empty(&replies));
}

static void test_send_failure(struct btd_device *dev, gconstpointer data)
{
	unsigned int conn = call_bearer(dev->le, "Connect");
	unsigned int disc;

	send_error = -EIO;
	disc = call_bearer(dev->le, "Disconnect");
	expect_reply(disc, ERROR_INTERFACE ".Failed");
	g_assert_nonnull(dev->att_io);
	g_assert_false(btd_bearer_is_disconnecting(dev->le));
	send_error = 0;
	disc = call_bearer(dev->le, "Disconnect");
	expect_reply(conn, ERROR_INTERFACE ".Failed");
	complete(MGMT_STATUS_SUCCESS);
	expect_reply(disc, NULL);
}

static void test_connect_failure(struct btd_device *dev, gconstpointer data)
{
	unsigned int id;

	socket_error = true;
	id = call_bearer(dev->le, "Connect");
	expect_reply(id, ERROR_INTERFACE ".Failed");
	socket_error = false;
	id = call_bearer(dev->le, "Connect");
	g_assert_nonnull(dev->att_io);
	g_assert_true(g_queue_is_empty(&replies));
	btd_bearer_connected(dev->le, -ECONNABORTED);
	expect_reply(id, ERROR_INTERFACE ".Failed");
}

static void test_browse(struct btd_device *dev, gconstpointer data)
{
	unsigned int conn;
	unsigned int disc;
	bool classic = GPOINTER_TO_INT(data);

	dev->browse = g_new0(struct browse_req, 1);
	dev->browse->device = dev;
	dev->browse->type = classic ? BROWSE_SDP : BROWSE_GATT;
	dev->browse->msg = new_call(DEVICE_INTERFACE, "Connect");
	conn = dbus_message_get_serial(dev->browse->msg);
	disc = call_bearer(dev->le, "Disconnect");

	if (classic) {
		g_assert_nonnull(dev->browse);
		browse_request_free(dev->browse);
	} else {
		g_assert_null(dev->browse);
		expect_reply(conn, ERROR_INTERFACE ".Failed");
	}

	complete(MGMT_STATUS_SUCCESS);
	expect_reply(disc, NULL);
}

static void test_bredr_disconnected(struct btd_device *dev, gconstpointer data)
{
	unsigned int id;

	btd_bearer_destroy(dev->le);
	dev->le = btd_bearer_new(dev, BDADDR_BREDR);
	dev->bredr_state.connected = false;
	id = call_bearer(dev->le, "Disconnect");
	expect_reply(id, ERROR_INTERFACE ".NotConnected");
	g_assert_null(completion);
	dev->bredr_state.connected = true;
}

static void test_pairing(struct btd_device *dev, gconstpointer data)
{
	struct bonding_req bonding = {};
	unsigned int id;

	dev->bonding = &bonding;
	id = call_bearer(dev->le, "Disconnect");
	expect_reply(id, ERROR_INTERFACE ".InProgress");
	g_assert_null(completion);
	g_assert_true(dev->bonding == &bonding);
	dev->bonding = NULL;
}

static void test_disconnect_timer(struct btd_device *dev, gconstpointer data)
{
	unsigned int disc;

	dev->le_state.connected = true;
	disc = call_bearer(dev->le, "Disconnect");
	g_assert_true(btd_bearer_is_disconnecting(dev->le));
	g_assert_null(completion);
	dev->le_state.connected = false;
	btd_bearer_disconnected(dev->le, MGMT_DEV_DISCONN_REMOTE);
	expect_reply(disc, NULL);
	g_assert_false(btd_bearer_is_disconnecting(dev->le));
}

static gboolean disconnect_deadline(gpointer data)
{
	g_error("Bearer disconnect timer did not run");
	return FALSE;
}

static void test_random_address(struct btd_device *dev, gconstpointer data)
{
	unsigned int disc;
	guint deadline;

	dev->bdaddr_type = BDADDR_LE_RANDOM;
	expected_address_type = BDADDR_LE_RANDOM;
	dev->le_state.connected = GPOINTER_TO_INT(data);
	disc = call_bearer(dev->le, "Disconnect");

	if (dev->le_state.connected) {
		deadline = g_timeout_add_seconds(5, disconnect_deadline, NULL);

		while (!link_disconnects)
			g_main_context_iteration(NULL, TRUE);

		g_source_remove(deadline);
		dev->le_state.connected = false;
		btd_bearer_disconnected(dev->le, MGMT_DEV_DISCONN_LOCAL_HOST);
	} else
		complete(MGMT_STATUS_SUCCESS);

	expect_reply(disc, NULL);
}

int main(int argc, char *argv[])
{
	g_test_init(&argc, &argv, NULL);

#define TEST(name, data, func) \
	g_test_add(name, struct btd_device, GUINT_TO_POINTER(data), \
			setup, func, teardown)

	TEST("/bearer/stale-link", MGMT_STATUS_SUCCESS, test_status);
	TEST("/bearer/not-connected", MGMT_STATUS_NOT_CONNECTED, test_status);
	TEST("/bearer/disconnected", MGMT_STATUS_DISCONNECTED, test_status);
	TEST("/bearer/kernel-failure", MGMT_STATUS_FAILED, test_status);
	TEST("/bearer/kernel-timeout", MGMT_STATUS_TIMEOUT, test_status);
	TEST("/bearer/request-cancelled", MGMT_STATUS_CANCELLED, test_status);
	TEST("/bearer/pending-connect", 0, test_pending_connect);
	TEST("/bearer/device-le-connect", 0, test_device_connect);
	TEST("/bearer/device-bredr-connect", 1, test_device_connect);
	TEST("/bearer/busy", 0, test_busy);
	TEST("/bearer/event-before-reply", 0, test_event_before_reply);
	TEST("/bearer/send-failure", 0, test_send_failure);
	TEST("/bearer/connect-failure", 0, test_connect_failure);
	TEST("/bearer/gatt-browse", 0, test_browse);
	TEST("/bearer/sdp-browse", 1, test_browse);
	TEST("/bearer/bredr-disconnected", 0, test_bredr_disconnected);
	TEST("/bearer/pairing", 0, test_pairing);
	TEST("/bearer/disconnect-timer", 0, test_disconnect_timer);
	TEST("/bearer/random-address/pending", 0, test_random_address);
	TEST("/bearer/random-address/connected", 1, test_random_address);

	return g_test_run();
}
