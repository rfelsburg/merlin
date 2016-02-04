#include <signal.h>
#include <stdarg.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include "daemonize.h"
#include "daemon.h"
#include "db_updater.h"
#include "config.h"
#include "logging.h"
#include "ipc.h"
#include "configuration.h"
#include "net.h"
#include "sql.h"
#include "state.h"
#include "shared.h"

static const char *progname;
static const char *pidfile, *merlin_user;
static char *import_program;
unsigned short default_port = 15551;
unsigned int default_addr = 0;
static int importer_pid;
static merlin_confsync csync;
static int num_children;
static int killing;
static int user_sig;
int db_log_reports = 1;
int db_log_notifications = 1;
static merlin_nodeinfo merlind;
static int merlind_sig;

static void usage(char *fmt, ...)
	__attribute__((format(printf,1,2)));

static void usage(char *fmt, ...)
{
	if (fmt) {
		va_list ap;

		va_start(ap, fmt);
		vprintf(fmt, ap);
		va_end(ap);
		putchar('\n');
	}

	printf("Usage: %s -c <config-file> [-d] [-k] [-s] [-h]\n"
		"\t-c|--config   Specify the configuration file name. Unknown, non-flag\n"
		"\t              arguments might also be interprented as the config file.\n"
		"\t-d|--debug    Enter \"debug\" mode - this just means it won't daemonize.\n"
		"\t-s            Don't start. Instead, print if merlin is already running.\n"
		"\t-k|--kill     Don't start. Instead, find a running instance and kill it.\n"
		"\t-h|--help     Print this help text.\n"
		, progname);

	exit(1);
}

/* node connect/disconnect handlers */
static int node_action_handler(merlin_node *node, int prev_state)
{
	switch (node->state) {
	case STATE_PENDING:
	case STATE_NEGOTIATING:
	case STATE_NONE:
		node_disconnect(node, "%s disconnected", node->name);

		/* only send INACTIVE if we haven't already */
		if (prev_state == STATE_CONNECTED) {
			ldebug("Sending IPC control INACTIVE for '%s'", node->name);
			return ipc_send_ctrl(CTRL_INACTIVE, node->id);
		}
	}

	return 1;
}

static int ipc_action_handler(merlin_node *node, int prev_state)
{
	uint i;

	switch (node->state) {
	case STATE_CONNECTED:
		break;

	case STATE_PENDING:
	case STATE_NEGOTIATING:
	case STATE_NONE:
		/* if ipc wasn't connected before, we return early */
		if (prev_state != STATE_CONNECTED)
			return 0;

		/* also tell our peers and masters */
		for (i = 0; i < num_masters + num_peers; i++) {
			merlin_node *n = node_table[i];
			node_send_ctrl_inactive(n, CTRL_GENERIC);
		}
	}

	return 0;
}

static void grok_daemon_compound(struct cfg_comp *comp)
{
	uint i;

	for (i = 0; i < comp->vars; i++) {
		struct cfg_var *v = comp->vlist[i];

		if (!strcmp(v->key, "port")) {
			char *endp;

			default_port = (unsigned short)strtoul(v->value, &endp, 0);
			if (default_port < 1 || *endp)
				cfg_error(comp, v, "Illegal value for port: %s", v->value);
			continue;
		}
		if (!strcmp(v->key, "address")) {
			unsigned int addr;
			if (inet_pton(AF_INET, v->value, &addr) == 1)
				default_addr = addr;
			else
				cfg_error(comp, v, "Illegal value for address: %s", v->value);
			continue;
		}
		if (!strcmp(v->key, "pidfile")) {
			pidfile = strdup(v->value);
			continue;
		}
		if (!strcmp(v->key, "merlin_user")) {
			merlin_user = strdup(v->value);
			continue;
		}
		if (!strcmp(v->key, "import_program")) {
			import_program = strdup(v->value);
			continue;
		}

		if (grok_common_var(comp, v))
			continue;
		if (log_grok_var(v->key, v->value))
			continue;

		cfg_error(comp, v, "Unknown variable");
	}

	for (i = 0; i < comp->nested; i++) {
		struct cfg_comp *c = comp->nest[i];
		uint vi;

		if (!prefixcmp(c->name, "database")) {
			use_database = 1;
			for (vi = 0; vi < c->vars; vi++) {
				struct cfg_var *v = c->vlist[vi];
				if (!strcmp(v->key, "log_report_data")) {
					db_log_reports = strtobool(v->value);
				} else if (!prefixcmp(v->key, "log_notification")) {
					db_log_notifications = strtobool(v->value);
				} else if (!prefixcmp(v->key, "track_current")) {
					cfg_warn(c, v, "'%s' has been removed", v->key);
				} else if (!strcmp(v->key, "enabled")) {
					use_database = strtobool(v->value);
				} else {
					sql_config(v->key, v->value);
				}
			}
			continue;
		}
		if (!strcmp(c->name, "object_config")) {
			grok_confsync_compound(c, &csync);
			continue;
		}
	}
}

