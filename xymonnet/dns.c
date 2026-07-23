/*----------------------------------------------------------------------------*/
/* Xymon monitor network test tool.                                           */
/*                                                                            */
/* Copyright (C) 2004-2011 Henrik Storner <henrik@hswn.dk>                    */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char rcsid[] = "$Id$";

#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <netdb.h>
#include <sys/time.h>

#include "libxymon.h"

#include <ares.h>
#include <ares_version.h>

#include "dns.h"
#include "dns2.h"

#ifdef HPUX
/* Doesn't have hstrerror */
char *hstrerror(int err) { return ""; }
#endif

static ares_channel mychannel;
static int pending_dns_count = 0;
int use_ares_lookup = 1;
int max_dns_per_run = 0;

int dns_stats_total   = 0;
int dns_stats_success = 0;
int dns_stats_failed  = 0;
int dns_stats_lookups = 0;
int dnstimeout        = 30;

FILE *dnsfaillog = NULL;


typedef struct dnsitem_t {
	char *name;
	struct in_addr addr;
	struct dnsitem_t *next;
	int failed;
	struct timespec resolvetime;
} dnsitem_t;

static void * dnscache;

static void dns_init(void)
{
	static int initdone = 0;

	if (initdone) return;

	dnscache = xtreeNew(strcasecmp);

	if (use_ares_lookup) {
		struct ares_options options;
		int status;

		/* ARES timeout backported from Xymon trunk 20120411 - this should give us a ~23 second timeout */
		options.timeout = 2000;
		options.tries = 4;

		status = ares_init_options(&mychannel, &options, (ARES_OPT_TIMEOUTMS | ARES_OPT_TRIES));
		if (status != ARES_SUCCESS) {
			errprintf("Cannot initialize ARES resolver, using standard\n");
			errprintf("ARES error was: '%s'\n", ares_strerror(status));
			use_ares_lookup = 0;
		}
	}

	initdone = 1;
}

static char *find_dnscache(char *hostname)
{
	struct in_addr inp;
	xtreePos_t handle;
	dnsitem_t *dnsc;

	dns_init();

	if (inet_aton(hostname, &inp) != 0) {
		/* It is an IP, so just use that */
		return hostname;
	}

	/* In the cache ? */
	handle = xtreeFind(dnscache, hostname);
	if (handle == xtreeEnd(dnscache)) return NULL;

	dnsc = (dnsitem_t *)xtreeData(dnscache, handle);
	return inet_ntoa(dnsc->addr);
}


static void dns_simple_callback(void *arg, int status, int timeout, struct hostent *hent)
{
	struct dnsitem_t *dnsc = (dnsitem_t *)arg;
	struct timespec etime;

	getntimer(&etime);
	tvdiff(&dnsc->resolvetime, &etime, &dnsc->resolvetime);
	pending_dns_count--;

	if (status == ARES_SUCCESS) {
		memcpy(&dnsc->addr, *(hent->h_addr_list), sizeof(dnsc->addr));
		dbgprintf("Got DNS result for host %s : %s\n", dnsc->name, inet_ntoa(dnsc->addr));
		dns_stats_success++;
	}
	else {
		memset(&dnsc->addr, 0, sizeof(dnsc->addr));
		dbgprintf("DNS lookup failed for %s - status %s (%d)\n", dnsc->name, ares_strerror(status), status);
		dnsc->failed = 1;
		dns_stats_failed++;

		if (dnsfaillog) {
			fprintf(dnsfaillog, "DNS lookup failed for %s - status %s (%d)\n", 
				dnsc->name, ares_strerror(status), status);
		}
	}
}



static void dns_ares_queue_run(ares_channel channel)
{
	int nfds;
	fd_set read_fds, write_fds;
	struct timeval *tvp, tv;
	int loops = 0;

	if ((channel == mychannel) && (!pending_dns_count)) return;

	dbgprintf("Processing %d DNS lookups with ARES\n", pending_dns_count);

	while (1) {	/* Loop continues until all requests handled (or time out) */
		loops++;
		FD_ZERO(&read_fds);
		FD_ZERO(&write_fds);
		nfds = ares_fds(channel, &read_fds, &write_fds);
		if (nfds == 0) {
			dbgprintf("Finished ARES queue after loop %d\n", loops);
			break;	/* No pending requests */
		}

		/* 
		 * Determine how long select() waits before processing timeouts.
		 * "dnstimeout" is the user configurable option which is
		 * the absolute maximum timeout value. However, ARES also
		 * has built in timeouts - these are defined at ares_init()
		 * using ARES_OPT_TIMEOUTMS and ARES_OPT_TRIES (default
		 * 5 secs / 4 tries = 20 second timeout).
		 */
		tv.tv_sec = dnstimeout; tv.tv_usec = 0;
		tvp = ares_timeout(channel, &tv, &tv);

		select(nfds, &read_fds, &write_fds, NULL, tvp);
		ares_process(channel, &read_fds, &write_fds);
	}

	if (pending_dns_count > 0) {
		errprintf("Odd ... pending_dns_count=%d after a queue run\n", 
				pending_dns_count);
		pending_dns_count = 0;
	}
}


