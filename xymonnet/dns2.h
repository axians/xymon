/*----------------------------------------------------------------------------*/
/* Xymon monitor network test tool.                                           */
/*                                                                            */
/* Copyright (C) 2004-2011 Henrik Storner <henrik@hswn.dk>                    */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

#ifndef __DNS2_H__
#define __DNS2_H__

typedef struct dns_resp_t {
	int msgstatus;
	strbuffer_t *msgbuf;
	struct dns_resp_t *next;
} dns_resp_t;

extern void dns_detail_callback(void *arg, int status, int timeouts, unsigned char *abuf, int alen);
extern int dns_soa_is_predecessor(unsigned int candidate, unsigned int reference);
extern int dns_name_type(char *name);

/*
 * Cross-NS consistency check (issue #235). Both functions operate on raw
 * wire-format buffers only, so dns.c's networking/orchestration code never
 * needs to know about ares_dns_record_t directly -- that stays entirely
 * inside dns2.c, alongside the ARES_VERSION gate that decides whether it's
 * even available. On c-ares < 1.22, dns_extract_ns_names() returns NULL
 * and dns_crossns_evaluate() returns true (nothing to check / consistent),
 * so callers automatically get today's single-server-only behavior.
 */
extern char **dns_extract_ns_names(const unsigned char *abuf, int alen);
extern int dns_crossns_evaluate(int atype, unsigned char **abufs, int *alens,
				const char **ns_labels, int ns_count, strbuffer_t *summary);

#endif