/* daemon-specific node manipulation */
static void post_process_nodes(void)
{
	uint i, x;

	ldebug("post processing %d masters, %d pollers, %d peers",
	       num_masters, num_pollers, num_peers);

	for (i = 0; i < num_nodes; i++) {
		merlin_node *node = node_table[i];

		if (!node) {
			lerr("node is null. i is %d. num_nodes is %d. wtf?", i, num_nodes);
			continue;
		}

		if (!node->csync.configured && csync.push.cmd) {
			if (asprintf(&node->csync.push.cmd, "%s %s", csync.push.cmd, node->name) < 0)
				lerr("CSYNC: Failed to add per-node confsync command for %s", node->name);
			else
				ldebug("CSYNC: Adding per-node sync to %s as: %s\n", node->name, node->csync.push.cmd);
		}

		if (!node->sain.sin_port)
			node->sain.sin_port = htons(default_port);

		node->action = node_action_handler;

		node->bq = nm_bufferqueue_create();
		if (node->bq == NULL) {
			lerr("Failed to create io cache for node %s. Aborting", node->name);
		}

		/*
		 * this lets us support multiple merlin instances on
		 * a single system, but all instances on the same
		 * system will be marked at the same time, so we skip
		 * them on the second pass here.
		 */
		if (node->flags & MERLIN_NODE_FIXED_SRCPORT) {
			continue;
		}

		if (node->sain.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
			node->flags |= MERLIN_NODE_FIXED_SRCPORT;
			ldebug("Using fixed source-port for local %s node %s",
				   node_type(node), node->name);
			continue;
		}
		for (x = i + 1; x < num_nodes; x++) {
			merlin_node *nx = node_table[x];
			if (node->sain.sin_addr.s_addr == nx->sain.sin_addr.s_addr) {
				ldebug("Using fixed source-port for %s node %s",
				       node_type(node), node->name);
				ldebug("Using fixed source-port for %s node %s",
				       node_type(nx), nx->name);
				node->flags |= MERLIN_NODE_FIXED_SRCPORT;
				nx->flags |= MERLIN_NODE_FIXED_SRCPORT;

				if (node->sain.sin_port == nx->sain.sin_port) {
					lwarn("Nodes %s and %s have same ip *and* same port. Voodoo?",
					      node->name, nx->name);
				}
			}
		}
	}
}

static int grok_config(char *path)
{
	uint i;
	struct cfg_comp *config;

	if (!path)
		return 0;

	config = cfg_parse_file(path);
	if (!config)
		return 0;

	for (i = 0; i < config->vars; i++) {
		struct cfg_var *v = config->vlist[i];

		if (!v->value)
			cfg_error(config, v, "No value for option '%s'", v->key);

		if (grok_common_var(config, v))
			continue;

		if (!strcmp(v->key, "port")) {
			default_port = (unsigned short)strtoul(v->value, NULL, 0);
			continue;
		}

		cfg_warn(config, v, "Unrecognized variable\n");
	}

	for (i = 0; i < config->nested; i++) {
		struct cfg_comp *c = config->nest[i];

		if (!prefixcmp(c->name, "daemon")) {
			grok_daemon_compound(c);
			continue;
		}
	}

	/*
	 * if we're supposed to kill a running daemon, ignore
	 * parsing and post-processing nodes. We avoid memory
	 * fragmentation by releasing the config memory before
	 * allocating memory for the nodes.
	 */
	if (!killing) {
		node_grok_config(config);
	}
	cfg_destroy_compound(config);
	if (!killing) {
		post_process_nodes();
	}

	return 1;
}