void add_host_to_dns_queue(char *hostname)
{
	dnsitem_t *dnsc;

	dns_init();
	dns_stats_total++;

	if (find_dnscache(hostname)) return; 	/* Already resolved */

	if (max_dns_per_run && (pending_dns_count >= max_dns_per_run)) {
		/* Limit the number of requests we do per run */
		dns_ares_queue_run(mychannel);
	}

	/* New hostname */
	dnsc = (dnsitem_t *)calloc(1, sizeof(dnsitem_t));

	dbgprintf("Adding hostname '%s' to resolver queue\n", hostname);
	pending_dns_count++;

	dnsc->name = strdup(hostname);
	getntimer(&dnsc->resolvetime);
	xtreeAdd(dnscache, dnsc->name, dnsc);

	if (use_ares_lookup) {
		ares_gethostbyname(mychannel, hostname, AF_INET, dns_simple_callback, dnsc);
	}
	else {
		/*
		 * This uses the normal resolver functions, but 
		 * sends the result through the same processing
		 * functions as used when we use ARES.
		 */
		struct hostent *hent;
		int status;

		hent = gethostbyname(hostname);
		if (hent) {
			status = ARES_SUCCESS;
			dns_stats_success++;
		}
		else {
			status = ARES_ENOTFOUND;
			dns_stats_failed++;
			dbgprintf("gethostbyname() failed with err %d: %s\n", h_errno, hstrerror(h_errno));
			if (dnsfaillog) {
				fprintf(dnsfaillog, "Hostname lookup failed for %s - status %s (%d)\n", 
						hostname, hstrerror(h_errno), h_errno);
			}
		}

		/* Send the result to our normal callback function */
		dns_simple_callback(dnsc, status, 0, hent);
	}
}

void add_url_to_dns_queue(char *url)
{
	weburl_t weburl;

	dns_init();

	decode_url(url, &weburl);

	if (weburl.proxyurl) {
		if (weburl.proxyurl->parseerror) return;
		add_host_to_dns_queue(weburl.proxyurl->host); 
	}
	else {
		if (weburl.desturl->parseerror) return;
		add_host_to_dns_queue(weburl.desturl->host); 
	}
}


void flush_dnsqueue(void)
{
	dns_init();
	dns_ares_queue_run(mychannel);
}

char *dnsresolve(char *hostname)
{
	char *result;

	dns_init();

	if (hostname == NULL) return NULL;

	flush_dnsqueue();
	dns_stats_lookups++;

	result = find_dnscache(hostname);
	if (result == NULL) {
		errprintf("dnsresolve: name '%s' not in cache. You probably have a NULL IP setting and the 'testip' flag set.\n", hostname);
		return NULL;
	}
	if (strcmp(result, "0.0.0.0") == 0) return NULL;

	return result;
}

/*
 * Cross-NS consistency check (issue #235). Captures the raw response
 * buffer from a single ares_search() call, for feeding to
 * dns_extract_ns_names()/dns_crossns_evaluate() (dns2.c) -- this file
 * never needs to touch ares_dns_record_t directly, keeping that (and the
 * ARES_VERSION gate that guards it) entirely inside dns2.c.
 */
typedef struct dns_crossns_capture_t {
	int status;
	unsigned char *abuf;
	int alen;
} dns_crossns_capture_t;

static void dns_crossns_capture_callback(void *arg, int status, int timeouts, unsigned char *abuf, int alen)
{
	dns_crossns_capture_t *cap = (dns_crossns_capture_t *)arg;

	(void)timeouts;
	cap->status = status;
	if ((status == ARES_SUCCESS) && abuf && (alen > 0)) {
		cap->abuf = (unsigned char *)malloc((size_t)alen);
		if (cap->abuf) {
			memcpy(cap->abuf, abuf, (size_t)alen);
			cap->alen = alen;
		}
	}
}

/*
 * Query a single nameserver IP for atype:tlookup and return the raw
 * response buffer (caller frees), or NULL on any failure. Used both for
 * the initial "what are this zone's NS records" discovery query and for
 * re-querying the same atype:tlookup against each discovered NS.
 */
