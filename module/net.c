#include <netdb.h>
#include <sys/select.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include "module.h"
#include "logging.h"
#include "io.h"
#include "ipc.h"
#include "net.h"

#define MERLIN_CONNECT_TIMEOUT 20 /* the (hardcoded) connect timeout we use */
#define MERLIN_CONNECT_INTERVAL 5 /* connect interval */

static int net_sock = -1; /* listening sock descriptor */

static unsigned short net_source_port(merlin_node *node)
{
	return ntohs(node->sain.sin_port) + default_port;
}

static merlin_node *find_node(struct sockaddr_in *sain)
{
	uint i;
	merlin_node *first = NULL;

	if (!sain)
		return NULL;

	for (i = 0; i < num_nodes; i++) {
		merlin_node *node = node_table[i];
		unsigned short source_port = ntohs(sain->sin_port);
		unsigned short in_port = net_source_port(node);
		ldebug("FINDNODE: node->sain.sin_addr.s_addr: %d", node->sain.sin_addr.s_addr);
		if (node->sain.sin_addr.s_addr == sain->sin_addr.s_addr) {
			if (source_port == in_port) {
				/* perfect match */
				ldebug("Inbound connection matches %s exactly (%s:%d)",
				       node->name, inet_ntoa(sain->sin_addr), in_port);
				return node;
			}

			if (!first && !(node->flags & MERLIN_NODE_FIXED_SRCPORT))
				first = node;
		}
	}

	if (first) {
		lwarn("Inbound connection presumably from %s (%s:%d != %s:%d)",
			  first->name,
			  inet_ntoa(sain->sin_addr), ntohs(sain->sin_port),
			  inet_ntoa(first->sain.sin_addr), net_source_port(first));
	}

	return first;
}


/*
 * Check if a socket is connected by looking up
 * ip and port of the remote host.
 * Returns 0 if not, and 1 if it is.
 */
int net_is_connected(merlin_node *node)
{
	struct sockaddr_in sain;
	socklen_t slen;
	int optval = 0, gsoerr = 0, gsores = 0, gpnres = 0, gpnerr = 0;

	if (!node || node->sock < 0)
		return 0;

	if (node->state == STATE_CONNECTED)
		return 1;
	if (node->state == STATE_NONE)
		return 0;

	/*
	 * yes, getpeername() actually has to be here, or getsockopt()
	 * won't return errors when we're not yet connected. It's
	 * important that we read the socket error state though, or
	 * some older kernels will maintain the link in SYN_SENT state
	 * more or less indefinitely, so get all the syscalls taken
	 * care of no matter if they actually work or not.
	 */
	errno = 0;
	slen = sizeof(struct sockaddr_in);
	gpnres = getpeername(node->sock, (struct sockaddr *)&sain, &slen);
	gpnerr = errno;
	slen = sizeof(optval);
	gsores = getsockopt(node->sock, SOL_SOCKET, SO_ERROR, &optval, &slen);
	gsoerr = errno;
	if (!gpnres && !gsores && !optval && !gpnerr && !gsoerr) {
		return 1;
	}

	if (optval) {
		node_disconnect(node, "connect() to %s node %s (%s:%d) failed: %s",
		                node_type(node), node->name,
		                inet_ntoa(node->sain.sin_addr),
		                ntohs(node->sain.sin_port),
		                strerror(optval));
		return 0;
	}

	if (gsores < 0 && gsoerr != ENOTCONN) {
		node_disconnect(node, "getsockopt(%d) failed for %s node %s: %s",
						node->sock, node_type(node), node->name, strerror(gsoerr));
	}

	if (gpnres < 0 && gpnerr != ENOTCONN) {
		lerr("getpeername(%d) failed for %s: %s",
			 node->sock, node->name, strerror(gpnerr));
		return 0;
	}

	/*
	 * if a connection is in progress, we should be getting
	 * ENOTCONN, but we need to give it time to complete
	 * first. 30 seconds should be enough.
	 */
	if (node->last_conn_attempt + MERLIN_CONNECT_TIMEOUT < time(NULL)) {
		node_disconnect(node, "connect() timed out after %d seconds",
						MERLIN_CONNECT_TIMEOUT);
	}

	return 0;
}