/*
 * if the import isn't done yet waitpid() will return 0
 * and we won't touch importer_pid at all.
 */
static void reap_child_process(void)
{
	int status, pid;
	unsigned int i;
	char *name = NULL;
	char *cmd_to_try = NULL;

	if (!num_children)
		return;

	pid = waitpid(-1, &status, WNOHANG);
	if (pid < 0) {
		if (errno == ECHILD) {
			/* no child running. Just reset */
			num_children = importer_pid = 0;
		} else {
			/* some random error. log it */
			lerr("waitpid(-1...) failed: %s", strerror(errno));
		}

		return;
	}

	/* child may not be done yet */
	if (!pid)
		return;

	/* we reaped an actual child, so decrement the counter */
	num_children--;

	/*
	 * looks like we reaped some helper we spawned,
	 * so let's figure out what to call it when we log
	 */
	linfo("Child with pid %d successfully reaped", pid);
	if (pid == importer_pid) {
		name = strdup("import program");
		importer_pid = 0;
		ipc_send_ctrl(CTRL_RESUME, CTRL_GENERIC);
	} else {
		/* not the importer program, so it must be an oconf push or fetch */
		for (i = 0; i < num_nodes; i++) {
			merlin_node *node = node_table[i];

			if (pid == node->csync.push.pid) {
				linfo("CSYNC: push finished for %s", node->name);
				node->csync.push.pid = 0;
				asprintf(&name, "CSYNC: oconf push to %s node %s", node_type(node), node->name);
				asprintf(&cmd_to_try, "mon oconf push %s", node->name);
				break;
			} else if (pid == node->csync.fetch.pid) {
				linfo("CSYNC: fetch finished from %s", node->name);
				node->csync.fetch.pid = 0;
				asprintf(&name, "CSYNC: oconf fetch from %s node %s", node_type(node), node->name);
				break;
			}
		}
	}

	if (WIFEXITED(status)) {
		if (!WEXITSTATUS(status)) {
			linfo("%s finished successfully", name);
		} else {
			lwarn("%s exited with return code %d", name, WEXITSTATUS(status));
			if (cmd_to_try) {
				lwarn("CSYNC: Try manually running '%s' (without quotes) as the monitor user", cmd_to_try);
			}
		}
	} else {
		if (WIFSIGNALED(status)) {
			lerr("%s was terminated by signal %d. %s core dump was produced",
			     name, WTERMSIG(status), WCOREDUMP(status) ? "A" : "No");
		} else {
			lerr("%s was shut down by an unknown source", name);
		}
	}

	free(name);
	free(cmd_to_try);
}

/*
 * Run a program, stashing the child pid in *pid.
 * Since it's not supposed to run all that often, we don't care a
 * whole lot about performance and lazily run all commands through
 * /bin/sh for argument handling
 */
static void run_program(char *what, char *cmd, int *prog_id)
{
	char *args[4] = { "sh", "-c", cmd, NULL };
	int pid;

	ldebug("Executing %s command '%s'", what, cmd);
	pid = fork();
	if (!pid) {
		/*
		 * child runs the command. if execvp() returns, that means it
		 * failed horribly and that we're basically screwed
		 */
		execv("/bin/sh", args);
		lerr("execv() failed: %s", strerror(errno));
		exit(1);
	}
	if (pid < 0) {
		lerr("Skipping %s due to failed fork(): %s", what, strerror(errno));
		return;
	}
	/*
	 * everything went ok, so update prog_id if passed
	 * and increment num_children
	 */
	if (prog_id)
		*prog_id = pid;
	num_children++;
}

/*
 * import objects and status from objects.cache and status.log,
 * respecively
 */
static int import_objects_and_status(char *cfg, char *cache)
{
	char *cmd;
	int result = 0;

	/* don't bother if we're not using a datbase */
	if (!use_database)
		return 0;

	/* ... or if an import is already in progress */
	if (importer_pid) {
		lwarn("Import already in progress. Ignoring import event");
		return 0;
	}

	if (!import_program) {
		lerr("No import program specified. Ignoring import event");
		return 0;
	}

	asprintf(&cmd, "%s --nagios-cfg='%s' "
			 "--db-type='%s' --db-name='%s' --db-user='%s' --db-pass='%s' --db-host='%s' --db-conn_str='%s'",
			 import_program, cfg,
			 sql_db_type(), sql_db_name(), sql_db_user(), sql_db_pass(), sql_db_host(), sql_db_conn_str());
	if (cache && *cache) {
		char *cmd2 = cmd;
		asprintf(&cmd, "%s --cache='%s'", cmd2, cache);
		free(cmd2);
	}

	if (sql_db_port()) {
		char *cmd2 = cmd;
		asprintf(&cmd, "%s --db-port='%u'", cmd2, sql_db_port());
		free(cmd2);
	}

	run_program("import", cmd, &importer_pid);
	free(cmd);

	return result;
}

