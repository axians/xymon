/* Regression test for a flap-suppression defect: the list form of the
 * hosts.cfg "noflap=test1,test2,..." tag stops working for every test after
 * the first one evaluated.
 *
 * xymond/xymond.c's isset_noflap() (~line 1389) does:
 *
 *     dstr = xmh_item(hinfo, XMH_NOFLAP);
 *     ...
 *     tok = strtok(dstr, ",");
 *
 * xmh_item() for XMH_NOFLAP returns a pointer straight INTO the host
 * record's own allelems buffer (lib/loadhosts.c's XMH_NOFLAP case falls
 * through to xmh_find_item(), which returns `host->elems[i] + keylen` --
 * unlike e.g. XMH_DOCURL, which builds a copy). strtok() then writes NULs
 * over the commas of that live buffer, permanently truncating the host's
 * noflap list at the first comma. Every later call sees only the first
 * entry, so flapping suppression silently stops applying to the rest.
 *
 * This is an oversight rather than a design choice: four of the six places
 * that tokenize an xmh_item() result strdup() it first --
 * xymond/convertnk.c:32 (XMH_NK), xymond/xymond_client.c:263 and
 * xymongen/loaddata.c:117 (XMH_NOCOLUMNS), lib/webaccess.c:75
 * (XMH_ALLPAGEPATHS). The two that do not are isset_noflap() here and
 * xymongen/loaddata.c:422 (XMH_COMPACT, covered structurally at the end).
 *
 * Note on how this must be tested: the defect only shows up on the SECOND
 * and later evaluations of the same host record, because the first call
 * still tokenizes an intact string. That mirrors production, where
 * isset_noflap() runs once per incoming status message. A test that used a
 * fresh host per test name would pass and hide the bug -- so the list
 * assertions below deliberately share one host and run in sequence. Which
 * tests lose suppression therefore depends on call order; in production
 * that means whichever test reports first keeps it and the others quietly
 * lose it.
 *
 * NOT FIXED -- pending maintainer discussion. Assertions are written
 * fix-forward: each states the desired end state, so it starts passing
 * once the defect is fixed rather than needing to be inverted.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libxymon.h"

static int failures = 0;

static void expect(const char *label, int cond, const char *detail)
{
	if (!cond) {
		fprintf(stderr, "%s: %s\n", label, detail);
		failures++;
	}
}

/*
 * Verbatim copy of xymond/xymond.c's isset_noflap(), minus its dbgprintf,
 * so this test exercises that function's real logic rather than a
 * paraphrase. It is static in xymond.c and xymond has no library form, so
 * mirroring is the only way to drive it.
 */
static int isset_noflap(void *hinfo, char *testname, char *hostname)
{
	char *tok, *dstr;
	int keylen;

	dstr = xmh_item(hinfo, XMH_NOFLAP);
	if (!dstr) return 0; /* no 'noflap' set */

	/* Check bare noflap (disable for host) vs "noflap=test1,test2" */
	/* A bare noflap will be set equal to the key itself (usually NOFLAP) like a flag */
	if (strcmp(dstr, "NOFLAP") == 0) return 1;

	/* if not 'NOFLAP', we should receive "=test1,test2". Skip the = */
	if (*dstr == '=') dstr++;

	keylen = strlen(testname);
	tok = strtok(dstr, ",");
	while (tok && (strncmp(testname, tok, keylen) != 0)) tok = strtok(NULL, ",");
	if (!tok) return 0; /* specifies noflap, but this test is not in the list */

	return 1;
}

/*
 * Mirrors the COMPACT tokenization in xymongen/loaddata.c (~line 422):
 * nested strtok_r on the xmh_item() result, again with no copy.
 */
static void loaddata_tokenize_compact(char *compacted)
{
	char *tok1, *savep1 = NULL, *savep2 = NULL;

	tok1 = strtok_r(compacted, ",", &savep1);
	while (tok1) {
		(void)strtok_r(tok1, "=", &savep2);
		(void)strtok_r(NULL, "\n", &savep2);
		tok1 = strtok_r(NULL, ",", &savep1);
	}
}

