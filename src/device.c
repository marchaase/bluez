/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2006-2007  Nokia Corporation
 *  Copyright (C) 2004-2008  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <gdbus.h>

#include "logging.h"
#include "textfile.h"

#include "hcid.h"
#include "adapter.h"
#include "device.h"
#include "dbus-common.h"
#include "dbus-hci.h"
#include "error.h"
#include "glib-helper.h"
#include "agent.h"
#include "sdp-xml.h"
#include "storage.h"

#define DEFAULT_XML_BUF_SIZE	1024
#define DISCONNECT_TIMER	2
#define DISCOVERY_TIMER		2000

struct btd_driver_data {
	struct btd_device_driver *driver;
	void *priv;
};

struct btd_device {
	bdaddr_t	bdaddr;
	gchar		*path;
	struct btd_adapter	*adapter;
	GSList		*uuids;
	GSList		*drivers;		/* List of driver_data */
	gboolean	temporary;
	struct agent	*agent;
	guint		disconn_timer;
	int		discov_active;		/* Service discovery active */
	char		*discov_requestor;	/* discovery requestor unique name */
	guint		discov_listener;
	guint		discov_timer;

	/* For Secure Simple Pairing */
	uint8_t		cap;
	uint8_t		auth;

	gboolean	connected;

	/* Whether were creating a security mode 3 connection */
	gboolean	secmode3;
};

struct browse_req {
	DBusConnection *conn;
	DBusMessage *msg;
	struct btd_device *device;
	GSList *uuids;
	GSList *uuids_added;
	GSList *uuids_removed;
	sdp_list_t *records;
	int search_uuid;
};

static uint16_t uuid_list[] = {
	PUBLIC_BROWSE_GROUP,
	PNP_INFO_SVCLASS_ID,
	HID_SVCLASS_ID,
	GENERIC_AUDIO_SVCLASS_ID,
	ADVANCED_AUDIO_SVCLASS_ID,
	AV_REMOTE_SVCLASS_ID,
	PANU_SVCLASS_ID,
	GN_SVCLASS_ID,
	NAP_SVCLASS_ID,
	SERIAL_PORT_SVCLASS_ID,
	0
};

static GSList *device_drivers = NULL;

static void device_free(gpointer user_data)
{
	struct btd_device *device = user_data;
	struct btd_adapter *adapter = device->adapter;
	struct agent *agent = adapter_get_agent(adapter);

	if (device->agent)
		agent_destroy(device->agent, FALSE);

	if (agent && agent_is_busy(agent, device))
		agent_cancel(agent);

	g_slist_foreach(device->uuids, (GFunc) g_free, NULL);
	g_slist_free(device->uuids);

	if (device->disconn_timer)
		g_source_remove(device->disconn_timer);

	if (device->discov_timer)
		g_source_remove(device->discov_timer);

	g_free(device->path);
	g_free(device);
}

static gboolean device_is_paired(struct btd_device *device)
{
	struct btd_adapter *adapter = device->adapter;
	char filename[PATH_MAX + 1], *str;
	char srcaddr[18], dstaddr[18];
	gboolean ret;
	bdaddr_t src;

	adapter_get_address(adapter, &src);
	ba2str(&src, srcaddr);
	ba2str(&device->bdaddr, dstaddr);

	create_name(filename, PATH_MAX, STORAGEDIR,
			srcaddr, "linkkeys");
	str = textfile_caseget(filename, dstaddr);
	ret = str ? TRUE : FALSE;
	g_free(str);

	return ret;
}

