/* Regression test documenting two distinct bugs behind a report that a
 * host's info page ("Other tags:" row, web/svcstatus-info.c's
 * generate_info()) sometimes seems to be missing/duplicating tags. Both
 * live in lib/loadhosts.c's xmh_item_idx(), which generate_info() calls to
 * decide a raw hosts.cfg tag is already a recognized attribute (and should
 * therefore NOT be echoed again in "Other tags").
 *
 * Bug 1 -- array truncation, case-independent (see "truncation" tests
 * below). xmh_item_idx() scans the static xmh_item_key[] table from index 0
 * and stops at the first NULL slot. lib/loadhosts.c even self-checks this
 * at setup (line ~217: "if (i != XMH_IP) errprintf(...)"), assuming every
 * real "tag:"-style key lives below index XMH_IP. That assumption no longer
 * holds: XMH_DOCURL, XMH_NOPROP, XMH_CLASS, XMH_OS, XMH_NOCOLUMNS,
 * XMH_NOTBEFORE, XMH_NOTAFTER, XMH_COMPACT, XMH_INTERFACES and
 * XMH_ACCEPT_ONLY were all added to enum xmh_item_t (lib/loadhosts.h) AFTER
 * XMH_IP/XMH_HOSTNAME, so their real keys ("CLASS:", "OS:", "DOC:", ...)
 * sit past the point where the scan already stopped. xmh_item_idx() can
 * therefore never recognize any of those ten keys, in ANY case -- they
 * always leak into "Other tags", duplicating whatever field displays them
 * elsewhere (e.g. CLASS: shows in lib/headfoot.c's class icon).
 *
 * Bug 2 -- case sensitivity, for keys that ARE reachable (index < XMH_IP).
 * xmh_find_item() (backs every xmh_item(host, XMH_COMMENT/...) lookup)
 * matches a tag's key prefix with strncasecmp -- case-INsensitive.
 * xmh_item_idx() matches with strncmp -- case-sensitive. A reachable tag
 * written in non-canonical case (e.g. "comment:" instead of "COMMENT:")
 * therefore still populates its structured field (proving xmh_find_item
 * recognized it) while ALSO leaking into "Other tags" (xmh_item_idx did
 * not), duplicating it -- while the canonically-cased form is correctly
 * suppressed there.
 *
 * NEITHER BUG IS FIXED. Left failing on purpose pending a maintainer
 * decision on the right fix for each (likely: reorder/consolidate
 * xmh_item_key[] for bug 1, and pick one case rule for bug 2). This test is
 * meant to be discussed alongside that decision, not merged as a passing
 * guard.
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
 * Mirrors the "Other tags:" filter in web/svcstatus-info.c's generate_info()
 * verbatim (including the URL-fetch-modifier exceptions), so this test
 * tracks that function's actual logic instead of a paraphrase of it.
 */
static int shown_as_other_tag(const char *val)
{
	if (*val == '~') val++;
	return ( (xmh_item_idx((char *)val) == -1)  &&
	         (strncmp(val, "http", 4)    != 0)  &&
	         (strncmp(val, "cont;", 5)   != 0)  &&
	         (strncmp(val, "cont=", 5)   != 0)  &&
	         (strncmp(val, "nocont;", 7) != 0)  &&
	         (strncmp(val, "nocont=", 7) != 0)  &&
	         (strncmp(val, "type;", 5)   != 0)  &&
	         (strncmp(val, "type=", 5)   != 0)  &&
	         (strncmp(val, "post;", 5)   != 0)  &&
	         (strncmp(val, "post=", 5)   != 0)  &&
	         (strncmp(val, "nopost=", 7) != 0)  &&
	         (strncmp(val, "nopost;", 7) != 0) );
}

static int host_has_other_tag(void *host, const char *needle)
{
	char *val = xmh_item_walk(host);
	while (val) {
		if (shown_as_other_tag(val) && strcmp(val, needle) == 0) return 1;
		val = xmh_item_walk(NULL);
	}
	return 0;
}

int main(int argc, char *argv[])
{
	void *classhost, *commentcanon, *commentlower;
	char *v;

	if (argc != 2) { fprintf(stderr, "usage: %s hosts.cfg\n", argv[0]); return 2; }

	if (load_hostnames(argv[1], NULL, 1) == -1) {
		fprintf(stderr, "cannot load %s\n", argv[1]);
		return 2;
	}

	classhost = hostinfo("classhost.example.com");
	commentcanon = hostinfo("commentcanon.example.com");
	commentlower = hostinfo("commentlower.example.com");
	if (!classhost || !commentcanon || !commentlower) {
		fprintf(stderr, "test hosts not found\n");
		return 2;
	}

	/* --- Bug 1: array truncation, independent of case --- */
	v = xmh_item(classhost, XMH_CLASS);
	expect("classhost: XMH_CLASS populated", v && (strcmp(v, "web") == 0),
	       "CLASS:web must populate the Class field");
	expect("classhost: CLASS:web (canonical case) suppressed from Other tags",
	       !host_has_other_tag(classhost, "CLASS:web"),
	       "BUG (truncation): xmh_item_idx's scan of xmh_item_key[] stops at the first "
	       "NULL slot (XMH_IP) before ever reaching XMH_CLASS's index, so even an "
	       "exact-case CLASS: tag is never recognized and leaks into Other tags");

	/* --- Bug 2: case sensitivity, for a key that IS reachable (XMH_COMMENT
	 * is index 3, far below the XMH_IP cutoff) --- */
	v = xmh_item(commentcanon, XMH_COMMENT);
	expect("commentcanon: XMH_COMMENT populated", v && (strcmp(v, "hello") == 0),
	       "COMMENT:hello must populate the Comment field");
	expect("commentcanon: COMMENT:hello (canonical case) suppressed from Other tags",
	       !host_has_other_tag(commentcanon, "COMMENT:hello"),
	       "a canonically-cased, in-range reserved tag must not also appear as a generic tag");

	v = xmh_item(commentlower, XMH_COMMENT);
	expect("commentlower: XMH_COMMENT populated despite different case", v && (strcmp(v, "hello") == 0),
	       "comment:hello should still populate the Comment field (case-insensitive match)");
	expect("commentlower: comment:hello suppressed from Other tags",
	       !host_has_other_tag(commentlower, "comment:hello"),
	       "BUG (case): xmh_item_idx is case-sensitive so this reserved tag is not "
	       "recognized and leaks into Other tags, duplicating the Comment field's value");

	printf(failures ? "FAILED\n" : "ALL OK\n");
	return failures ? 1 : 0;
}