static unsigned char *dns_crossns_query_one(const char *ip, char *tlookup, int atype, int *alen_out)
{
	ares_channel nschannel;
	struct ares_options options;
	struct in_addr addr;
	dns_crossns_capture_t cap;

	*alen_out = 0;
	if (inet_aton(ip, &addr) == 0) return NULL;

	options.flags = ARES_FLAG_NOCHECKRESP;
	options.servers = &addr;
	options.nservers = 1;
	options.timeout = 2000;
	options.tries = 2;
	if (ares_init_options(&nschannel, &options, (ARES_OPT_FLAGS | ARES_OPT_SERVERS | ARES_OPT_TIMEOUTMS | ARES_OPT_TRIES)) != ARES_SUCCESS) {
		return NULL;
	}

	memset(&cap, 0, sizeof(cap));
	ares_search(nschannel, tlookup, C_IN, atype, dns_crossns_capture_callback, &cap);
	dns_ares_queue_run(nschannel);
	ares_destroy(nschannel);

	*alen_out = cap.alen;
	return cap.abuf;
}

/*
 * Automatic cross-NS consistency check for one dns= sub-test: discover the
 * zone's authoritative nameservers (queried against 'serverip', which is
 * presumed authoritative or at least able to answer for its own zone),
 * resolve each via the normal system resolver, re-query atype:tlookup
 * against every one of them, and evaluate agreement (dns2.c). Silently a
 * no-op (returns true) if NS discovery finds nothing usable (e.g. tlookup
 * isn't a zone apex, or the responding side is old c-ares) -- this keeps
 * ordinary non-zone-apex dns= tests (e.g. "dns=mx:host.example.com")
 * behaving exactly as before. Appends a summary to 'banner' when a
 * cross-NS check was actually performed.
 *
 * 'staticns' is the raw value of the "dns-ns:" hosts.cfg tag (NULL if not
 * set): NULL means automatic NS discovery (the default); "off" or "none"
 * (case-insensitive) disables cross-NS checking for this host entirely
 * (e.g. for zones like google.com whose NS clusters are independently
 * managed by design, not via replication, so "consistency" doesn't apply);
 * anything else is treated as a comma-separated list of NS hostnames/IPs to
 * use INSTEAD of dynamic discovery.
 */
static int dns_crossns_check(char *serverip, char *tlookup, int atype, char *staticns, strbuffer_t *banner)
{
	unsigned char *ns_abuf;
	int ns_alen = 0;
	char **ns_names;
	int i, ns_count, consistent;
	unsigned char **abufs;
	int *alens;
	char **labels;

	if (staticns && (strcasecmp(staticns, "off") == 0 || strcasecmp(staticns, "none") == 0)) {
		return 1; /* cross-NS checking explicitly disabled for this host */
	}

	if (staticns) {
		/* Static override: use exactly the admin-declared NS list instead of
		 * dynamic NS-record discovery (e.g. for zones with independently
		 * managed NS clusters, where discovery would find the same list
		 * anyway but the point is to skip auto-discovery's assumptions). */
		char *statcopy = strdup(staticns);
		char *tok;

		ns_count = 0;
		for (tok = strtok(statcopy, ","); tok; tok = strtok(NULL, ",")) ns_count++;
		xfree(statcopy);

		ns_names = (char **)calloc((size_t)ns_count + 1, sizeof(char *));
		statcopy = strdup(staticns);
		i = 0;
		for (tok = strtok(statcopy, ","); tok; tok = strtok(NULL, ",")) ns_names[i++] = strdup(tok);
		ns_names[ns_count] = NULL;
		xfree(statcopy);
	}
	else {
		ns_abuf = dns_crossns_query_one(serverip, tlookup, T_NS, &ns_alen);
		if (!ns_abuf) return 1;

		ns_names = dns_extract_ns_names(ns_abuf, ns_alen);
		xfree(ns_abuf);
		if (!ns_names) return 1; /* not a zone apex, or c-ares too old to check */

		for (ns_count = 0; ns_names[ns_count]; ns_count++) ;
	}

	if (ns_count < 2) {
		/* Nothing meaningful to cross-check with 0 or 1 NS record. */
		for (i = 0; i < ns_count; i++) xfree(ns_names[i]);
		xfree(ns_names);
		return 1;
	}

	/* Resolve every NS hostname via the normal system resolver. */
	for (i = 0; i < ns_count; i++) add_host_to_dns_queue(ns_names[i]);

	abufs = (unsigned char **)calloc((size_t)ns_count, sizeof(unsigned char *));
	alens = (int *)calloc((size_t)ns_count, sizeof(int));
	labels = (char **)calloc((size_t)ns_count, sizeof(char *));

	for (i = 0; i < ns_count; i++) {
		char *ip = dnsresolve(ns_names[i]);

		labels[i] = strdup(ns_names[i]);
		if (ip) abufs[i] = dns_crossns_query_one(ip, tlookup, atype, &alens[i]);
	}

	consistent = dns_crossns_evaluate(atype, abufs, alens, (const char **)labels, ns_count, banner);

	for (i = 0; i < ns_count; i++) {
		xfree(ns_names[i]);
		xfree(labels[i]);
		xfree(abufs[i]);
	}
	xfree(ns_names);
	xfree(abufs);
	xfree(alens);
	xfree(labels);

	return consistent;
}