static DBusMessage *get_properties(DBusConnection *conn,
				DBusMessage *msg, void *user_data)
{
	struct btd_device *device = user_data;
	struct btd_adapter *adapter = device->adapter;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	bdaddr_t src;
	char name[248], srcaddr[18], dstaddr[18];
	char **uuids;
	const char *ptr;
	dbus_bool_t boolean;
	uint32_t class;
	int i;
	GSList *l;
	struct active_conn_info *dev;

	ba2str(&device->bdaddr, dstaddr);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

	/* Address */
	ptr = dstaddr;
	dbus_message_iter_append_dict_entry(&dict, "Address",
						DBUS_TYPE_STRING, &ptr);

	/* Name */
	ptr = NULL;
	memset(name, 0, sizeof(name));
	adapter_get_address(adapter, &src);
	ba2str(&src, srcaddr);

	if (read_device_name(srcaddr, dstaddr, name) == 0) {
		ptr = name;
		dbus_message_iter_append_dict_entry(&dict, "Name",
						DBUS_TYPE_STRING, &ptr);
	}

	/* Alias (fallback to name or address) */
	if (read_device_alias(srcaddr, dstaddr, name, sizeof(name)) < 1) {
		if (!ptr) {
			g_strdelimit(dstaddr, ":", '-');
			ptr = dstaddr;
		}
	} else
		ptr = name;

	if (ptr)
		dbus_message_iter_append_dict_entry(&dict, "Alias",
						DBUS_TYPE_STRING, &ptr);

	/* Class */
	if (read_remote_class(&src, &device->bdaddr, &class) == 0) {
		const char *icon = class_to_icon(class);

		dbus_message_iter_append_dict_entry(&dict, "Class",
						DBUS_TYPE_UINT32, &class);

		if (icon)
			dbus_message_iter_append_dict_entry(&dict, "Icon",
						DBUS_TYPE_STRING, &icon);
	}

	/* Paired */
	boolean = device_is_paired(device);
	dbus_message_iter_append_dict_entry(&dict, "Paired",
						DBUS_TYPE_BOOLEAN, &boolean);

	/* Trusted */
	boolean = read_trust(&src, dstaddr, GLOBAL_TRUST);
	dbus_message_iter_append_dict_entry(&dict, "Trusted",
						DBUS_TYPE_BOOLEAN, &boolean);

	/* Connected */
	dev = adapter_search_active_conn_by_bdaddr(adapter, &device->bdaddr);
	if (dev)
		boolean = TRUE;
	else
		boolean = FALSE;

	dbus_message_iter_append_dict_entry(&dict, "Connected",
						DBUS_TYPE_BOOLEAN, &boolean);

	/* UUIDs */
	uuids = g_new0(char *, g_slist_length(device->uuids) + 1);
	for (i = 0, l = device->uuids; l; l = l->next, i++)
		uuids[i] = l->data;
	dbus_message_iter_append_dict_entry(&dict, "UUIDs",
						DBUS_TYPE_ARRAY, &uuids);
	g_free(uuids);

	/* Adapter */
	ptr = adapter_get_path(adapter);
	dbus_message_iter_append_dict_entry(&dict, "Adapter",
						DBUS_TYPE_OBJECT_PATH, &ptr);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static DBusMessage *set_alias(DBusConnection *conn, DBusMessage *msg,
					const char *alias, void *data)
{
	struct btd_device *device = data;
	struct btd_adapter *adapter = device->adapter;
	char srcaddr[18], dstaddr[18];
	bdaddr_t src;
	int err;

	adapter_get_address(adapter, &src);
	ba2str(&src, srcaddr);
	ba2str(&device->bdaddr, dstaddr);

	/* Remove alias if empty string */
	err = write_device_alias(srcaddr, dstaddr,
			g_str_equal(alias, "") ? NULL : alias);
	if (err < 0)
		return g_dbus_create_error(msg,
				ERROR_INTERFACE ".Failed",
				strerror(-err));

	dbus_connection_emit_property_changed(conn, dbus_message_get_path(msg),
					DEVICE_INTERFACE, "Alias",
					DBUS_TYPE_STRING, &alias);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *set_trust(DBusConnection *conn, DBusMessage *msg,
					dbus_bool_t value, void *data)
{
	struct btd_device *device = data;
	struct btd_adapter *adapter = device->adapter;
	char srcaddr[18], dstaddr[18];
	bdaddr_t src;


	adapter_get_address(adapter, &src);
	ba2str(&src, srcaddr);
	ba2str(&device->bdaddr, dstaddr);

	write_trust(srcaddr, dstaddr, GLOBAL_TRUST, value);

	dbus_connection_emit_property_changed(conn, dbus_message_get_path(msg),
					DEVICE_INTERFACE, "Trusted",
					DBUS_TYPE_BOOLEAN, &value);

	return dbus_message_new_method_return(msg);
}

static inline DBusMessage *invalid_args(DBusMessage *msg)
{
	return g_dbus_create_error(msg,
			ERROR_INTERFACE ".InvalidArguments",
			"Invalid arguments in method call");
}

static DBusMessage *set_property(DBusConnection *conn,
				DBusMessage *msg, void *data)
{
	DBusMessageIter iter;
	DBusMessageIter sub;
	const char *property;

	if (!dbus_message_iter_init(msg, &iter))
		return invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &property);
	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return invalid_args(msg);
	dbus_message_iter_recurse(&iter, &sub);

	if (g_str_equal("Trusted", property)) {
		dbus_bool_t value;

		if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_BOOLEAN)
			return invalid_args(msg);
		dbus_message_iter_get_basic(&sub, &value);

		return set_trust(conn, msg, value, data);
	} else if (g_str_equal("Alias", property)) {
		const char *alias;

		if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_STRING)
			return invalid_args(msg);
		dbus_message_iter_get_basic(&sub, &alias);

		return set_alias(conn, msg, alias, data);
	}

	return invalid_args(msg);
}

