#include "module.h"
#include "node.h"

static merlin_peer_group **peer_group;
static unsigned int num_peer_groups;
bitmap *poller_handled_hosts = NULL;
bitmap *poller_handled_services = NULL;

static void pgroup_reassign_checks(merlin_peer_group *pgrp)
{
	int i, x;

	/* first reset top-level hosts */
	ldebug("Reassigning checks for group ipc");
	for (i = 0; i < ipc.pgroup->active_nodes; i++) {
		merlin_node *node = ipc.pgroup->nodes[i];
		node->assigned.extra.hosts = node->assigned.extra.services = 0;
		node->assigned.current.hosts =
			ipc.pgroup->assign[ipc.pgroup->active_nodes - 1][node->peer_id].hosts;
		node->assigned.current.services =
			ipc.pgroup->assign[ipc.pgroup->active_nodes - 1][node->peer_id].services;
	}
	for (i = 1; i < num_peer_groups; i++) {
		int active;
		merlin_peer_group *pg = peer_group[i];

		active = pg->active_nodes;
		ldebug("Reassigning for peer group %d with %d active nodes",
			   pg->id, active);

		if (!active) {
			ldebug("ipc.pgroup->active_nodes = %d", ipc.pgroup->active_nodes);
			for (x = 0; x < ipc.pgroup->active_nodes; x++) {
				merlin_node *node = ipc.pgroup->nodes[x];
				ldebug("Dealing with node %s", node->name);
				if (node->state != STATE_CONNECTED)
					continue;
				node->assigned.extra.hosts +=
					pg->assign[ipc.pgroup->active_nodes - 1][node->peer_id].hosts;
				node->assigned.extra.services +=
					pg->assign[ipc.pgroup->active_nodes - 1][node->peer_id].services;
				ldebug("  done. Moving on to next node");
			}
			continue;
		}

		ldebug("Peer group is active. Neato");
		for (x = 0; x < pg->total_nodes; x++) {
			merlin_node *node = pg->nodes[x];
			node->assigned.current.hosts = pg->assign[active - 1][node->peer_id].hosts;
			node->assigned.current.services = pg->assign[active - 1][node->peer_id].services;
		}
	}
}

static int timeval_comp(const struct timeval *a, const struct timeval *b)
{
	if (a == b)
		return 0;

	if (a->tv_sec == b->tv_sec)
		return a->tv_usec - b->tv_usec;

	return a->tv_sec - b->tv_sec;
}

static int cmp_peer(const void *a_, const void *b_)
{
	const merlin_node *a = *(const merlin_node **)a_;
	const merlin_node *b = *(const merlin_node **)b_;

	/* make sure disconnected nodes are sorted last */
	if (a->state != b->state) {
		if (a->state == STATE_CONNECTED)
			return -1;
		if (b->state == STATE_CONNECTED)
			return 1;
	}

	/*
	 * also make sure nodes that haven't sent a CTRL_ACTIVE
	 * are sorted after the ones that have, and discarded in
	 * the id assignment dance
	 */
	if (a->info.start.tv_sec && !b->info.start.tv_sec)
		return -1;
	if (b->info.start.tv_sec && !a->info.start.tv_sec)
		return 1;

	return timeval_comp(&a->info.start, &b->info.start);
}

void pgroup_assign_peer_ids(merlin_peer_group *pg)
{
	uint i;

	if (!pg)
		return;

	/* sort peerid_table with earliest started first */
	ldebug("Sorting peer id table for peer-group %d with %d nodes",
		   pg->id, pg->total_nodes);
	qsort(pg->nodes, pg->total_nodes, sizeof(merlin_node *), cmp_peer);
	pg->active_nodes = 0;
	ldebug("Done sorting");

	/*
	 * this could be done with a binary search, but since we expect
	 * fewer than 10 peers in each tier and we still have to walk all
	 * the ones with a start-time higher than ours it's not really
	 * worth the complexity
	 */
	ldebug("Assining peer ids. Order:");
	for (i = 0; i < pg->total_nodes; i++) {
		merlin_node *node = pg->nodes[i];

		/*
		 * we must assign peer_id using i here, in case we sort multiple
		 * times. Otherwise we'll only ever increase the peer_id and
		 * end up with all peers having the same id.
		 */
		node->peer_id = i;
		ldebug("   %.1d: %s", node->peer_id, node->name);
		if (node == &ipc || (node->state == STATE_CONNECTED && node->info.start.tv_sec)) {
			pg->active_nodes++;
		}
	}

	ldebug("Reassigning checks");
	pgroup_reassign_checks(pg);

	if (pg == ipc.pgroup) {
		ipc.info.peer_id = ipc.peer_id;
		linfo("We're now peer #%d out of %d active ones",
			  ipc.peer_id, pg->active_nodes);
		linfo("Handling %u host and %u service checks",
			  ipc.assigned.current.hosts, ipc.assigned.current.services);
		ipc.info.host_checks_handled = ipc.assigned.current.hosts;
		ipc.info.service_checks_handled = ipc.assigned.current.services;
	}
}

