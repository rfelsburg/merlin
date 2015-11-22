#include "steps_merlin.h"
#include <glib.h>
#include <stdlib.h>
#include <base/jsonx.h>

/* merlincat headers */
#include <merlincat/client_gsource.h>
#include <merlincat/server_gsource.h>
#include <merlincat/event_packer.h>
#include <merlincat/merlinreader.h>

/* merlin headers */
#include <shared/shared.h>
#include <shared/compat.h>

/* naemon / libnaemon headers */
#include <naemon/naemon.h>

typedef struct MerlinScenario_ {
	GTree *connections;
} MerlinScenario;

typedef struct MerlinScenarioConnection_ {
	ClientSource *cs;
	ServerSource *ss;

	MerlinReader *mr;
	ConnectionStorage *conn;

	/*
	 * Buffer of merlin_events, clear from the beginning, initialized by step
	 * "X starts recording"
	 */
	GPtrArray *event_buffer;
} MerlinScenarioConnection;

static struct kvvec *jsontbl_to_kvvec(JsonNode *tbl);

static MerlinScenarioConnection *mrlscenconn_new(ConnectionInfo *conn_info);
static void mrlscenconn_destroy(MerlinScenarioConnection *msc);
static void mrlscenconn_clear_buffer(MerlinScenarioConnection *msc);
static gboolean mrlscenconn_record_match(MerlinScenarioConnection *msc,
	const char *typestr, struct kvvec *matchkv);

static MerlinScenarioConnection *mrlscen_get_conn(MerlinScenario *ms,
	const gchar *tag);

static gpointer net_conn_new(ConnectionStorage *conn, gpointer user_data);
static void net_conn_data(ConnectionStorage *conn, gpointer buffer,
	gsize length, gpointer conn_user_data);
static void net_conn_close(gpointer conn_user_data);

STEP_BEGIN(step_begin_scenario);
STEP_END(step_end_scenario);

STEP_DEF(step_connect_tcp);
STEP_DEF(step_connect_unix);
STEP_DEF(step_listen_tcp);
STEP_DEF(step_listen_unix);
STEP_DEF(step_disconnect);

STEP_DEF(step_is_connected);
STEP_DEF(step_is_disconnected);

STEP_DEF(step_send_event);

STEP_DEF(step_clear_buffer);
STEP_DEF(step_record_check);
STEP_DEF(step_no_record_check);

CukeStepEnvironment steps_merlin =
	{
		.tag = "merlin",
		.begin_scenario = step_begin_scenario,
		.end_scenario = step_end_scenario,

		.definitions =
			{
				/* Connection handling */
				{ "^([a-z0-9-_]+) connect to merlin at port ([0-9]+)$",
					step_connect_tcp },
				{
					"^([a-z0-9-_]+) connect to merlin at port ([0-9]+) from port ([0-9]+)$",
					step_connect_tcp },
				{
					"^([a-z0-9-_]+) connect to merlin at socket (.+)$",
					step_connect_unix },
				{
					"^([a-z0-9-_]+) listens for merlin at port ([0-9]+)$",
					step_listen_tcp },
				{
					"^([a-z0-9-_]+) listens for merlin at socket (.+)$",
					step_listen_unix },
				{ "^([a-z0-9-_]+) disconnects from merlin$", step_disconnect },

				/* Connection verification */
				{ "^([a-z0-9-_]+) is connected to merlin$", step_is_connected },
				{
					"^([a-z0-9-_]+) is not connected to merlin$",
					step_is_disconnected },

				/* Send events */
				{ "^([a-z0-9-_]+) sends raw event ([A-Z_]+)$", step_send_event },

				/* Receive events */
				{ "^([a-z0-9-_]+) clears buffer$", step_clear_buffer },
				{ "^([a-z0-9-_]+) received event ([A-Z_]+)$", step_record_check },
				{
					"^([a-z0-9-_]+) should not receive ([A-Z_]+)$",
					step_no_record_check },

				{ NULL, NULL }
			}
	};

STEP_BEGIN(step_begin_scenario) {
	MerlinScenario *ms = g_malloc0(sizeof(MerlinScenario));
	/* Create a storage for all connections */
	ms->connections = g_tree_new_full((GCompareDataFunc) g_strcmp0, NULL,
		g_free, (GDestroyNotify) mrlscenconn_destroy);
	return ms;
}