static void discover_services_req_exit(void *user_data)
{
	struct btd_device *device = user_data;
	struct btd_adapter *adapter = device->adapter;
	bdaddr_t src;

	adapter_get_address(adapter, &src);

	debug("DiscoverDevices requestor exited");

	bt_cancel_discovery(&src, &device->bdaddr);
}

static DBusMessage *discover_services(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct btd_device *device = user_data;
	const char *pattern;
	int err;

	if (device->discov_active)
		return g_dbus_create_error(msg, ERROR_INTERFACE ".InProgress",
							"Discover in progress");

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &pattern,
						DBUS_TYPE_INVALID) == FALSE)
		goto fail;

	if (strlen(pattern) == 0) {
		err = device_browse(device, conn, msg, NULL, FALSE);
		if (err < 0)
			goto fail;
	} else {
		uuid_t uuid;

		if (bt_string2uuid(&uuid, pattern) < 0)
			return invalid_args(msg);

		err = device_browse(device, conn, msg, &uuid, FALSE);
		if (err < 0)
			goto fail;
	}

	return NULL;

fail:
	return g_dbus_create_error(msg, ERROR_INTERFACE ".Failed",
					"Discovery Failed");
}

static DBusMessage *cancel_discover(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct btd_device *device = user_data;
	struct btd_adapter *adapter = device->adapter;
	bdaddr_t src;

	adapter_get_address(adapter, &src);

	if (!device->discov_active)
		return g_dbus_create_error(msg,
				ERROR_INTERFACE ".Failed",
				"No pending discovery");

	/* only the discover requestor can cancel the inquiry process */
	if (!device->discov_requestor ||
			strcmp(device->discov_requestor, dbus_message_get_sender(msg)))
		return g_dbus_create_error(msg,
				ERROR_INTERFACE ".NotAuthorized",
				"Not Authorized");

	if (bt_cancel_discovery(&src, &device->bdaddr) < 0)
		return g_dbus_create_error(msg,
				ERROR_INTERFACE ".Failed",
				"No pending discover");

	return dbus_message_new_method_return(msg);
}

static gboolean disconnect_timeout(gpointer user_data)
{
	struct btd_device *device = user_data;
	struct active_conn_info *ci;
	disconnect_cp cp;
	int dd;
	uint16_t dev_id = adapter_get_dev_id(device->adapter);

	device->disconn_timer = 0;

	ci = adapter_search_active_conn_by_bdaddr(device->adapter,
							&device->bdaddr);

	if (!ci)
		return FALSE;

	dd = hci_open_dev(dev_id);
	if (dd < 0)
		goto fail;

	memset(&cp, 0, sizeof(cp));
	cp.handle = htobs(ci->handle);
	cp.reason = HCI_OE_USER_ENDED_CONNECTION;

	hci_send_cmd(dd, OGF_LINK_CTL, OCF_DISCONNECT,
			DISCONNECT_CP_SIZE, &cp);

	close(dd);

fail:
	return FALSE;
}

static DBusMessage *disconnect(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct btd_device *device = user_data;
	struct active_conn_info *dev;

	dev = adapter_search_active_conn_by_bdaddr(device->adapter,
							&device->bdaddr);

	if (!dev)
		return g_dbus_create_error(msg,
				ERROR_INTERFACE ".NotConnected",
				"Device is not connected");

	g_dbus_emit_signal(conn, device->path,
			DEVICE_INTERFACE, "DisconnectRequested",
			DBUS_TYPE_INVALID);

	device->disconn_timer = g_timeout_add_seconds(DISCONNECT_TIMER,
						disconnect_timeout, device);

	return dbus_message_new_method_return(msg);
}

