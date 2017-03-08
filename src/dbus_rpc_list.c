/*
 * Copyright (C) 2016 Inteno Broadband Technology AB. All rights reserved.
 *
 * Author: Denis Osvald <denis.osvald@sartura.hr>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

/*
 * dbus over websocket - dbus list
 */
#include "dbus_rpc_list.h"

#include "common.h"
#include "wsubus.impl.h"
#include "rpc.h"
#include "util_ubus_blob.h"

#include <libubox/blobmsg_json.h>
#include <libubox/blobmsg.h>
#include <dbus/dbus.h>

#include <libwebsockets.h>

#include <assert.h>
#include <sys/types.h>
#include <regex.h>
#include <alloca.h>

struct wsd_call_ctx {
	struct lws *wsi;

	struct blob_attr *id;
	struct ubusrpc_blob_list *list_args;

	struct blob_buf retbuf;
	struct DBusMessageIter arr_iter;

	struct DBusMessage *reply;
	int reply_slot;

	struct DBusPendingCall *call_req;

	struct list_head cq;
};

static void wsd_introspect_cb(DBusPendingCall *call, void *data);

static void introspect_list_next(struct wsd_call_ctx *ctx)
{
	struct prog_context *prog = lws_context_user(lws_get_context(ctx->wsi));
	const char *str;
	dbus_message_iter_get_basic(&ctx->arr_iter, &str);
	// TODO select a reasonable object path we will support
	DBusMessage *introspect = dbus_message_new_method_call(str, "/org/freedesktop", DBUS_INTERFACE_INTROSPECTABLE, "Introspect");
	DBusPendingCall *introspect_call;
	dbus_connection_send_with_reply(prog->dbus_ctx, introspect, &introspect_call, -1);
	dbus_pending_call_set_notify(introspect_call, wsd_introspect_cb, ctx, NULL);
	dbus_message_unref(introspect);
	dbus_pending_call_unref(introspect_call);
}

static void introspect_list_finish(struct wsd_call_ctx *ctx)
{
	char *response_str = jsonrpc__resp_ubus(ctx->id, 0, ctx->retbuf.head);
	//char *response_str = blobmsg_format_json(ctx->retbuf.head, true);
	wsu_queue_write_str(ctx->wsi, response_str);
	free(response_str);
	dbus_message_unref(ctx->reply);
}

/* FIXME
don't invoke wrath of Zalgo by using regex on XML
http://stackoverflow.com/questions/1732348/regex-match-open-tags-except-xhtml-self-contained-tags/1732454#1732454
*/
static regex_t interface_regex;
static regex_t method_regex;
static regex_t arg_regex;
static regex_t attr_regex;

__attribute__((constructor)) static void _init(void)
{
	regcomp(&interface_regex, "\\(<interface name=\"\\)\\([^\"]*\\)\"", 0);
	regcomp(&method_regex, "\\(<method name=\"\\)\\([^\"]*\\)\"", 0);
	regcomp(&arg_regex, "\\(<arg \\)\\([^>]*\\)/>", 0);
	regcomp(&attr_regex, "\\([^ \t]*\\)=\"\\([^\"]*\\)\"", 0);
}

__attribute__((destructor)) static void _dtor(void)
{
	regfree(&interface_regex);
	regfree(&method_regex);
	regfree(&arg_regex);
	regfree(&attr_regex);
}