STEP_END(step_end_scenario) {
	MerlinScenario *ms = (MerlinScenario*) scenario;
	/* Close all connections, and free storage */
	g_tree_destroy(ms->connections);
	g_free(ms);
}

STEP_DEF(step_connect_tcp) {
	MerlinScenario *ms = (MerlinScenario*) scenario;
	const char *conntag = NULL;
	long dport = 0;
	long sport = 0;
	MerlinScenarioConnection *msc;
	ConnectionInfo conn_info;

	if (!jsonx_locate(args, 'a', 0, 's', &conntag)
		|| !jsonx_locate(args, 'a', 1, 'l', &dport)) {
		STEP_FAIL("Invalid arguments");
		return;
	}
	if (!jsonx_locate(args, 'a', 2, 'l', &sport)) {
		/* This is valid, just not specified, take default  value */
		sport = 0;
	}

	conn_info.listen = 0;
	conn_info.type = TCP;
	conn_info.dest_addr = "127.0.0.1";
	conn_info.dest_port = dport;
	conn_info.source_addr = "0.0.0.0";
	conn_info.source_port = sport;
	msc = mrlscenconn_new(&conn_info);
	if (msc == NULL) {
		STEP_FAIL("Can not connect to merlin socket");
		return;
	}

	g_tree_insert(ms->connections, g_strdup(conntag), msc);

	STEP_OK;
}

STEP_DEF(step_connect_unix) {
	MerlinScenario *ms = (MerlinScenario*) scenario;
	const char *conntag = NULL;
	const char *sockpath = NULL;
	MerlinScenarioConnection *msc;
	ConnectionInfo conn_info;

	if (!jsonx_locate(args, 'a', 0, 's', &conntag)
		|| !jsonx_locate(args, 'a', 1, 's', &sockpath)) {
		STEP_FAIL("Invalid arguments");
		return;
	}

	conn_info.listen = 0;
	conn_info.type = UNIX;
	conn_info.dest_addr = g_strdup(sockpath);
	conn_info.dest_port = 0;
	conn_info.source_addr = "";
	conn_info.source_port = 0;

	msc = mrlscenconn_new(&conn_info);

	g_free(conn_info.dest_addr);
	if (msc == NULL) {
		STEP_FAIL("Can not connect to merlin socket");
		return;
	}

	g_tree_insert(ms->connections, g_strdup(conntag), msc);

	STEP_OK;
}

STEP_DEF(step_listen_tcp) {
	MerlinScenario *ms = (MerlinScenario*) scenario;
	const char *conntag = NULL;
	long dport = 0;
	MerlinScenarioConnection *msc;
	ConnectionInfo conn_info;

	if (!jsonx_locate(args, 'a', 0, 's', &conntag)
		|| !jsonx_locate(args, 'a', 1, 'l', &dport)) {
		STEP_FAIL("Invalid arguments");
		return;
	}

	conn_info.listen = 1;
	conn_info.type = TCP;
	conn_info.dest_addr = "127.0.0.1";
	conn_info.dest_port = dport;
	conn_info.source_addr = "0.0.0.0";
	conn_info.source_port = 0;
	msc = mrlscenconn_new(&conn_info);
	if (msc == NULL) {
		STEP_FAIL("Can not start listen to merlin socket");
		return;
	}

	g_tree_insert(ms->connections, g_strdup(conntag), msc);

	STEP_OK;
}

STEP_DEF(step_listen_unix) {
	MerlinScenario *ms = (MerlinScenario*) scenario;
	const char *conntag = NULL;
	const char *sockpath = NULL;
	MerlinScenarioConnection *msc;
	ConnectionInfo conn_info;

	if (!jsonx_locate(args, 'a', 0, 's', &conntag)
		|| !jsonx_locate(args, 'a', 1, 's', &sockpath)) {
		STEP_FAIL("Invalid arguments");
		return;
	}

	conn_info.listen = 1;
	conn_info.type = UNIX;
	conn_info.dest_addr = g_strdup(sockpath);
	conn_info.dest_port = 0;
	conn_info.source_addr = "";
	conn_info.source_port = 0;

	msc = mrlscenconn_new(&conn_info);

	g_free(conn_info.dest_addr);
	if (msc == NULL) {
		STEP_FAIL("Can not start listen to merlin socket");
		return;
	}

	g_tree_insert(ms->connections, g_strdup(conntag), msc);

	STEP_OK;
}