static GDBusMethodTable device_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	get_properties	},
	{ "SetProperty",	"sv",	"",		set_property	},
	{ "DiscoverServices",	"s",	"a{us}",	discover_services,
						G_DBUS_METHOD_FLAG_ASYNC},
	{ "CancelDiscovery",	"",	"",		cancel_discover	},
	{ "Disconnect",		"",	"",		disconnect	},
	{ }
};

static GDBusSignalTable device_signals[] = {
	{ "PropertyChanged",		"sv"	},
	{ "DisconnectRequested",	""	},
	{ }
};

gboolean device_get_connected(struct btd_device *device)
{
	return device->connected;
}

void device_set_connected(DBusConnection *conn, struct btd_device *device,
			gboolean connected)
{
	device->connected = connected;

	dbus_connection_emit_property_changed(conn, device->path,
						DEVICE_INTERFACE,
						"Connected", DBUS_TYPE_BOOLEAN,
						&connected);

	if (connected && device->secmode3) {
		struct btd_adapter *adapter = device_get_adapter(device);
		bdaddr_t sba;

		adapter_get_address(adapter, &sba);

		device->secmode3 = FALSE;

		hcid_dbus_bonding_process_complete(&sba, &device->bdaddr, 0);
	}
}

void device_set_secmode3_conn(struct btd_device *device, gboolean enable)
{
	device->secmode3 = enable;
}

struct btd_device *device_create(DBusConnection *conn, struct btd_adapter *adapter,
					const gchar *address)
{
	gchar *address_up;
	struct btd_device *device;
	uint16_t dev_id = adapter_get_dev_id(adapter);

	device = g_try_malloc0(sizeof(struct btd_device));
	if (device == NULL)
		return NULL;

	address_up = g_ascii_strup(address, -1);
	device->path = g_strdup_printf("%s/hci%d/dev_%s", "/org/bluez",
							dev_id, address_up);
	g_strdelimit(device->path, ":", '_');
	g_free(address_up);

	debug("Creating device %s", device->path);

	if (g_dbus_register_interface(conn, device->path, DEVICE_INTERFACE,
				device_methods, device_signals, NULL,
				device, device_free) == FALSE) {
		device_free(device);
		return NULL;
	}

	str2ba(address, &device->bdaddr);
	device->adapter = adapter;

	return device;
}

void device_remove(DBusConnection *conn, struct btd_device *device)
{
	GSList *list;
	struct btd_device_driver *driver;
	gchar *path = g_strdup(device->path);

	debug("Removing device %s", path);

	for (list = device->drivers; list; list = list->next) {
		struct btd_driver_data *driver_data = list->data;
		driver = driver_data->driver;

		driver->remove(device);
		g_free(driver_data);
	}

	g_dbus_unregister_interface(conn, path, DEVICE_INTERFACE);

	g_free(path);
}

gint device_address_cmp(struct btd_device *device, const gchar *address)
{
	char addr[18];

	ba2str(&device->bdaddr, addr);
	return strcasecmp(addr, address);
}

void device_probe_drivers(struct btd_device *device, GSList *uuids, sdp_list_t *recs)
{
	GSList *list;
	const char **uuid;
	int err;

	debug("Probe drivers for %s", device->path);

	for (list = device_drivers; list; list = list->next) {
		struct btd_device_driver *driver = list->data;
		GSList *records = NULL;

		for (uuid = driver->uuids; *uuid; uuid++) {
			sdp_record_t *rec;

			if (!g_slist_find_custom(uuids, *uuid,
					(GCompareFunc) strcasecmp))
				continue;

			rec = find_record_in_list(recs, *uuid);
			if (!rec)
				continue;

			records = g_slist_append(records, rec);
		}

		if (records) {
			struct btd_driver_data *driver_data = g_new0(struct btd_driver_data, 1);

			err = driver->probe(device, records);
			if (err < 0) {
				error("probe failed for driver %s",
							driver->name);

				g_free(driver_data);
				continue;
			}

			driver_data->driver = driver;
			device->drivers = g_slist_append(device->drivers,
								driver_data);
		}
	}

	for (list = uuids; list; list = list->next) {
		GSList *l = g_slist_find_custom(device->uuids, list->data,
							(GCompareFunc) strcmp);
		if (l)
			continue;

		device->uuids = g_slist_insert_sorted(device->uuids,
					list->data, (GCompareFunc) strcmp);
	}
}

