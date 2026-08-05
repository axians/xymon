/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Drives the real do_net.c DNS timing parser with RRD plumbing stubbed.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

char rrdvalues[8192];
static char currentfn[512];
static char captured_fns[16][512];
static char captured_values[16][8192];
static int rrd_writes;

static void setupfn(char *format, char *part) { (void)format; (void)part; }
static void setupfn2(char *format, char *part1, char *part2)
{
	snprintf(currentfn, sizeof(currentfn), format, part1, part2);
}
static void setupfn3(char *format, char *part1, char *part2, char *part3)
{
	snprintf(currentfn, sizeof(currentfn), format, part1, part2, part3);
}
static void *setup_template(char *params[]) { (void)params; return (void *)1; }
static int create_and_update_rrd(char *hostname, char *testname, char *classname,
				 char *pagepaths, char *params[], void *template)
{
	(void)hostname; (void)testname; (void)classname; (void)pagepaths;
	(void)params; (void)template;
	if (rrd_writes < 16) {
		snprintf(captured_fns[rrd_writes], sizeof(captured_fns[rrd_writes]), "%s", currentfn);
		snprintf(captured_values[rrd_writes], sizeof(captured_values[rrd_writes]), "%s", rrdvalues);
	}
	rrd_writes++;
	return 0;
}
static char *xgetenv(const char *name) { (void)name; return ""; }
static char *xstrdup(const char *value) { return strdup(value); }
#define xfree free

#include "../../xymond/rrd/do_net.c"

static int fails;

static void check_value(const char *name, const char *record, double wanted)
{
	int timestamp;
	double actual;
	double difference;

	if (sscanf(record, "%d:%lf", &timestamp, &actual) != 2) {
		printf("FAIL %s: invalid record '%s'\n", name, record);
		fails++;
		return;
	}
	difference = actual - wanted;
	if (difference < 0) difference = -difference;
	if (difference > 0.0000005) {
		printf("FAIL %s: got %.9f, want %.9f\n", name, actual, wanted);
		fails++;
	}
}

static void check_filename(int index, const char *wanted)
{
	if (strcmp(captured_fns[index], wanted) != 0) {
		printf("FAIL filename %d: got '%s', want '%s'\n",
		       index, captured_fns[index], wanted);
		fails++;
	}
}

int main(void)
{
	char message[1024];

	snprintf(message, sizeof(message), "%s",
		 "NS response time: ns1.example. 1:www.example. 0.012345678\n"
		 "NS response time: ns1.example. 28:www.example. 0.022345678\n"
		 "NS response time: ns/2.example. 0.200000000\n"
		 "NS response time: ns3.example. 1:www.example. nan\n"
		 "NS response time: ns4.example. 1:www.example. -1.0\n"
		 "NS response time: ns5.example. 1:www.example. 1.0 trailing\n"
		 "\nSeconds: 0.300000\n");

	do_net_rrd("host", "dns", "", "", message, (time_t)1000);

	if (rrd_writes != 4) {
		printf("FAIL DNS timing writes: got %d, want 4\n", rrd_writes);
		fails++;
	}
	else {
		check_filename(0, "tcp.dns.ns1.example..1_www.example..2efd8de9.rrd");
		check_value("ns1 response time", captured_values[0], 0.012345678);
		check_filename(1, "tcp.dns.ns1.example..28_www.example..c9f663c8.rrd");
		check_value("ns1 second-query response time", captured_values[1], 0.022345678);
		check_filename(2, "tcp.dns.ns_2.example..rrd");
		check_value("legacy ns2 response time", captured_values[2], 0.2);
		check_filename(3, "tcp.dns.rrd");
		check_value("aggregate response time", captured_values[3], 0.3);
	}

	if (fails) {
		printf("%d check(s) FAILED\n", fails);
		return 1;
	}
	printf("all DNS response-time checks ok\n");
	return 0;
}