STEP_DEF(step_disconnect) {
	MerlinScenario *ms = (MerlinScenario*) scenario;
	const char *conntag = NULL;

	if (!jsonx_locate(args, 'a', 0, 's', &conntag)) {
		STEP_FAIL("Invalid arguments");
		return;
	}

	/* This frees up the connection and everything */
	if (!g_tree_remove(ms->connections, conntag)) {
		/* Fail if the connection was not found */
		STEP_FAIL("No active connection");
		return;
	}

	STEP_OK;
}

STEP_DEF(step_is_connected) {
	MerlinScenario *ms = (MerlinScenario*) scenario;
	const char *conntag = NULL;
	MerlinScenarioConnection *msc;

	if (!jsonx_locate(args, 'a', 0, 's', &conntag)) {
		STEP_FAIL("Invalid arguments");
		return;
	}

	msc = g_tree_lookup(ms->connections, conntag);
	if (msc == NULL) {
		/* If conntag isn't found, it's not connected */
		STEP_FAIL("Unknown connection reference");
		return;
	}
	if (msc->conn == NULL) {
		/* If connection isn't found, it's not connected */
		STEP_FAIL("Connection isn't found");
		return;
	}
	if(!connection_is_connected(msc->conn)) {
		STEP_FAIL("Not connected");
		return;
	}
	STEP_OK;
}

STEP_DEF(step_is_disconnected) {
	MerlinScenario *ms = (MerlinScenario*) scenario;
	const char *conntag = NULL;
	MerlinScenarioConnection *msc;

	if (!jsonx_locate(args, 'a', 0, 's', &conntag)) {
		STEP_FAIL("Invalid arguments");
		return;
	}

	msc = g_tree_lookup(ms->connections, conntag);
	if (msc == NULL) {
		/* If conntag isn't found, it's not connected */
		STEP_OK;
		return;
	}
	if (msc->conn == NULL) {
		/* If connection isn't found, it's not connected */
		STEP_OK;
		return;
	}
	if(connection_is_connected(msc->conn)) {
		/* Fail if connected */
		STEP_FAIL("Connected");
		return;
	}
	STEP_OK;
}

STEP_DEF(step_send_event) {
	MerlinScenario *ms = (MerlinScenario*) scenario;
	MerlinScenarioConnection *msc;
	const char *conntag = NULL;
	const char *typetag = NULL;
	JsonNode *tbl = NULL;
	JsonNode *row = NULL;
	merlin_event *evt = NULL;

	struct kvvec *kvv = NULL;

	if (!jsonx_locate(args, 'a', 0, 's', &conntag)
		|| !jsonx_locate(args, 'a', 1, 's', &typetag)
		|| !jsonx_locate(args, 'a', 2, 'j', &tbl)) {
		STEP_FAIL("Invalid arguments");
		return;
	}
	if (tbl->tag != JSON_ARRAY) {
		STEP_FAIL("Not a table argument");
		return;
	}

	msc = g_tree_lookup(ms->connections, conntag);
	if (msc == NULL) {
		/* If conntag isn't found, fail */
		STEP_FAIL("Unknown connection reference");
		return;
	}
	if (msc->conn == NULL) {
		/* If disconnected, fail */
		STEP_FAIL("Connection isn't found");
		return;
	}

	kvv = kvvec_create(30);
	if (kvv == NULL) {
		STEP_FAIL("Memory error, can't create kvvec");
		return;
	}

	json_foreach(row, tbl)
	{
		const char *key = NULL;
		const char *value = NULL;
		if (jsonx_locate(row, 'a', 0, 's', &key)
			&& jsonx_locate(row, 'a', 1, 's', &value)) {
			kvvec_addkv_str(kvv, strdup(key), strdup(value));
		}
	}

	evt = event_packer_unpack_kvv(typetag, kvv);
	if (!evt) {
		STEP_FAIL("Failed to pack message");
		kvvec_destroy(kvv, KVVEC_FREE_ALL);
		return;
	}

	g_message("Sending packet of type %s", typetag);
	connection_send(msc->conn, evt, HDR_SIZE + evt->hdr.len);

	free(evt);
	kvvec_destroy(kvv, KVVEC_FREE_ALL);

	STEP_OK;
}