void device_remove_drivers(struct btd_device *device, GSList *uuids, sdp_list_t *recs)
{
	struct btd_adapter *adapter = device_get_adapter(device);
	GSList *list, *next;
	char srcaddr[18], dstaddr[18];
	bdaddr_t src;

	adapter_get_address(adapter, &src);
	ba2str(&src, srcaddr);
	ba2str(&device->bdaddr, dstaddr);

	debug("Remove drivers for %s", device->path);

	for (list = device->drivers; list; list = next) {
		struct btd_driver_data *driver_data = list->data;
		struct btd_device_driver *driver = driver_data->driver;
		const char **uuid;

		next = list->next;

		for (uuid = driver->uuids; *uuid; uuid++) {
			sdp_record_t *rec;

			if (!g_slist_find_custom(uuids, *uuid,
					(GCompareFunc) strcasecmp))
				continue;

			driver->remove(device);
			device->drivers = g_slist_remove(device->drivers,
								driver_data);

			g_free(driver_data);

			rec = find_record_in_list(recs, *uuid);
			if (!rec)
				continue;

			delete_record(srcaddr, dstaddr, rec->handle);
		}
	}

	for (list = uuids; list; list = list->next)
		device->uuids = g_slist_remove(device->uuids, list->data);
}

static void iter_append_record(DBusMessageIter *dict, uint32_t handle,
							const char *record)
{
	DBusMessageIter entry;

	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY,
							NULL, &entry);

	dbus_message_iter_append_basic(&entry, DBUS_TYPE_UINT32, &handle);

	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &record);

	dbus_message_iter_close_container(dict, &entry);
}

static void append_and_grow_string(void *data, const char *str)
{
	sdp_buf_t *buff = data;
	int len;

	len = strlen(str);

	if (!buff->data) {
		buff->data = malloc(DEFAULT_XML_BUF_SIZE);
		if (!buff->data)
			return;
		buff->buf_size = DEFAULT_XML_BUF_SIZE;
	}

	/* Grow string */
	while (buff->buf_size < (buff->data_size + len + 1)) {
		void *tmp;
		uint32_t new_size;

		/* Grow buffer by a factor of 2 */
		new_size = (buff->buf_size << 1);

		tmp = realloc(buff->data, new_size);
		if (!tmp)
			return;

		buff->data = tmp;
		buff->buf_size = new_size;
	}

	/* Include the NULL character */
	memcpy(buff->data + buff->data_size, str, len + 1);
	buff->data_size += len;
}

static void discover_device_reply(struct browse_req *req, sdp_list_t *recs)
{
	DBusMessage *reply;
	DBusMessageIter iter, dict;
	sdp_list_t *seq;

	reply = dbus_message_new_method_return(req->msg);
	if (!reply)
		return;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_UINT32_AS_STRING DBUS_TYPE_STRING_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

	for (seq = recs; seq; seq = seq->next) {
		sdp_record_t *rec = (sdp_record_t *) seq->data;
		sdp_buf_t result;

		if (!rec)
			break;

		memset(&result, 0, sizeof(sdp_buf_t));

		convert_sdp_record_to_xml(rec, &result,
				append_and_grow_string);

		if (result.data) {
			const char *val = (char *) result.data;
			iter_append_record(&dict, rec->handle, val);
			free(result.data);
		}
	}

	dbus_message_iter_close_container(&iter, &dict);

	dbus_connection_send(req->conn, reply, NULL);
	dbus_message_unref(reply);
}

static void services_changed(struct browse_req *req)
{
	DBusConnection *conn = get_dbus_connection();
	struct btd_device *device = req->device;
	char **uuids;
	GSList *l;
	int i;

	uuids = g_new0(char *, g_slist_length(device->uuids) + 1);
	for (i = 0, l = device->uuids; l; l = l->next, i++)
		uuids[i] = l->data;

	dbus_connection_emit_property_changed(conn, device->path,
					DEVICE_INTERFACE, "UUIDs",
					DBUS_TYPE_ARRAY, &uuids);

	g_free(uuids);
}

static int rec_cmp(const void *a, const void *b)
{
	const sdp_record_t *r1 = a;
	const sdp_record_t *r2 = b;

	return r1->handle - r2->handle;
}