/* nagios.cfg, objects.cache (optional) and status.log (optional) */
static int read_nagios_paths(merlin_event *pkt)
{
	char *nagios_paths_arena;
	char *npath[3] = { NULL, NULL, NULL };
	uint i;
	size_t offset = 0;

	if (!use_database)
		return 0;

	nagios_paths_arena = malloc(pkt->hdr.len);
	if (!nagios_paths_arena)
		return -1;
	memcpy(nagios_paths_arena, pkt->body, pkt->hdr.len);

	for (i = 0; i < ARRAY_SIZE(npath) && offset < pkt->hdr.len; i++) {
		npath[i] = nagios_paths_arena + offset;
		offset += strlen(npath[i]) + 1;
	}

	import_objects_and_status(npath[0], npath[1]);
	free(nagios_paths_arena);

	return 0;
}

/*
 * Compares *node's info struct and returns:
 * 0 if node's config is same as ours (we should do nothing)
 * > 0 if node's config is newer than ours (we should fetch)
 * < 0 if node's config is older than ours (we should push)
 *
 * If hashes don't match but config is exactly the same
 * age, we instead return:
 * > 0 if node started after us (we should fetch)
 * < 0 if node started before us (we should push)
 *
 * If all of the above are identical, we return the hash delta.
 * This should only happen rarely, but it will ensure that not
 * both sides try to fetch or push at the same time.
 */
static int csync_config_cmp(merlin_node *node, int *was_error)
{
	int mtime_delta;
	*was_error =0;

	ldebug("CSYNC: %s: Comparing config", node->name);
	if (!ipc.info.last_cfg_change) {
		/*
		 * if our module is inactive, we can't know anything so we
		 * can't do anything, and we can't fetch the last config
		 * change time, since it might be being changed as we speak.
		 */
		ldebug("CSYNC: %s: Our module is inactive, so can't check", node->name);
		*was_error = 1;
		return 0;
	}

	/*
	 * All peers must have identical configuration
	 */
	if (node->type == MODE_PEER) {
		int hash_delta;
		hash_delta = memcmp(node->info.config_hash, ipc.info.config_hash, 20);
		if (!hash_delta) {
			ldebug("CSYNC: %s: hashes match. No sync required", node->name);
			return 0;
		}
		*was_error = 1;
	}

	/* For non-peers, we simply move on from here. */
	mtime_delta = node->info.last_cfg_change - ipc.info.last_cfg_change;
	if (mtime_delta) {
		ldebug("CSYNC: %s: mtime_delta (%lu - %lu): %d", node->name,
			node->info.last_cfg_change, ipc.info.last_cfg_change, mtime_delta);
		return mtime_delta;
	}

	/*
	 * Error path. This node is a peer, but we have a hash mismatch
	 * and matching mtimes. Unusual, to say the least. Either way,
	 * we can't really do anything except warn about it and get
	 * on with things. This will only happen when someone manages
	 * to save the config exactly the same second on both nodes.
	 */
	lerr("CSYNC: %s: Can't determine confsync action", node->name);
	lerr("CSYNC: %s: hash mismatch but mtime matches", node->name);
	lerr("CSYNC: %s: User intervention required.", node->name);

	*was_error = 1;
	return 0;
}

/*
 * executed when a node comes online and reports itself as
 * being active. This is where we run the configuration sync
 * if any is configured
 *
 * Note that the 'push' and 'fetch' options in the configuration
 * are simply guidance names. One could configure them in reverse
 * if one wanted, or make them boil noodles for the IT staff or
 * paint a skateboard blue for all Merlin cares. It will just
 * assume that things work out just fine so long as the config
 * is (somewhat) in sync.
 */