STEP_DEF(step_clear_buffer) {
	MerlinScenario *ms = (MerlinScenario*) scenario;
	MerlinScenarioConnection *msc;
	const char *conntag = NULL;

	if (!jsonx_locate(args, 'a', 0, 's', &conntag)) {
		STEP_FAIL("Invalid arguments");
		return;
	}

	msc = g_tree_lookup(ms->connections, conntag);
	if (msc == NULL) {
		/* If conntag isn't found, fail */
		STEP_FAIL("Unknown connection reference");
		return;
	}

	mrlscenconn_clear_buffer(msc);

	STEP_OK;
}

STEP_DEF(step_record_check) {
	MerlinScenario *ms = (MerlinScenario*) scenario;
	MerlinScenarioConnection *msc;
	const char *conntag = NULL;
	const char *typetag = NULL;
	JsonNode *tbl = NULL;
	gint res = 0;
	struct kvvec *kvv = NULL;

	if (!jsonx_locate(args, 'a', 0, 's', &conntag)
		|| !jsonx_locate(args, 'a', 1, 's', &typetag)) {
		STEP_FAIL("Invalid arguments");
		return;
	}

	/* It's ok not to have a table, just keep it to NULL */
	if (!jsonx_locate(args, 'a', 2, 'j', &tbl)) {
		tbl = NULL;
	}

	msc = mrlscen_get_conn(ms, conntag);
	if(msc == NULL) {
		STEP_FAIL("Unknown connection reference");
		return;
	}

	kvv = jsontbl_to_kvvec(tbl);
	res = mrlscenconn_record_match(msc, typetag, kvv) ? 1 : 0;
	kvvec_destroy(kvv, KVVEC_FREE_ALL);

	if(res) {
		STEP_OK;
	} else {
		STEP_FAIL("No matching entries");
	}
}

STEP_DEF(step_no_record_check) {
	MerlinScenario *ms = (MerlinScenario*) scenario;
	MerlinScenarioConnection *msc;
	const char *conntag = NULL;
	const char *typetag = NULL;
	JsonNode *tbl = NULL;
	gint res = 0;
	struct kvvec *kvv = NULL;

	if (!jsonx_locate(args, 'a', 0, 's', &conntag)
		|| !jsonx_locate(args, 'a', 1, 's', &typetag)) {
		STEP_FAIL("Invalid arguments");
		return;
	}

	/* It's ok not to have a table, just keep it to NULL */
	if (!jsonx_locate(args, 'a', 2, 'j', &tbl)) {
		tbl = NULL;
	}

	msc = mrlscen_get_conn(ms, conntag);
	if(msc == NULL) {
		STEP_FAIL("Unknown connection reference");
		return;
	}

	kvv = jsontbl_to_kvvec(tbl);
	res = mrlscenconn_record_match(msc, typetag, kvv) ? 1 : 0;
	kvvec_destroy(kvv, KVVEC_FREE_ALL);

	if(res) {
		STEP_FAIL("Entries matched");
	} else {
		STEP_OK;
	}
}

/**
 * Build a kvvec out of a json table, if possible. Return empty kvvec otherwise
 */
static struct kvvec *jsontbl_to_kvvec(JsonNode *tbl) {
	struct kvvec *kvv;
	JsonNode *row = NULL;

	kvv = kvvec_create(30);
	if (kvv == NULL) {
		/* If disconnected, fail */
		return NULL;
	}

	if (tbl == NULL || tbl->tag != JSON_ARRAY) {
		return kvv;
	}

	json_foreach(row, tbl)
	{
		const char *key = NULL;
		const char *value = NULL;
		if (jsonx_locate(row, 'a', 0, 's', &key)
			&& jsonx_locate(row, 'a', 1, 's', &value)) {
			kvvec_addkv_str(kvv, strdup(key), strdup(value));
		}
	}

	return kvv;
}

static MerlinScenarioConnection *mrlscen_get_conn(MerlinScenario *ms,
	const gchar *tag) {
	MerlinScenarioConnection *msc;
	msc = g_tree_lookup(ms->connections, tag);
	if (msc == NULL) {
		/* If conntag isn't found, fail */
		return NULL;
	}
	if (msc->conn == NULL) {
		/* If disconnected, fail */
		return NULL;
	}
	return msc;
}