int dns_test_server(char *serverip, char *hostname, char *crossns, strbuffer_t *banner)
{
	ares_channel channel;
	struct ares_options options;
	struct in_addr serveraddr;
	int status;
	struct timespec starttime, endtime;
	struct timespec *tspent;
	char msg[100];
	SBUF_DEFINE(tspec);
	char *tst;
	dns_resp_t *responses = NULL;
	dns_resp_t *walk = NULL;
	int i;
	int crossns_ok;

	dns_init();

	if (inet_aton(serverip, &serveraddr) == 0) {
		errprintf("dns_test_server: serverip '%s' not a valid IP\n", serverip);
		return 1;
	}

	options.flags = ARES_FLAG_NOCHECKRESP;
	options.servers = &serveraddr;
	options.nservers = 1;
	/* ARES timeout backported from Xymon trunk 20120411 - this should give us a ~23 second timeout */
	options.timeout = 2000;
	options.tries = 4;

	status = ares_init_options(&channel, &options, (ARES_OPT_FLAGS | ARES_OPT_SERVERS | ARES_OPT_TIMEOUTMS | ARES_OPT_TRIES));
	if (status != ARES_SUCCESS) {
		errprintf("Could not initialize ares channel: %s\n", ares_strerror(status));
		return 1;
	}

	tspec = strdup(hostname);
	tspec_buflen = strlen(tspec) + 1;
	getntimer(&starttime);
	tst = strtok(tspec, ",");
	do {
		dns_resp_t *newtest = (dns_resp_t *)malloc(sizeof(dns_resp_t));
		char *p, *tlookup;
		int atype = T_A;

		newtest->msgbuf = newstrbuffer(0);
		newtest->next = NULL;
		if (responses == NULL) responses = newtest; else walk->next = newtest;
		walk = newtest;

		p = strchr(tst, ':');
		tlookup = (p ? p+1 : tst);
		if (p) { *p = '\0'; atype = dns_name_type(tst); *p = ':'; }

		dbgprintf("ares_search: tlookup='%s', class=%d, type=%d\n", tlookup, C_IN, atype);
		ares_search(channel, tlookup, C_IN, atype, dns_detail_callback, newtest);
		tst = strtok(NULL, ",");
	} while (tst);

	dns_ares_queue_run(channel);

	getntimer(&endtime);
	tspent = tvdiff(&starttime, &endtime, NULL);
	clearstrbuffer(banner); status = ARES_SUCCESS;
	strncpy(tspec, hostname, tspec_buflen);
	tst = strtok(tspec, ",");
	crossns_ok = 1;
	for (walk = responses, i=1; (walk); walk = walk->next, i++) {
		char *p, *tlookup;
		int atype = T_A;

		/* Print an identifying line if more than one query */
		if ((walk != responses) || (walk->next)) {
			snprintf(msg, sizeof(msg), "\n*** DNS lookup of '%s' ***\n", tst);
			addtobuffer(banner, msg);
		}
		addtostrbuffer(banner, walk->msgbuf);
		if (walk->msgstatus != ARES_SUCCESS) status = walk->msgstatus;
		xfree(walk->msgbuf);

		/* Automatic cross-NS consistency check (issue #235). Re-derive
		 * tlookup/atype from this sub-test's spec the same way the query
		 * loop above did. Only meaningful when the initial query itself
		 * succeeded -- no point cross-checking a lookup that already
		 * failed against its own target server. */
		p = strchr(tst, ':');
		tlookup = (p ? p+1 : tst);
		if (p) { *p = '\0'; atype = dns_name_type(tst); *p = ':'; }
		if (walk->msgstatus == ARES_SUCCESS) {
			if (!dns_crossns_check(serverip, tlookup, atype, crossns, banner)) crossns_ok = 0;
		}

		tst = strtok(NULL, ",");
	}
	xfree(tspec);
	snprintf(msg, sizeof(msg), "\nSeconds: %u.%.9ld\n", (unsigned int)tspent->tv_sec, tspent->tv_nsec);
	addtobuffer(banner, msg);

	ares_destroy(channel);

	return ((status != ARES_SUCCESS) || !crossns_ok);
}