void csync_node_active(merlin_node *node)
{
	time_t now;
	int val = 0, error = 0;
	merlin_confsync *cs = NULL;
	merlin_child *child = NULL;

	ldebug("CSYNC: %s: Checking...", node->name);
	/* bail early if we have no push/fetch configuration */
	cs = &node->csync;
	if (!cs->push.cmd && !cs->fetch.cmd) {
		ldebug("CSYNC: %s: No config sync configured.", node->name);
		node_disconnect(node, "Disconnecting from %s, as config can't be synced", node->name);
		return;
	}

	val = csync_config_cmp(node, &error);
	if (val || error)
		node_disconnect(node, "Disconnecting from %s, as config is out of sync", node->name);

	if (!val)
		return;

	if (cs == &csync && !(node->flags & MERLIN_NODE_CONNECT)) {
		ldebug("CSYNC: %s node %s configured with 'connect = no'. Avoiding global push",
			   node_type(node), node->name);
		return;
	}

	if (node->type == MODE_MASTER) {
		if (cs->fetch.cmd && strcmp(cs->fetch.cmd, "no")) {
			child = &cs->fetch;
			ldebug("CSYNC: We'll try to fetch");
		} else {
			ldebug("CSYNC: Refusing to run global sync to a master node");
		}
	} else if (node->type == MODE_POLLER) {
		if (cs->push.cmd && strcmp(cs->push.cmd, "no")) {
			child = &cs->push;
			ldebug("CSYNC: We'll try to push");
		} else {
			ldebug("CSYNC: Should have pushed, but push not configured for %s", node->name);
		}
	} else {
		if (val < 0) {
			if (cs->push.cmd && strcmp(cs->push.cmd, "no")) {
				child = &cs->push;
				ldebug("CSYNC: We'll try to push");
			} else {
				ldebug("CSYNC: Should have pushed, but push not configured for %s", node->name);
			}
		} else if (val > 0) {
			if (cs->fetch.cmd && strcmp(cs->fetch.cmd, "no")) {
				child = &cs->fetch;
				ldebug("CSYNC: We'll try to fetch");
			} else {
				ldebug("CSYNC: Should have fetched, but fetch not configured for %s", node->name);
			}
		}
	}

	if (!child) {
		ldebug("CSYNC: No action required for %s", node->name);
		return;
	}

	if (child->pid) {
		ldebug("CSYNC: '%s' already running for %s, or globally", child->cmd, node->name);
		return;
	}

	now = time(NULL);
	if (node->csync_last_attempt >= now - 30) {
		ldebug("CSYNC: Config sync attempted %lu seconds ago. Waiting at least %lu seconds",
		       now - node->csync_last_attempt, 30 - (now - node->csync_last_attempt));
		return;
	}

	node->csync_num_attempts++;
	linfo("CSYNC: triggered against %s node %s; val: %d; command: [%s]",
	      node_type(node), node->name, val, child->cmd);
	node->csync_last_attempt = now;
	run_program("csync", child->cmd, &child->pid);
	if (child->pid > 0) {
		ldebug("CSYNC: command has pid %d", child->pid);
	} else {
		child->pid = 0;
	}
}


static int handle_ipc_event(merlin_event *pkt)
{
	int result = 0;

	if (pkt->hdr.type == CTRL_PACKET) {
		switch (pkt->hdr.code) {
		case CTRL_PATHS:
			read_nagios_paths(pkt);
			return 0;

		case CTRL_ACTIVE:
			result = handle_ctrl_active(&ipc, pkt);
			/*
			 * both ESYNC_ENODES and ESYNC_ECONFTIME are fine from
			 * IPC, but means we need to make sure all other nodes
			 * are disconnected before continuing
			 */
			if (result == ESYNC_ENODES || result == ESYNC_ECONFTIME) {
				unsigned int i;
				result = 0;
				for (i = 0; i < num_nodes; i++) {
					node_disconnect(node_table[i], "Local config changed, node must reconnect with new config.");
				}
			} else if (result < 0) {
				/* ipc is incompatible with us. weird */
				return 0;
			}
			node_set_state(&ipc, STATE_CONNECTED, "Connected");
			break;

		case CTRL_INACTIVE:
			/* this should really never happen, but forward it if it does */
			memset(&ipc.info, 0, sizeof(ipc.info));
			break;
		default:
			lwarn("forwarding control packet %d to the network",
				  pkt->hdr.code);
			break;
		}
	}

	/*
	 * we must send to the network before we run mrm_db_update(),
	 * since the latter deblockifies the packet and makes it
	 * unusable in network transfers without repacking, but only
	 * if this isn't magically marked as a NONET event
	 */
	if (pkt->hdr.code != MAGIC_NONET)
		result = net_send_ipc_data(pkt);

	/* skip sending control packets to database */
	if (use_database && pkt->hdr.type != CTRL_PACKET)
		result |= mrm_db_update(&ipc, pkt);

	return result;
}