/*
 * Reads input from a particular node and ships it off to
 * the "handle_event()"
 */
int net_input(int sd, int io_evt, void *node_)
{
	merlin_event *pkt;
	merlin_node *node = (merlin_node *)node_;
	int len, events = 0;

	errno = 0;
	ldebug("NETINPUT from %p (%s)", node, node ? node->name : "oops");
	len = node_recv(node);
	if (len < 0) {
		return 0;
	}
	node->stats.bytes.read += len;
	node->last_recv = time(NULL);

	while ((pkt = node_get_event(node))) {
		events++;
		handle_event(node, pkt);
		free(pkt);
	}
	ldebug("Read %d events in %s from %s node %s",
		   events, human_bytes(len), node_type(node), node->name);

	return events;
}


/*
 * Negotiate which socket to use for communication when the remote
 * host has accepted a connection attempt from us while we have
 * accepted one from the remote host. We must make sure both ends
 * agree on one socket to use.
 */
static int net_negotiate_socket(merlin_node *node, int con, int lis)
{
	struct sockaddr_in lissain, consain;
	socklen_t slen = sizeof(struct sockaddr_in);

	linfo("negotiate: Choosing socket for %s %s (%d or %d)", node_type(node), node->name, con, lis);

	if (con < 0)
		return lis;
	if (lis < 0)
		return con;

	/* we prefer the socket with the lowest ip-address */
	if (getsockname(lis, (struct sockaddr *)&lissain, &slen) < 0) {
		lerr("negotiate: getsockname(%d, ...) failed: %s",
			 lis, strerror(errno));
		return con;
	}

	if (getpeername(con, (struct sockaddr *)&consain, &slen) < 0) {
		lerr("negotiate: getpeername(%d, ...) failed: %s",
			 con, strerror(errno));
		return lis;
	}

	ldebug("negotiate: lis(%d): %s:%d", lis,
		   inet_ntoa(lissain.sin_addr), ntohs(lissain.sin_port));
	ldebug("negotiate: con(%d): %s:%d", con,
		   inet_ntoa(consain.sin_addr), ntohs(consain.sin_port));

	if (lissain.sin_addr.s_addr > consain.sin_addr.s_addr) {
		ldebug("negotiate: con has lowest ip. using that");
		return con;
	}
	if (consain.sin_addr.s_addr > lissain.sin_addr.s_addr) {
		ldebug("negotiate: lis has lowest ip. using that");
		return lis;
	}

	/*
	 * this will happen if multiple merlin instances run
	 * on the same server, such as when we're testing
	 * things. In that case, let the portnumber decide
	 * the tiebreak
	 */
	if (lissain.sin_port > consain.sin_port) {
		ldebug("negotiate: con has lowest port. using that");
		return con;
	}
	if (consain.sin_port > lissain.sin_port) {
		ldebug("negotiate: lis has lowest port. using that");
		return lis;
	}

	ldebug("negotiate: con and lis are equal. killing both");
	node->last_conn_attempt_logged = 0;
	node_disconnect(node, "socket negotiation failed");
	iobroker_close(nagios_iobs, lis);
	node->sock = -1;

	return -1;
}

/*
 * This gets called when a connect() attempt has become writable.
 * It's entirely possible that the node we're trying to connect
 * to has connected to us while we were waiting for them, in
 * which case we need to figure out which of the two connections
 * we're supposed to use.
 */