static merlin_peer_group *pgroup_create(char *hostgroups)
{
	merlin_peer_group *pg, **ary;

	if (!(pg = calloc(1, sizeof(*pg))))
		return NULL;

	if (!(ary = realloc(peer_group, num_peer_groups + 1 * sizeof(merlin_peer_group *)))) {
		free(pg);
		return NULL;
	}
	peer_group = ary;

	pg->hostgroups = hostgroups;
	pg->id = num_peer_groups++;
	peer_group[pg->id] = pg;

	return pg;
}

static merlin_peer_group *pgroup_get_by_cshgs(char *hgs)
{
	int i;

	for (i = 0; i < num_peer_groups; i++) {
		if (!peer_group[i]->hostgroups)
			continue;

		if (!strcmp(peer_group[i]->hostgroups, hgs))
			return peer_group[i];
	}

	return pgroup_create(hgs);
}

static void pgroup_alloc_counters(merlin_peer_group *pg)
{
	int i;

	pg->host_map = bitmap_create(num_objects.hosts);
	pg->service_map = bitmap_create(num_objects.services);
	pg->alloc = pg->total_nodes;
	pg->assign = calloc(pg->alloc, sizeof(void *));
	for (i = 0; i < pg->alloc; i++) {
		pg->assign[i] = calloc(i + 1, sizeof(**pg->assign));
	}
}

static void pgroup_destroy(merlin_peer_group *pg)
{
	int i;

	bitmap_destroy(pg->host_map);
	bitmap_destroy(pg->service_map);
	for (i = 0; i < max(pg->total_nodes, num_peers); i++) {
		free(pg->assign[i]);
	}
}

static int pgroup_add_node(merlin_peer_group *pg, merlin_node *node)
{
	merlin_node **ary;

	ldebug("Adding node '%s' to peer group %d", node->name, pg->id);
	ary = realloc(pg->nodes, (pg->total_nodes + 1) * sizeof(merlin_node *));
	pg->nodes = ary;
	pg->nodes[pg->total_nodes++] = node;
	node->pgroup = pg;

	return 0;
}

/* returns -1 if there are config issues and 0 otherwise */
static int map_pgroup_hgroup(merlin_peer_group *pg, hostgroup *hg)
{
	hostsmember *hm;
	int dupes = 0;

	ldebug("Mapping hostgroup '%s' to peer group %d\n", hg->group_name, pg->id);
	for (hm = hg->members; hm; hm = hm->next) {
		servicesmember *sm;
		host *h = hm->host_ptr;
		int x, peer_id;

		ldebug("  Looking at host %d: '%s'\n", h->id, h->name);

		/*
		 * if the host is already in this selection, such as
		 * from overlapping hostgroups assigned to a poller group,
		 * we just move on (this also ensures we don't double-count
		 * services).
		 */
		if (bitmap_isset(pg->host_map, h->id)) {
			ldebug("       already in this group\n");
			continue;
		}

		/*
		 * if it's not ours but another poller handles it, we
		 * need to warn about it so we can perform a more
		 * exact check later
		 */
		if (bitmap_isset(poller_handled_hosts, h->id)) {
			ldebug("Host '%s' is handled by two different poller groups!", h->name);
			dupes++;
		}
		bitmap_set(poller_handled_hosts, h->id);

		for (x = 0; x < pg->alloc; x++) {
			peer_id = h->id % (x + 1);
			pg->assign[x][peer_id].hosts++;
		}

		bitmap_set(pg->host_map, h->id);
		for (sm = h->services; sm; sm = sm->next) {
			service *s = sm->service_ptr;

			bitmap_set(pg->service_map, s->id);
			bitmap_set(poller_handled_services, s->id);
			for (x = 0; x < pg->alloc; x++) {
				peer_id = s->id % (x + 1);
				pg->assign[x][peer_id].services++;
			}
		}
	}

	return dupes;
}