static int ipc_reap_events(void)
{
	int len, events = 0;
	merlin_event *pkt;

	node_log_event_count(&ipc, 0);

	len = node_recv(&ipc);
	if (len < 0)
		return len;

	while ((pkt = node_get_event(&ipc))) {
		events++;
		handle_ipc_event(pkt);
		free(pkt);
	}

	return 0;
}

static int io_poll_sockets(void)
{
	fd_set rd, wr;
	int sel_val, ipc_listen_sock, nfound;
	int sockets = 0;
	struct timeval tv = { 2, 0 };
	static time_t last_ipc_reinit = 0;

	/*
	 * Try re-initializing ipc if the module isn't connected
	 * and it was a while since we tried it.
	 */
	if (ipc.sock < 0 && last_ipc_reinit + 5 < time(NULL)) {
		ipc_reinit();
		last_ipc_reinit = time(NULL);
	}

	ipc_listen_sock = ipc_listen_sock_desc();
	sel_val = max(ipc.sock, ipc_listen_sock);

	FD_ZERO(&rd);
	FD_ZERO(&wr);
	if (ipc.sock >= 0)
		FD_SET(ipc.sock, &rd);
	if (ipc_listen_sock >= 0)
		FD_SET(ipc_listen_sock, &rd);

	sel_val = net_polling_helper(&rd, &wr, sel_val);
	if (sel_val < 0)
		return 0;

	nfound = select(sel_val + 1, &rd, &wr, NULL, &tv);
	if (nfound < 0) {
		lerr("select() returned %d (errno = %d): %s", nfound, errno, strerror(errno));
		sleep(1);
		return -1;
	}

	if (ipc_listen_sock > 0 && FD_ISSET(ipc_listen_sock, &rd)) {
		linfo("Accepting inbound connection on ipc socket");
		ipc_accept();
	} else if (ipc.sock > 0 && FD_ISSET(ipc.sock, &rd)) {
		sockets++;
		ipc_reap_events();
	}

	sockets += net_handle_polling_results(&rd, &wr);

	return 0;
}