static void wsd_introspect_cb(DBusPendingCall *call, void *data)
{
	struct wsd_call_ctx *ctx = data;

	DBusMessage *reply = dbus_pending_call_steal_reply(call);

	const char *obj;
	dbus_message_iter_get_basic(&ctx->arr_iter, &obj);
	void *p = blobmsg_open_table(&ctx->retbuf, obj);
	lwsl_debug(">>>>>>>>>> Introspected %s\n", obj);

	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_METHOD_RETURN && !strcmp(dbus_message_get_signature(reply), "s")) {
		const char *xml;
		regmatch_t imatch[3];
		dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &xml);
		size_t xml_len = strlen(xml);
		for (const char *icur = xml; icur < xml + xml_len
				&& regexec(&interface_regex, icur, ARRAY_SIZE(imatch), imatch, 0) == 0
				&& imatch[2].rm_so >= 0; icur += imatch[0].rm_eo) {
			size_t iface_len = imatch[2].rm_eo - imatch[2].rm_so;
			char iface[iface_len+1];
			strncpy(iface, icur + imatch[2].rm_so, iface_len);
			iface[iface_len] = '\0';

			char *iendcur = strstr(icur + imatch[2].rm_eo, "</interface>");
			if (!iendcur) {
				continue;
			}

			regmatch_t mmatch[3];
			for (const char *mcur = icur + imatch[0].rm_so; mcur < xml + xml_len && mcur < iendcur
					&& regexec(&method_regex, mcur, ARRAY_SIZE(mmatch), mmatch, 0) == 0
					&& mmatch[2].rm_so >= 0 && mcur + mmatch[2].rm_so < iendcur; mcur += mmatch[0].rm_eo) {
				size_t method_len = mmatch[2].rm_eo - mmatch[2].rm_so;
				char method[iface_len+1+method_len+1];
				strcpy(method, iface);
				strcpy(method+iface_len, ".");
				strncpy(method+iface_len+1, mcur + mmatch[2].rm_so, method_len);
				method[iface_len+1+method_len] = '\0';

				void *pp = blobmsg_open_array(&ctx->retbuf, method);
				char *mendcur = strstr(mcur + mmatch[2].rm_eo, "</method>");
				if (!iendcur) {
					blobmsg_close_table(&ctx->retbuf, pp);
					continue;
				}

				regmatch_t amatch[3];
				for (const char *acur = mcur + mmatch[0].rm_so; acur < xml + xml_len && acur < mendcur
						&& regexec(&arg_regex, acur, ARRAY_SIZE(amatch), amatch, 0) == 0
						&& amatch[2].rm_so >= 0 && acur + amatch[2].rm_so < mendcur; acur += amatch[0].rm_eo) {
					size_t arg_len = amatch[2].rm_eo - amatch[2].rm_so;
					char arg[arg_len+1];
					strncpy(arg, acur + amatch[2].rm_so, arg_len);
					arg[arg_len] = '\0';

					regmatch_t atmatch[3];

					bool skip = false;
					char *arg_name = NULL, *arg_type = NULL;
					for (const char *atcur = acur + amatch[2].rm_so; atcur < acur + amatch[2].rm_eo
							&& regexec(&attr_regex, atcur, ARRAY_SIZE(atmatch), atmatch, 0) == 0
							&& atmatch[0].rm_eo >= 0 && atmatch[0].rm_eo < amatch[0].rm_eo; atcur += atmatch[0].rm_eo) {
						size_t atn_len = atmatch[1].rm_eo - atmatch[1].rm_so;
						size_t atv_len = atmatch[2].rm_eo - atmatch[2].rm_so;
						char atn[atn_len+1];
						char atv[atv_len+1];
						strncpy(atn, atcur + atmatch[1].rm_so, atn_len);
						strncpy(atv, atcur + atmatch[2].rm_so, atv_len);
						atn[atn_len] = '\0';
						atv[atv_len] = '\0';
						if (!strcmp("direction", atn) && !strcmp("out", atv)) {
							skip = true;
							break;
						}
						if (!strcmp("type", atn)) {
							arg_type = strdup(atv);
						} else if (!strcmp("name", atn)) {
							arg_name = strdup(atv);
						}
					}

					if (skip)
						continue;

					void *ppp = blobmsg_open_table(&ctx->retbuf, "");

					if (arg_name)
						blobmsg_add_string(&ctx->retbuf, "name", arg_name);
					if (arg_type)
						blobmsg_add_string(&ctx->retbuf, "type", arg_type);

					blobmsg_close_table(&ctx->retbuf, ppp);
				}
				blobmsg_close_array(&ctx->retbuf, pp);
			}
		}
	}

	blobmsg_close_table(&ctx->retbuf, p);

	if (dbus_message_iter_next(&ctx->arr_iter)) {
		introspect_list_next(ctx);
	} else {
		introspect_list_finish(ctx);
	}

	dbus_message_unref(reply);
}