static void update_services(struct browse_req *req, sdp_list_t *recs)
{
	struct btd_device *device = req->device;
	struct btd_adapter *adapter = device_get_adapter(device);
	sdp_list_t *seq;
	char srcaddr[18], dstaddr[18];
	bdaddr_t src;

	adapter_get_address(adapter, &src);
	ba2str(&src, srcaddr);
	ba2str(&device->bdaddr, dstaddr);

	for (seq = recs; seq; seq = seq->next) {
		sdp_record_t *rec = (sdp_record_t *) seq->data;
		sdp_buf_t pdu;
		sdp_list_t *svcclass = NULL;
		gchar *uuid_str;
		GSList *l;

		if (!rec)
			break;

		if (sdp_get_service_classes(rec, &svcclass) < 0)
			continue;

		/* Extract the first element and skip the remainning */
		uuid_str = bt_uuid2string(svcclass->data);
		if (!uuid_str) {
			sdp_list_free(svcclass, free);
			continue;
		}

		if (!strcasecmp(uuid_str, PNP_UUID)) {
			uint16_t source, vendor, product, version;
			sdp_data_t *pdlist;

			pdlist = sdp_data_get(rec, SDP_ATTR_VENDOR_ID_SOURCE);
			source = pdlist ? pdlist->val.uint16 : 0x0000;

			pdlist = sdp_data_get(rec, SDP_ATTR_VENDOR_ID);
			vendor = pdlist ? pdlist->val.uint16 : 0x0000;

			pdlist = sdp_data_get(rec, SDP_ATTR_PRODUCT_ID);
			product = pdlist ? pdlist->val.uint16 : 0x0000;

			pdlist = sdp_data_get(rec, SDP_ATTR_VERSION);
			version = pdlist ? pdlist->val.uint16 : 0x0000;

			if (source || vendor || product || version)
				store_device_id(srcaddr, dstaddr, source,
						vendor, product, version);
		}

		/* Driver UUID found */
		l = g_slist_find_custom(req->uuids, uuid_str,
				(GCompareFunc) strcasecmp);
		if (l) {
			char *uuid = l->data;

			req->uuids = g_slist_remove(req->uuids, uuid);
			g_free(uuid);
		}

		/* Check for duplicates */
		if (sdp_list_find(req->records, rec, rec_cmp)) {
			g_free(uuid_str);
			sdp_list_free(svcclass, free);
			continue;
		}

		store_record(srcaddr, dstaddr, rec);

		/* Copy record */
		if (sdp_gen_record_pdu(rec, &pdu) == 0) {
			sdp_record_t *record;
			int scanned;

			record = sdp_extract_pdu(pdu.data, pdu.data_size,
						&scanned);
			free(pdu.data);
			req->records = sdp_list_append(req->records, record);
		}

		l = g_slist_find_custom(device->uuids, uuid_str,
							(GCompareFunc) strcmp);
		if (!l)
			req->uuids_added = g_slist_append(req->uuids_added,
								uuid_str);
		else {
			req->uuids_removed = g_slist_remove(req->uuids_removed,
								l->data);
			g_free(uuid_str);
		}

		sdp_list_free(svcclass, free);
	}
}

static void store(struct btd_device *device)
{
	struct btd_adapter *adapter = device->adapter;
	bdaddr_t src;
	char *str;

	adapter_get_address(adapter, &src);

	if (!device->uuids) {
		write_device_profiles(&src, &device->bdaddr, "");
		return;
	}

	str = bt_list2string(device->uuids);
	write_device_profiles(&src, &device->bdaddr, str);
	g_free(str);
}

static void browse_req_free(struct browse_req *req)
{
	struct btd_device *device = req->device;

	device->discov_active = 0;

	if (device->discov_requestor) {
		g_dbus_remove_watch(req->conn, device->discov_listener);
		device->discov_listener = 0;
		g_free(device->discov_requestor);
		device->discov_requestor = NULL;
	}

	if (req->msg)
		dbus_message_unref(req->msg);
	if (req->conn)
		dbus_connection_unref(req->conn);
	g_slist_free(req->uuids_added);
	g_slist_free(req->uuids_removed);
	if (req->records)
		sdp_list_free(req->records, (sdp_free_func_t) sdp_record_free);
	g_free(req);
}