static void dump_daemon_nodes(void)
{
	int fd;
	unsigned int i;

	user_sig &= ~(1 << SIGUSR1);

	fd = open("/tmp/merlind.nodeinfo", O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if (fd < 0) {
		lerr("USERSIG: Failed to open /tmp/merlind.nodeinfo for dumping: %s", strerror(errno));
		return;
	}

	dump_nodeinfo(&ipc, fd, 0);
	for (i = 0; i < num_nodes; i++)
		dump_nodeinfo(node_table[i], fd, i + 1);
}

static void polling_loop(void)
{
	for (;!merlind_sig;) {
		uint i;
		time_t now = time(NULL);

		if (user_sig & (1 << SIGUSR1))
			dump_daemon_nodes();

		/*
		 * log the event count. The marker to prevent us from
		 * spamming the logs is in log_event_count() in logging.c
		 */
		ipc_log_event_count();

		/* reap any children that might have finished */
		reap_child_process();

		/* When the module is disconnected, we can't validate handshakes,
		 * so any negotiation would need to be redone after the module
		 * has started. Don't even bother.
		 */
		if (ipc.state == STATE_CONNECTED) {
			while (!merlind_sig && net_accept_one() >= 0)
				; /* nothing */

			for (i = 0; !merlind_sig && i < num_nodes; i++) {
				merlin_node *node = node_table[i];
				/* try connecting if we're not already */
				if (!net_is_connected(node) && node->state == STATE_NONE) {
					net_try_connect(node);
				}
			}
		}

		if (merlind_sig)
			return;

		/*
		 * io_poll_sockets() is the real worker. It handles network
		 * and ipc based IO and ships inbound events off to their
		 * right destination.
		 */
		io_poll_sockets();

		if (merlind_sig)
			return;

		/*
		 * Try to commit any outstanding queries
		 */
		sql_try_commit(0);
	}
}


static void clean_exit(int sig)
{
	if (sig) {
		lwarn("Caught signal %d. Shutting down", sig);
	}

	ipc_deinit();
	sql_close();
	net_deinit();
	log_deinit();
	daemon_shutdown();

	if (!sig || sig == SIGINT || sig == SIGTERM)
		exit(EXIT_SUCCESS);
	exit(EXIT_FAILURE);
}

static void merlind_sighandler(int sig)
{
	merlind_sig = sig;
}

static void sigusr_handler(int sig)
{
	user_sig |= 1 << sig;
}

int merlind_main(int argc, char **argv)
{
	int i, result, status = 0;

	progname = strrchr(argv[0], '/');
	progname = progname ? progname + 1 : argv[0];

	is_module = 0;
	self = &merlind;
	ipc_init_struct();
	gettimeofday(&merlind.start, NULL);

	/*
	 * Solaris doesn't support MSG_NOSIGNAL, so
	 * we ignore SIGPIPE globally instead
	 */
	signal(SIGPIPE, SIG_IGN);

	for (i = 1; i < argc; i++) {
		char *opt, *arg = argv[i];

		if (*arg != '-') {
			if (!merlin_config_file) {
				merlin_config_file = arg;
				continue;
			}
			goto unknown_argument;
		}

		if (!strcmp(arg, "-h") || !strcmp(arg, "--help"))
			usage(NULL);
		if (!strcmp(arg, "-k") || !strcmp(arg, "--kill")) {
			killing = 1;
			continue;
		}
		if (!strcmp(arg, "-d") || !strcmp(arg, "--debug")) {
			debug++;
			continue;
		}
		if (!strcmp(arg, "-s")) {
			status = 1;
			continue;
		}

		if ((opt = strchr(arg, '=')))
			opt++;
		else if (i < argc - 1)
			opt = argv[i + 1];
		else
			usage("Unknown argument, or argument '%s' requires a parameter", arg);

		i++;
		if (!strcmp(arg, "--config") || !strcmp(arg, "-c")) {
			merlin_config_file = opt;
			continue;
		}
		unknown_argument:
		usage("Unknown argument: %s", arg);
	}

	if (!merlin_config_file)
		usage("No config-file specified\n");

	merlin_config_file = nspath_absolute(merlin_config_file, NULL);
	if (!grok_config(merlin_config_file)) {
		fprintf(stderr, "%s contains errors. Bailing out\n", merlin_config_file);
		return 1;
	}

	if (!pidfile)
		pidfile = PKGRUNDIR "/merlin.pid";

	if (killing)
		return kill_daemon(pidfile);

	if (status)
		return daemon_status(pidfile);

	if (use_database && !import_program) {
		lwarn("Using database, but no import program configured. Are you sure about this?");
		lwarn("If not, make sure you specify the import_program directive in");
		lwarn("the \"daemon\" section of your merlin configuration file");
	}

	log_init();
	ipc.action = ipc_action_handler;
	result = ipc_init();
	if (result < 0) {
		printf("Failed to initalize ipc socket: %s\n", strerror(errno));
		return 1;
	}
	if (net_init() < 0) {
		printf("Failed to initialize networking: %s\n", strerror(errno));
		return 1;
	}

	if (!debug) {
		if (daemonize(merlin_user, NULL, pidfile, 0) < 0)
			exit(EXIT_FAILURE);

		/*
		 * we'll leak these file-descriptors, but that
		 * doesn't really matter as we just want accidental
		 * output to go somewhere where it'll be ignored
		 */
		fclose(stdin);
		open("/dev/null", O_RDONLY);
		fclose(stdout);
		open("/dev/null", O_WRONLY);
		fclose(stderr);
		open("/dev/null", O_WRONLY);
	}

	signal(SIGINT, merlind_sighandler);
	signal(SIGTERM, merlind_sighandler);
	signal(SIGUSR1, sigusr_handler);
	signal(SIGUSR2, sigusr_handler);

	sql_init();
	state_init();
	linfo("Merlin daemon " PACKAGE_VERSION " successfully initialized");
	polling_loop();
	state_deinit();
	clean_exit(0);

	return 0;
}