static int conn_writable(int sd, int events, void *node_)
{
	merlin_node *node = (merlin_node *)node_;
	int result;
	int sel_sd;

	/* unregister so we don't peg one cpu at 100% */
	ldebug("CONN: In conn_writable(): node=%s; sd=%d; node->conn_sock=%d", node->name, sd, node->conn_sock);
	iobroker_unregister(nagios_iobs, sd);

	if (node->sock < 0) {
		/* no inbound connection accept()'ed yet */
		node->sock = sd;
		node->conn_sock = -1;
		if (!net_is_connected(node)) {
			node_disconnect(node, "Connection attempt failed: %s", strerror(errno));
			close(sd);
			return 0;
		}
		iobroker_register(nagios_iobs, sd, node, net_input);
		node_set_state(node, STATE_NEGOTIATING, "Connect completed successfully. Negotiating protocol");
		return 0;
	}

	sel_sd = net_negotiate_socket(node, node->conn_sock, node->sock);
	if (sel_sd < 0) {
		node_disconnect(node, "Failed to negotiate socket");
		return 0;
	}

	if (sel_sd == node->conn_sock) {
		iobroker_close(nagios_iobs, node->sock);
	} else if (sel_sd == node->sock) {
		iobroker_close(nagios_iobs, node->conn_sock);
	}

	node->sock = sel_sd;
	node->conn_sock = -1;
	node_set_state(node, STATE_NEGOTIATING, "polled for writability");
	/* now re-register for input */
	ldebug("IOB: registering %s(%d) for input events", node->name, node->sock);
	result = iobroker_register(nagios_iobs, node->sock, node, net_input);
	if (result < 0) {
		lerr("IOB: Failed to register %s(%d) for input events: %s",
		     node->name, node->sock, iobroker_strerror(result));
	}

	return 0;
}


/*
 * Initiate a connection attempt to a node and mark it as PENDING.
 * Note that since we're using sockets in non-blocking mode (in order
 * to be able to effectively multiplex), the connection attempt will
 * never be completed in this function
 */