static void pgroup_map_objects(void)
{
	int i, x, dupes = 0;

	for (i = 0; i < num_peer_groups; i++) {
		char *p, *comma;
		struct merlin_peer_group *pg = peer_group[i];

		pgroup_alloc_counters(pg);

		for (p = pg->hostgroups; p; p = comma) {
			hostgroup *hg;
			comma = strchr(p, ',');
			if (comma)
				*comma = 0;
			hg = find_hostgroup(p);
			if (!hg) {
				lerr("Fatal: Hostgroup '%s' not found", p);
				sigshutdown = TRUE;
				return;
			}

			dupes = map_pgroup_hgroup(pg, hg);
			if (dupes) {
				lerr("CONFIG ANOMALY: Hostgroup '%s' has %d hosts overlapping with another hostgroup used for poller assigment",
					hg->group_name, dupes);
			}
			pg->overlapping += dupes;
			if (comma)
				*(comma++) = ',';
			else
				break;
		}
		pg->assigned.hosts = bitmap_count_set_bits(pg->host_map);
		pg->assigned.services = bitmap_count_set_bits(pg->service_map);
	}

	for (i = 0; i < num_objects.hosts; i++) {
		servicesmember *sm;
		if (bitmap_isset(poller_handled_hosts, i))
			continue;

		if (i < num_objects.hosts && !bitmap_isset(poller_handled_hosts, i)) {
			for (x = 0; x < num_peers + 1; x++) {
				int peer_id = i % (x + 1);
				ipc.pgroup->assign[x][peer_id].hosts++;
			}
			ipc.pgroup->assigned.hosts++;
		}
		for (sm = host_ary[i]->services; sm; sm = sm->next) {
			for (x = 0; x < num_peers + 1; x++) {
				int peer_id = sm->service_ptr->id % (x + 1);
				ipc.pgroup->assign[x][peer_id].services++;
			}
			ipc.pgroup->assigned.services++;
		}
	}

	linfo("hosts: %u; services: %u", num_objects.hosts, num_objects.services);
	for (i = 0; i < num_peer_groups; i++) {
		char *p = NULL;
		merlin_peer_group *pg = peer_group[i];
		linfo("peer-group %u", pg->id);
		for (x = 0; x < pg->total_nodes; x++) {
			merlin_node *node = pg->nodes[x];
			char *buf = NULL;
			if (p) {
				asprintf(&buf, "%s, %s", node->name, p);
				free(p);
				p = buf;
			} else {
				asprintf(&p, "%s", node->name);
			}
		}
		linfo("  %d nodes          : %s", pg->total_nodes, p);
		free(p);
		if (pg->hostgroups)
			linfo("  hostgroups: %s", pg->hostgroups);
		linfo("  assigned hosts   : %u", pg->assigned.hosts);
		linfo("  assigned services: %u", pg->assigned.services);
		linfo("  Check/takeover accounting:");
		for (x = 1; x < pg->alloc; x++) {
			int y;
			linfo("    %d node%s online:", x + 1, x ? "s" : "");
			for (y = 0; y < x + 1; y++) {
				linfo("      peer %d takes %d hosts, %d services", y,
					   pg->assign[x][y].hosts,
					   pg->assign[x][y].services);
			}
		}
	}
}

static int cmpstringp(const void *p1, const void *p2)
{
	return strcmp(*(const char **)p1, *(const char **)p2);
}

/*
 * returns a sorted version of a comma-separated string, with
 * spaces surrounding commas removed
 */
static char *get_sorted_csstr(const char *orig_str)
{
	char *str, *comma, *ret = NULL, **ary, *next;
	unsigned int i = 0, entries = 0, len;

	if (!orig_str || !(str = strdup(orig_str)))
		return NULL;
	len = strlen(orig_str);

	if (!(ary = calloc(len / 2, sizeof(char *)))) {
		free(str);
		free(ret);
		return NULL;
	}

	for (next = str;;) {
		char *p = next;

		while (*p == ',' || *p == ' ' || *p == '\t')
			p++;
		comma = next = strchr(p, ',');
		if (comma) {
			next = comma + 1;
			while (*comma == ',' || *comma == ' ' || *comma == '\t') {
				*comma = 0;
				comma--;
			}
		}

		ary[entries++] = p;
		if (!comma)
			break;
	}

	qsort(ary, entries, sizeof(char *), cmpstringp);
	len = 1;
	for (i = 0; i < entries; i++) {
		len += strlen(ary[i]) + 1;
	}

	if (!(ret = calloc(1, len))) {
		free(str);
		free(ary);
		return NULL;
	}

	for (i = 0; i < entries; i++) {
		if (ret[0])
			ret[strlen(ret)] = ',';
		strcat(ret, ary[i]);
	}

	free(str);
	free(ary);
	return ret;
}

void pgroup_init(void)
{
	int i;
	merlin_peer_group *pg;

	linfo("Initializing peer-groups");
	if (num_pollers) {
		poller_handled_hosts = bitmap_create(num_objects.hosts);
		poller_handled_services = bitmap_create(num_objects.services);
	}
	ipc.pgroup = pgroup_create(NULL);
	pgroup_add_node(ipc.pgroup, &ipc);
	for (i = 0; i < num_nodes; i++) {
		merlin_node *node = node_table[i];
		if (node->type == MODE_PEER)
			pgroup_add_node(ipc.pgroup, node);
	}
	if (!num_pollers || !hostgroup_list) {
		ipc.pgroup->assigned.hosts = num_objects.hosts;
		ipc.pgroup->assigned.services = num_objects.services;
	} else for (i = 0; i < num_pollers; i++) {
		merlin_node *node = poller_table[i];
		char *hgs;

		hgs = get_sorted_csstr(node->hostgroups);
		pg = pgroup_get_by_cshgs(hgs);
		pgroup_add_node(pg, node);
	}

	pgroup_map_objects();
}

void pgroup_deinit(void)
{
	int i;

	for (i = 0; i < num_peer_groups; i++)
		pgroup_destroy(peer_group[i]);
	free(peer_group);
	peer_group = NULL;
	bitmap_destroy(poller_handled_hosts);
	bitmap_destroy(poller_handled_services);
}