int main(int argc, char *argv[])
{
	void *barehost, *listhost, *recordhost, *compacthost;
	char *before, *after;

	if (argc != 2) { fprintf(stderr, "usage: %s hosts.cfg\n", argv[0]); return 2; }

	if (load_hostnames(argv[1], NULL, 1) == -1) {
		fprintf(stderr, "cannot load %s\n", argv[1]);
		return 2;
	}

	barehost    = hostinfo("barehost.example.com");
	listhost    = hostinfo("listhost.example.com");
	recordhost  = hostinfo("recordhost.example.com");
	compacthost = hostinfo("compacthost.example.com");
	if (!barehost || !listhost || !recordhost || !compacthost) {
		fprintf(stderr, "test hosts not found\n");
		return 2;
	}

	/* --- Control: the bare flag form. lib/loadhosts.c's XMH_NOFLAP case
	 * explicitly mirrors flag semantics for this, so it yields the key
	 * string "NOFLAP" and isset_noflap() short-circuits before any
	 * tokenizing. Repeated calls must therefore both hold. --- */
	expect("bare noflap: suppresses flapping for the first test queried",
	       isset_noflap(barehost, "web", "barehost") == 1,
	       "a bare noflap tag must disable flapping for every test on the host");
	expect("bare noflap: still suppresses on a second, different test",
	       isset_noflap(barehost, "cpu", "barehost") == 1,
	       "a bare noflap tag must keep working across repeated evaluations");

	/* --- The defect: "noflap=web,cpu,disk" evaluated once per status
	 * message, as xymond does. Sequential on purpose (see header). --- */
	expect("noflap list: first test evaluated is suppressed",
	       isset_noflap(listhost, "web", "listhost") == 1,
	       "web is listed in noflap=web,cpu,disk and must be suppressed");
	expect("noflap list: second test in the list is still suppressed",
	       isset_noflap(listhost, "cpu", "listhost") == 1,
	       "BUG: the first call's strtok() truncated the host's live noflap list at the "
	       "first comma, so cpu is no longer found and silently loses flap suppression");
	expect("noflap list: third test in the list is still suppressed",
	       isset_noflap(listhost, "disk", "listhost") == 1,
	       "BUG: same root cause -- disk is lost once the list has been tokenized in place");

	/* Control: a test that genuinely is not listed must stay unsuppressed,
	 * so the assertions above cannot be satisfied by making this always true. */
	expect("noflap list: an unlisted test is not suppressed",
	       isset_noflap(listhost, "mem", "listhost") == 0,
	       "mem is absent from noflap=web,cpu,disk and must not be suppressed");

	/* --- Root cause, stated directly: evaluating a host must not damage
	 * its configuration record. --- */
	before = strdup(xmh_item(recordhost, XMH_NOFLAP));
	(void)isset_noflap(recordhost, "web", "recordhost");
	after = xmh_item(recordhost, XMH_NOFLAP);
	expect("noflap: evaluating a host leaves its record intact",
	       after && (strcmp(before, after) == 0),
	       "BUG: isset_noflap() tokenizes the pointer xmh_item() returned, which points "
	       "into the host record's own allelems buffer, so the stored tag is modified");
	free(before);

	/* --- Same hazard, structural only: xymongen/loaddata.c:422 tokenizes
	 * XMH_COMPACT in place too. No user-visible symptom is demonstrated
	 * here -- within one xymongen run each host is normally visited once.
	 * The exposure would be two hostlist entries resolving to the same
	 * record via hostinfo() (duplicate/cloned host definitions), where the
	 * second visit would see a truncated value. Asserted as the same
	 * "consumers must not mutate the record" invariant. --- */
	before = strdup(xmh_item(compacthost, XMH_COMPACT));
	loaddata_tokenize_compact(xmh_item(compacthost, XMH_COMPACT));
	after = xmh_item(compacthost, XMH_COMPACT);
	expect("COMPACT: tokenizing for page layout leaves the record intact",
	       after && (strcmp(before, after) == 0),
	       "BUG (latent): xymongen/loaddata.c tokenizes the XMH_COMPACT value in place "
	       "without copying it first, damaging the host record the same way");
	free(before);

	printf(failures ? "FAILED\n" : "ALL OK\n");
	return failures ? 1 : 0;
}