static void search_cb(sdp_list_t *recs, int err, gpointer user_data)
{
	struct browse_req *req = user_data;
	struct btd_device *device = req->device;
	DBusMessage *reply;

	if (err < 0) {
		error("%s: error updating services: %s (%d)",
				device->path, strerror(-err), -err);
		goto proceed;
	}

	update_services(req, recs);

	if (!req->uuids_added && !req->uuids_removed) {
		debug("%s: No service update", device->path);
		goto proceed;
	}

	/* Probe matching drivers for services added */
	if (req->uuids_added)
		device_probe_drivers(device, req->uuids_added, req->records);

	/* Remove drivers for services removed */
	if (req->uuids_removed)
		device_remove_drivers(device, req->uuids_removed, req->records);

	/* Propagate services changes */
	services_changed(req);

proceed:
	/* Store the device's profiles in the filesystem */
	store(device);

	if (req->msg) {
		if (dbus_message_is_method_call(req->msg, DEVICE_INTERFACE,
						"DiscoverServices")) {
			discover_device_reply(req, req->records);
			goto cleanup;
		}

		g_dbus_emit_signal(req->conn, dbus_message_get_path(req->msg),
				ADAPTER_INTERFACE, "DeviceCreated",
				DBUS_TYPE_OBJECT_PATH, &device->path,
				DBUS_TYPE_INVALID);
	}

	/* Update device list */
	adapter_update_devices(device->adapter);

	if (!req->msg)
		goto cleanup;

	/* Reply create device request */
	reply = dbus_message_new_method_return(req->msg);
	if (!reply)
		goto cleanup;

	dbus_message_append_args(reply, DBUS_TYPE_OBJECT_PATH, &device->path,
							DBUS_TYPE_INVALID);

	dbus_connection_send(req->conn, reply, NULL);
	dbus_message_unref(reply);

cleanup:
	browse_req_free(req);
}

static void browse_cb(sdp_list_t *recs, int err, gpointer user_data)
{
	struct browse_req *req = user_data;
	struct btd_device *device = req->device;
	struct btd_adapter *adapter = device->adapter;
	bdaddr_t src;
	uuid_t uuid;

	/* If we have a valid response and req->search_uuid == 1, then
	   public browsing was successful -- we don't need any more */
	if (err < 0 || (req->search_uuid == 1 && recs))
		goto done;

	update_services(req, recs);

	adapter_get_address(adapter, &src);

	/* Search for mandatory uuids */
	if (uuid_list[req->search_uuid]) {
		sdp_uuid16_create(&uuid, uuid_list[req->search_uuid++]);
		bt_search_service(&src, &device->bdaddr, &uuid,
						browse_cb, user_data, NULL);
		return;
	}

	/* Search for drivers uuids */
	if (req->uuids) {
		char *uuid_str = req->uuids->data;

		bt_string2uuid(&uuid, uuid_str);
		req->uuids = g_slist_remove(req->uuids, uuid_str);
		g_free(uuid_str);
		bt_search_service(&src, &device->bdaddr, &uuid,
						browse_cb, user_data, NULL);
		return;
	}

	/* Search for l2cap uuid */
	if (!req->records) {
		sdp_uuid16_create(&uuid, L2CAP_UUID);
		bt_search_service(&src, &device->bdaddr, &uuid,
						search_cb, user_data, NULL);
		return;
	}

done:
	search_cb(recs, err, user_data);
}

static void init_browse(struct browse_req *req, gboolean reverse)
{
	GSList *l;

	for (l = device_drivers; l; l = l->next) {
		struct btd_device_driver *driver = l->data;
		int i;

		for (i = 0; driver->uuids[i]; i++) {
			char *uuid;
			int j;

			/* Eliminate duplicates of UUIDs in uuid_list[]... */
			if (strlen(driver->uuids[i]) == 36 && 
			    !strncmp(driver->uuids[i], "0000", 4) &&
			    !strcasecmp(driver->uuids[i] + 8, 
					"-0000-1000-8000-00805F9B34FB")) {
				uint16_t uuid16 = strtol(driver->uuids[i],
							 NULL, 16);
				for (j = 0; uuid_list[j]; j++) {
					if (uuid16 == uuid_list[j])
						continue;
				}

			}
			/* ... and of UUIDs another driver already asked for */
			if (g_slist_find_custom(req->uuids, driver->uuids[i],
					(GCompareFunc) strcasecmp))
				continue;

			uuid = g_strdup(driver->uuids[i]);
			req->uuids = g_slist_append(req->uuids, uuid);
		}
	}

	/* If we are doing reverse-SDP don't try to detect removed profiles
	 * since some devices hide their service records while they are
	 * connected
	 */
	if (reverse)
		return;

	for (l = req->device->uuids; l; l = l->next)
		req->uuids_removed = g_slist_append(req->uuids_removed,
						l->data);
}

