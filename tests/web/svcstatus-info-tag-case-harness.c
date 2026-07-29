/* Regression test documenting four related defects in lib/loadhosts.c's
 * reserved-tag key table, found while investigating a production report that
 * a host's info page ("Other tags:" row, web/svcstatus-info.c's
 * generate_info()) sometimes seems to be missing or duplicating tags.
 *
 * Three of the four trace to xmh_item_idx(), which answers "is this raw
 * hosts.cfg tag a recognized reserved attribute?" for two callers:
 * generate_info() (to avoid echoing a tag that already has its own field)
 * and xymonnet/xymonnet.c:492 (to decide a tag is NOT a network test).
 *
 * Bug 1 -- key table truncation, independent of case.
 *   xmh_item_idx() scans xmh_item_key[] from index 0 and stops at the first
 *   NULL slot; lib/loadhosts.c self-checks that this stop lands exactly on
 *   XMH_IP (see ~line 217), i.e. it assumes every real key sits below
 *   XMH_IP. That no longer holds: DOC:, NOPROP:, ACCEPTONLY:, CLASS:, OS:,
 *   NOCOLUMNS:, NOTBEFORE:, NOTAFTER:, COMPACT: and INTERFACES: were all
 *   added to enum xmh_item_t (lib/loadhosts.h) after XMH_IP, so those ten
 *   keys are past the point where the scan already stopped and can never be
 *   recognized -- in ANY case. They always leak into "Other tags",
 *   duplicating whatever field renders them elsewhere (CLASS:, for
 *   instance, drives the class icon in lib/headfoot.c).
 *
 * Bug 2 -- case sensitivity, for the keys that ARE reachable.
 *   xmh_find_item() (backs every xmh_item(host, XMH_COMMENT/...) lookup)
 *   matches a key prefix with strncasecmp -- case-INsensitive.
 *   xmh_item_idx() matches with strncmp -- case-sensitive. So a reachable
 *   tag written in non-canonical case still populates its structured field
 *   (xmh_find_item recognized it) while ALSO leaking into "Other tags"
 *   (xmh_item_idx did not). Note there is no single canonical convention to
 *   follow: the table mixes upper-case keys (NET:, COMMENT:, NOCLEAR) with
 *   lower-case ones (ssldays=, prefer, trace), as does hosts.cfg(5) itself.
 *
 * Bug 3 -- xymonnet corrupts the host record via the same misclassification.
 *   Because xmh_item_idx() reports the ten bug-1 keys as unrecognized,
 *   xymonnet/xymonnet.c:492 accepts them as test specifications. Every one
 *   of them contains ':', so they fall through that dispatch chain to its
 *   final "Simple TCP connect test" branch, which splits the spec IN PLACE
 *   at '@' and ':' (xymonnet.c:~656). The pointer being split comes from
 *   xmh_item_walk(), i.e. it points into the host record's own allelems
 *   buffer -- so the tag is truncated and its value destroyed. The service
 *   lookup on the truncated name then fails and the "test" is silently
 *   dropped. (xymonnet reads none of those ten attributes itself, so its
 *   own behaviour survives; the damage is a corrupted in-memory record.)
 *
 * Bug 4 -- latent: XMH_FLAG_MULTIHOMED is not registered as a flag.
 *   lib/loadhosts.c:176 sets xmh_item_name[XMH_FLAG_MULTIHOMED] to
 *   "XMH_MULTIHOMED" -- missing FLAG_. xmh_item_isflag[] is derived by
 *   prefix-matching "XMH_FLAG_" against that name (line ~222), so
 *   MULTIHOMED is never marked a flag: xmh_item() returns the empty
 *   remainder after the key instead of the canonical key string that every
 *   other flag yields, and xmh_key_idx() cannot resolve the enum's own
 *   name. Its only consumer (xymond/xymond.c:1641) tests == NULL, and ""
 *   is non-NULL, so nothing breaks today -- hence "latent".
 *
 * NONE OF THESE ARE FIXED. This harness is written fix-forward: every
 * assertion states the desired end state, so each one starts passing when
 * the corresponding defect is fixed, rather than needing to be inverted.
 * Left failing on purpose pending a maintainer decision on the right fix
 * for each.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

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

/*
 * Mirrors the final "Simple TCP connect test" branch of xymonnet.c's
 * testspec dispatch (xymonnet.c:~648-660): it splits the spec in place at
 * '@' and then at ':'. Reproduced here rather than by running xymonnet,
 * because the corruption is internal to xymonnet's own address space and
 * not observable from outside it.
 */