static void wsd_call_ctx_free(void *f)
{
	struct wsd_call_ctx *ctx = f;
	blob_buf_free(&ctx->retbuf);
	free(ctx->list_args->src_blob);
	free(ctx->list_args);
	free(ctx->id);
	if (ctx->reply_slot >= 0)
		dbus_message_free_data_slot(&ctx->reply_slot);
	free(ctx);
}

static void wsd_list_cb(DBusPendingCall *call, void *data)
{
	struct wsd_call_ctx *ctx = data;

	blob_buf_init(&ctx->retbuf, 0);
	DBusMessage *reply = dbus_pending_call_steal_reply(call);
	assert(reply);
	char *response_str;

	int type = dbus_message_get_type(reply);
	if (type == DBUS_MESSAGE_TYPE_ERROR) {
		void *data_tkt = blobmsg_open_table(&ctx->retbuf, "data");
		blobmsg_add_string(&ctx->retbuf, "DBus", dbus_message_get_error_name(reply));
		blobmsg_close_table(&ctx->retbuf, data_tkt);
		response_str = jsonrpc__resp_error(ctx->id, JSONRPC_ERRORCODE__INTERNAL_ERROR, blobmsg_data(ctx->retbuf.head));
		goto out;
	}
	if (type != DBUS_MESSAGE_TYPE_METHOD_RETURN) {
		response_str = jsonrpc__resp_error(ctx->id, JSONRPC_ERRORCODE__INTERNAL_ERROR, NULL);
		goto out;
	}

	if (strcmp(dbus_message_get_signature(reply), "as")) {
		void *data_tkt = blobmsg_open_table(&ctx->retbuf, "data");
		blobmsg_add_string(&ctx->retbuf, "DBus", DBUS_ERROR_INVALID_SIGNATURE);
		blobmsg_close_table(&ctx->retbuf, data_tkt);
		response_str = jsonrpc__resp_error(ctx->id, JSONRPC_ERRORCODE__INTERNAL_ERROR, blobmsg_data(ctx->retbuf.head));
		goto out;
	}

	ctx->reply = reply;
	//dbus_message_ref(ctx->reply);
	dbus_message_allocate_data_slot(&ctx->reply_slot);
	dbus_message_set_data(ctx->reply, ctx->reply_slot, ctx, wsd_call_ctx_free);
	DBusMessageIter resp_iter;
	dbus_message_iter_init(reply, &resp_iter);

	dbus_message_iter_recurse(&resp_iter, &ctx->arr_iter);

	if (dbus_message_iter_get_arg_type(&ctx->arr_iter) != DBUS_TYPE_INVALID) {
		introspect_list_next(ctx);
	} else {
		introspect_list_finish(ctx);
	}

	return;

out:
	wsu_queue_write_str(ctx->wsi, response_str);
}

int ubusrpc_handle_dlist(struct lws *wsi, struct ubusrpc_blob *ubusrpc, struct blob_attr *id)
{
	struct prog_context *prog = lws_context_user(lws_get_context(wsi));

	DBusMessage *msg = dbus_message_new_method_call(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS, "ListNames");
	if (!msg) {
		return -1;
	}

	DBusPendingCall *call;
	if (!dbus_connection_send_with_reply(prog->dbus_ctx, msg, &call, 2000) || !call) {
		dbus_message_unref(msg);
		free(ubusrpc->list.src_blob);
		return -1;
	}

	struct wsd_call_ctx *ctx = calloc(1, sizeof *ctx);
	ctx->wsi = wsi;
	ctx->list_args = &ubusrpc->list;
	ctx->id = id ? blob_memdup(id) : NULL;
	ctx->reply_slot = -1;

	if (!dbus_pending_call_set_notify(call, wsd_list_cb, ctx, NULL)) {
		dbus_message_unref(msg);
		dbus_pending_call_unref(call);
		free(ubusrpc->list.src_blob);
		free(ctx->id);
		free(ctx);
		return -1;
	}

	dbus_message_unref(msg);
	dbus_pending_call_unref(call);
	return 0;
}