/**
 * Create a TCP connection, and return a storage for that connection.
 *
 * It's ok to add handlers to the main context, to update the state during
 * runtime, as long as everything is freed during mrlscenconn_destroy
 */
static MerlinScenarioConnection *mrlscenconn_new(ConnectionInfo *conn_info) {
	MerlinScenarioConnection *msc = g_malloc0(sizeof(MerlinScenarioConnection));

	msc->event_buffer = g_ptr_array_new_with_free_func(g_free);

	if(conn_info->listen) {
		msc->ss = server_source_new(conn_info, net_conn_new, net_conn_data,
			net_conn_close, msc);
		if(msc->ss == NULL)
			goto fail_out;
	} else {
		msc->cs = client_source_new(conn_info, net_conn_new, net_conn_data,
			net_conn_close, msc);
		if(msc->cs == NULL)
			goto fail_out;
	}

	return msc;

	fail_out: /**/
	mrlscenconn_destroy(msc);
	return NULL;
}
static void mrlscenconn_destroy(MerlinScenarioConnection *msc) {
	if (msc == NULL)
		return;
	if (msc->event_buffer != NULL) {
		g_ptr_array_unref(msc->event_buffer);
	}
	client_source_destroy(msc->cs);
	server_source_destroy(msc->ss);
	g_free(msc);
}
static void mrlscenconn_clear_buffer(MerlinScenarioConnection *msc) {
	g_ptr_array_set_size(msc->event_buffer, 0);
}
static gboolean mrlscenconn_record_match(MerlinScenarioConnection *msc,
	const char *typestr, struct kvvec *matchkv) {
	gint type, i, count;
	merlin_event *evt;

	if (msc->event_buffer == NULL) {
		g_message("No recording active");
		return FALSE;
	}

	type = event_packer_str_to_type(typestr);
	count = 0;
	for (i = 0; i < msc->event_buffer->len; i++) {
		evt = msc->event_buffer->pdata[i];
		if (evt->hdr.type == type) {
			struct kvvec *evtkv = event_packer_pack_kvv(evt, NULL);
			int evt_i, match_i, misses;
			misses = 0;
			for (match_i = 0; match_i < matchkv->kv_pairs; match_i++) {
				int found = 0;
				for (evt_i = 0; evt_i < evtkv->kv_pairs; evt_i++) {
					if (0 == strcmp(evtkv->kv[evt_i].key,
						matchkv->kv[match_i].key)) {
						found = 1;
						if (0 != strcmp(evtkv->kv[evt_i].value,
							matchkv->kv[match_i].value)) {
							/* Key matches, but not value = miss */
							misses++;
						}
					}
				}
				if (!found) {
					/* If we search for a non-existing key, it's a miss */
					misses++;
				}
			}
			if (misses == 0) {
				count++;
			}
			kvvec_destroy(evtkv, KVVEC_FREE_ALL);
		}
	}
	return count > 0;
}

static gpointer net_conn_new(ConnectionStorage *conn, gpointer user_data) {
	MerlinScenarioConnection *msc = (MerlinScenarioConnection *) user_data;
	if(msc->conn != NULL) {
		/* If we already have a connection, we can't take a new one */
		/* TODO: make it possible to disconnect/reject connection */
		return NULL;
	}
	msc->mr = merlinreader_new();
	msc->conn = conn;
	return msc;
}
static void net_conn_data(ConnectionStorage *conn, gpointer buffer,
	gsize length, gpointer conn_user_data) {
	MerlinScenarioConnection *msc = (MerlinScenarioConnection *) conn_user_data;
	merlin_event *evt;
	gsize read_size;

	if(msc == NULL) {
		/* It's a connection we can't handle, just ignore */
		return;
	}

	while (length) {
		read_size = merlinreader_add_data(msc->mr, buffer, length);
		length -= read_size;
		buffer += read_size;

		while (NULL != (evt = merlinreader_get_event(msc->mr))) {
			if (msc->event_buffer == NULL) {
				g_free(evt);
			} else {
				g_ptr_array_add(msc->event_buffer, evt);
			}
		}
	}
}
static void net_conn_close(gpointer conn_user_data) {
	MerlinScenarioConnection *msc = (MerlinScenarioConnection *) conn_user_data;
	if (msc == NULL)
		return;
	merlinreader_destroy(msc->mr);
	msc->mr = NULL;
	msc->conn = NULL;
}