int net_try_connect(merlin_node *node)
{
	int sockopt = 1;
	struct sockaddr *sa = (struct sockaddr *)&node->sain;
	int should_log = 0;
	struct timeval connect_timeout = { MERLIN_CONNECT_TIMEOUT, 0 };
	struct sockaddr_in sain;
	time_t interval = MERLIN_CONNECT_INTERVAL;
	int result;

	/* don't log obsessively */
	if (node->last_conn_attempt_logged + 30 <= time(NULL)) {
		should_log = 1;
		node->last_conn_attempt_logged = time(NULL);
	}

	if (!(node->flags & MERLIN_NODE_CONNECT)) {
		if (should_log) {
			linfo("CONN: Connect attempt blocked by config to %s node %s",
				  node_type(node), node->name);
		}
		return 0;
	}

	/* don't bother trying to connect if it's pending or done */
	switch (node->state) {
	case STATE_NEGOTIATING:
		if (node->conn_sock < 0)
			break;
	case STATE_CONNECTED:
	case STATE_PENDING:
		ldebug("CONN: node %s state is %s, so bailing",
		       node->name, node_state_name(node->state));
		return 0;
	}

	/* if it's not yet time to connect, don't even try it */
	if (node->last_conn_attempt + interval > time(NULL)) {
		return 0;
	}

	/* mark the time so we can time it out ourselves if need be */
	node->last_conn_attempt = time(NULL);

	/* create the socket if necessary */
	if (node->conn_sock < 0) {
		node_disconnect(node, "struct reset (no real disconnect)");
		node->conn_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (node->conn_sock < 0) {
			lerr("CONN: Failed to obtain connection socket for node %s: %s", node->name, strerror(errno));
			lerr("CONN: Aborting connection attempt to %s", node->name);
			return -1;
		}
	}

	sa->sa_family = AF_INET;
	if (should_log) {
		linfo("CONN: Connecting to %s %s@%s:%d", node_type(node), node->name,
		      inet_ntoa(node->sain.sin_addr),
		      ntohs(node->sain.sin_port));
	}

	if (setsockopt(node->conn_sock, SOL_SOCKET, SO_REUSEADDR, &sockopt, sizeof(int))) {
		ldebug("CONN: Failed to set sockopt SO_REUSEADDR for node %s connect socket %d: %s",
		       node->name, node->conn_sock, strerror(errno));
	}
	if (node->flags & MERLIN_NODE_FIXED_SRCPORT) {
		ldebug("CONN: Using fixed source port %d for %s node %s",
			   net_source_port(node), node_type(node), node->name);
		/*
		 * first we bind() to a local port calculated by our own
		 * listening port + the target port.
		 */
		sain.sin_family = AF_INET;
		sain.sin_port = htons(net_source_port(node));
		sain.sin_addr.s_addr = 0;
		if (bind(node->conn_sock, (struct sockaddr *)&sain, sizeof(sain))) {
			lerr("CONN: Failed to bind() outgoing socket %d for node %s to port %d: %s",
				 node->conn_sock, node->name, ntohs(sain.sin_port), strerror(errno));
			if (errno == EBADF || errno == EADDRINUSE) {
				close(node->conn_sock);
				node->conn_sock = -1;
				return -1;
			}
		}
	}

	if (fcntl(node->conn_sock, F_SETFL, O_NONBLOCK) < 0) {
		lwarn("CONN: Failed to set socket %d for %s non-blocking: %s", node->conn_sock, node->name, strerror(errno));
	}
	if (setsockopt(node->conn_sock, SOL_SOCKET, SO_RCVTIMEO,
	               &connect_timeout, sizeof(connect_timeout)) < 0)
	{
		ldebug("CONN: Failed to set receive timeout for %d, node %s: %s",
		       node->conn_sock, node->name, strerror(errno));
	}
	if (setsockopt(node->conn_sock, SOL_SOCKET, SO_SNDTIMEO,
	               &connect_timeout, sizeof(connect_timeout)) < 0)
	{
		ldebug("CONN: Failed to set send timeout for %d, node %s: %s",
		       node->conn_sock, node->name, strerror(errno));
	}

	if (connect(node->conn_sock, sa, sizeof(struct sockaddr_in)) < 0) {
		if (errno == EINPROGRESS) {
			/*
			 * non-blocking socket and connect() can't be completed
			 * immediately (ie, the normal case)
			 */
			node_set_state(node, STATE_PENDING, "Connecting");
		}
		else if (errno == EALREADY) {
			ldebug("CONN: Connect already in progress for socket %d to %s. This should never happen", node->conn_sock, node->name);
			node_set_state(node, STATE_PENDING, "connect() already in progress");
		} else {
			/* a real connection error */
			ldebug("CONN: connect() via %d to %s failed: %s",
			       node->conn_sock, node->name, strerror(errno));
			close(node->conn_sock);
			node->conn_sock = -1;
			if (should_log) {
				node_disconnect(node, "CONN: connect() failed to %s node '%s' (%s:%d): %s",
								node_type(node), node->name,
								inet_ntoa(node->sain.sin_addr),
								ntohs(node->sain.sin_port),
								strerror(errno));
			} else {
				node_disconnect(node, NULL);
			}
			return -1;
		}
	}

	result = iobroker_register_out(nagios_iobs, node->conn_sock, node, conn_writable);
	if (result < 0) {
		node_disconnect(node, "IOB: Failed to register %s connect socket %d with iobroker: %s",
		                node->name, node->conn_sock, iobroker_strerror(result));
		close(node->conn_sock);
		node->conn_sock = -1;
		return -1;
	}

	return 0;
}


/*
 * Accept an inbound connection from a remote host
 * Returns 0 on success and -1 on errors
 */