int device_browse(struct btd_device *device, DBusConnection *conn,
			DBusMessage *msg, uuid_t *search, gboolean reverse)
{
	struct btd_adapter *adapter = device->adapter;
	struct browse_req *req;
	bdaddr_t src;
	uuid_t uuid;
	bt_callback_t cb;
	int err;

	if (device->discov_active)
		return -EBUSY;

	adapter_get_address(adapter, &src);

	req = g_new0(struct browse_req, 1);
	if (conn && msg) {
		req->conn = dbus_connection_ref(conn);
		req->msg = dbus_message_ref(msg);
	}
	req->device = device;

	if (search) {
		memcpy(&uuid, search, sizeof(uuid_t));
		cb = search_cb;
	} else {
		sdp_uuid16_create(&uuid, uuid_list[req->search_uuid++]);
		init_browse(req, reverse);
		cb = browse_cb;
	}

	device->discov_active = 1;

	if (msg) {
		device->discov_requestor = g_strdup(dbus_message_get_sender(msg));
		/* Track the request owner to cancel it
		 * automatically if the owner exits */
		device->discov_listener = g_dbus_add_disconnect_watch(conn,
						dbus_message_get_sender(msg),
						discover_services_req_exit,
						device, NULL);
	}

	err = bt_search_service(&src, &device->bdaddr,
				&uuid, cb, req, NULL);
	if (err < 0)
		browse_req_free(req);

	return err;
}

struct btd_adapter *device_get_adapter(struct btd_device *device)
{
	if (!device)
		return NULL;

	return device->adapter;
}

void device_get_address(struct btd_device *device, bdaddr_t *bdaddr)
{
	bacpy(bdaddr, &device->bdaddr);
}

const gchar *device_get_path(struct btd_device *device)
{
	if (!device)
		return NULL;

	return device->path;
}

struct agent *device_get_agent(struct btd_device *device)
{
	if (!device)
		return NULL;

	return  device->agent;
}

void device_set_agent(struct btd_device *device, struct agent *agent)
{
	if (!device)
		return;

	device->agent = agent;
}

gboolean device_is_busy(struct btd_device *device)
{
	return device->discov_active ? TRUE : FALSE;
}

gboolean device_is_temporary(struct btd_device *device)
{
	return device->temporary;
}

void device_set_temporary(struct btd_device *device, gboolean temporary)
{
	if (!device)
		return;

	device->temporary = temporary;
}

void device_set_cap(struct btd_device *device, uint8_t cap)
{
	if (!device)
		return;

	device->cap = cap;
}

void device_set_auth(struct btd_device *device, uint8_t auth)
{
	if (!device)
		return;

	device->auth = auth;
}

uint8_t device_get_auth(struct btd_device *device)
{
	return device->auth;
}

static gboolean start_discovery(gpointer user_data)
{
	struct btd_device *device = user_data;

	device_browse(device, NULL, NULL, NULL, TRUE);

	device->discov_timer = 0;

	return FALSE;
}

int device_set_paired(DBusConnection *conn, struct btd_device *device,
			struct bonding_request_info *bonding)
{
	dbus_bool_t paired = TRUE;

	device_set_temporary(device, FALSE);

	dbus_connection_emit_property_changed(conn, device->path,
						DEVICE_INTERFACE, "Paired",
						DBUS_TYPE_BOOLEAN, &paired);

	/* If we were initiators start service discovery immediately.
	 * However if the other end was the initator wait a few seconds
	 * before SDP. This is due to potential IOP issues if the other
	 * end starts doing SDP at the same time as us */
	if (bonding) {
		/* If we are initiators remove any discovery timer and just
		 * start discovering services directly */
		if (device->discov_timer) {
			g_source_remove(device->discov_timer);
			device->discov_timer = 0;
		}

		return device_browse(device, bonding->conn, bonding->msg,
					NULL, FALSE);
	}

	/* If we are not initiators and there is no currently active discovery
	 * or discovery timer, set the discovery timer */
	if (!device->discov_active && !device->discov_timer)
		device->discov_timer = g_timeout_add(DISCOVERY_TIMER,
							start_discovery,
							device);

	return 0;
}

int btd_register_device_driver(struct btd_device_driver *driver)
{
	device_drivers = g_slist_append(device_drivers, driver);

	return 0;
}

void btd_unregister_device_driver(struct btd_device_driver *driver)
{
	device_drivers = g_slist_remove(device_drivers, driver);
}