static void xymonnet_tcp_split_in_place(char *testspec)
{
	char *option, *srcip;

	srcip = strchr(testspec, '@');
	if (srcip) { *srcip = '\0'; srcip++; }

	option = strchr(testspec, ':');
	if (option) { *option = '\0'; option++; }
}

/* Hand a host's own live tag string to the routine above, the way
 * xymonnet does (its testspec pointer comes straight from xmh_item_walk). */
static void xymonnet_process_tag(void *host, const char *tagtext)
{
	char *tag = xmh_item_walk(host);
	while (tag) {
		if (strcmp(tag, tagtext) == 0) { xymonnet_tcp_split_in_place(tag); return; }
		tag = xmh_item_walk(NULL);
	}
}

int main(int argc, char *argv[])
{
	void *classhost, *commentcanon, *commentlower, *mutatehost, *flaghost;
	char *v;

	if (argc != 2) { fprintf(stderr, "usage: %s hosts.cfg\n", argv[0]); return 2; }

	if (load_hostnames(argv[1], NULL, 1) == -1) {
		fprintf(stderr, "cannot load %s\n", argv[1]);
		return 2;
	}

	classhost    = hostinfo("classhost.example.com");
	commentcanon = hostinfo("commentcanon.example.com");
	commentlower = hostinfo("commentlower.example.com");
	mutatehost   = hostinfo("mutatehost.example.com");
	flaghost     = hostinfo("flaghost.example.com");
	if (!classhost || !commentcanon || !commentlower || !mutatehost || !flaghost) {
		fprintf(stderr, "test hosts not found\n");
		return 2;
	}

	/* --- Bug 1: key table truncation, independent of case --- */
	v = xmh_item(classhost, XMH_CLASS);
	expect("classhost: XMH_CLASS populated", v && (strcmp(v, "web") == 0),
	       "CLASS:web must populate the Class field");
	expect("classhost: CLASS:web (canonical case) suppressed from Other tags",
	       !host_has_other_tag(classhost, "CLASS:web"),
	       "BUG 1 (truncation): xmh_item_idx's scan of xmh_item_key[] stops at the first "
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
	       "BUG 2 (case): xmh_item_idx is case-sensitive so this reserved tag is not "
	       "recognized and leaks into Other tags, duplicating the Comment field's value");

	/* --- Bug 3: xymonnet accepts the misclassified tag as a testspec and
	 * splits it inside the host's own buffer. Guarded on the same
	 * misclassification that causes it, so that fixing bug 1 skips the
	 * mutation entirely and this assertion then passes. --- */
	if (xmh_item_idx("CLASS:web") == -1) xymonnet_process_tag(mutatehost, "CLASS:web");
	v = xmh_item(mutatehost, XMH_CLASS);
	expect("mutatehost: CLASS: value survives xymonnet's testspec handling",
	       v && (strcmp(v, "web") == 0),
	       "BUG 3 (record corruption): xmh_item_idx does not recognize CLASS:, so xymonnet "
	       "accepts it as a test spec and its TCP-connect branch splits the string in place "
	       "at ':' -- inside the host record's own allelems buffer -- destroying the value");

	/* --- Bug 4 (latent): the flag contract. dialup is the control: a
	 * correctly-registered flag yields its canonical key string. --- */
	v = xmh_item(flaghost, XMH_FLAG_DIALUP);
	expect("flaghost: dialup flag yields its canonical key", v && (strcmp(v, "dialup") == 0),
	       "a registered flag must return its key string, not the empty remainder");

	v = xmh_item(flaghost, XMH_FLAG_MULTIHOMED);
	expect("flaghost: MULTIHOMED flag yields its canonical key",
	       v && (strcmp(v, "MULTIHOMED") == 0),
	       "BUG 4 (name typo): xmh_item_name[XMH_FLAG_MULTIHOMED] is \"XMH_MULTIHOMED\" "
	       "(missing FLAG_), so xmh_item_isflag[] never marks it a flag and xmh_item() "
	       "returns the empty remainder instead of the canonical key");

	expect("XMH_FLAG_MULTIHOMED is resolvable by its own enum name",
	       xmh_key_idx("XMH_FLAG_MULTIHOMED") == XMH_FLAG_MULTIHOMED,
	       "BUG 4 (name typo): same root cause -- xmh_key_idx() cannot resolve the name, "
	       "so it returns XMH_LAST");

	printf(failures ? "FAILED\n" : "ALL OK\n");
	return failures ? 1 : 0;
}