static int net_accept_one(int sd, int events, void *discard)
{
	int sock, result;
	merlin_node *node;
	struct sockaddr_in sain;
	socklen_t slen = sizeof(struct sockaddr_in);

	sock = accept(sd, (struct sockaddr *)&sain, &slen);
	if (sock < 0) {
		lerr("accept() failed: %s", strerror(errno));
		return -1;
	}

	node = find_node(&sain);
	linfo("NODESTATE: %s connected from %s:%d. Current state is %s",
		  node ? node->name : "An unregistered node",
		  inet_ntoa(sain.sin_addr), ntohs(sain.sin_port),
		  node ? node_state_name(node->state) : "unknown");
	if (!node) {
		close(sock);
		return 0;
	}

	switch (node->state) {
	case STATE_NEGOTIATING:
	case STATE_CONNECTED: case STATE_PENDING:
		/* if node->sock >= 0, we must negotiate which one to use */
		if (node->sock >= 0) {
			int sel_sd = net_negotiate_socket(node, node->sock, sock);
			if (sel_sd != sock) {
				close(sock);
			}
		}
		break;

	case STATE_NONE:
		/*
		 * we must close it unconditionally or we'll leak fd's
		 * for reconnecting nodes that were previously connected
		 */
		node_disconnect(node, "fd leak prevention for connecting nodes");
		node->sock = sock;
		break;

	default:
		lerr("%s %s has an unknown status", node_type(node), node->name);
		break;
	}

	node_set_state(node, STATE_NEGOTIATING, "Inbound connection accepted. Negotiating protocol version");
	result = iobroker_register(nagios_iobs, node->sock, node, net_input);
	if (result < 0) {
		lerr("IOB: Failed to register %d for %s node %s for input events: %s",
		     node->sock, node_type(node), node->name, iobroker_strerror(result));
	}

	return sock;
}


/* close all sockets and release the memory used by
 * static global vars for networking purposes */
int net_deinit(void)
{
	uint i;

	for (i = 0; i < num_nodes; i++) {
		node_disconnect(node_table[i], "Deinitializing networking");
	}

	iobroker_close(nagios_iobs, net_sock);
	close(net_sock);
	net_sock = -1;

	return 0;
}

/*
 * set up the listening socket (if applicable)
 */
int net_init(void)
{
	int result, sockopt = 1;
	struct sockaddr_in sain, inbound;
	struct sockaddr *sa = (struct sockaddr *)&sain;
	socklen_t addrlen = sizeof(inbound);

	if (!num_nodes)
		return 0;

	sain.sin_addr.s_addr = default_addr;
	sain.sin_port = htons(default_port);
	sain.sin_family = AF_INET;

	net_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (net_sock < 0)
		return -1;

	merlin_set_socket_options(net_sock, 0);

	/* if this fails we can do nothing but try anyway */
	(void)setsockopt(net_sock, SOL_SOCKET, SO_REUSEADDR, &sockopt, sizeof(int));

	result = bind(net_sock, sa, addrlen);
	if (result < 0)
		return -1;

	result = listen(net_sock, SOMAXCONN);
	if (result < 0)
		return -1;

	result = iobroker_register(nagios_iobs, net_sock, NULL, net_accept_one);
	if (result < 0) {
		lerr("IOB: Failed to register network socket with I/O broker: %s", iobroker_strerror(result));
		return -1;
	}

	return 0;
}

/* send a specific packet to a specific host */
int net_sendto(merlin_node *node, merlin_event *pkt)
{
	if (!pkt || !node) {
		lerr("net_sendto() called with neither node nor pkt");
		return -1;
	}

    /* Do not block in the Naemon event loop, retry on the next iteration. */
	return node_send_event(node, pkt, 0);
}

int net_sendto_many(merlin_node **ntable, uint num, merlin_event *pkt)
{
	uint i;

	if (!ntable || !pkt || !num || !*ntable)
		return -1;

	for (i = 0; i < num; i++) {
		merlin_node *node = ntable[i];
		net_sendto(node, pkt);
	}

	return 0;
}


/*
 * If a node hasn't been heard from in too long, we mark it as no
 * longer connected, signalling that we should, potentially, take
 * over checks for the AWOL node
 */
static void check_node_activity(merlin_node *node)
{
	time_t now = time(NULL);

	if (node->sock == -1 || node->state != STATE_CONNECTED)
		return;

	/* this one's on a reaaaally slow link */
	if (!node->data_timeout)
		return;

	if (node->last_recv < now - node->data_timeout)
		node_disconnect(node, "Too long since last action");
}
