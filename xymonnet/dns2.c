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

/*
 * All of the code for parsing DNS responses and formatting these into
 * text were taken from the "adig.c" source-file included with the
 * C-ARES 1.2.0 library. This file carries the following copyright
 * notice, reproduced in full:
 *
 * --------------------------------------------------------------------
 * Copyright 1998 by the Massachusetts Institute of Technology.
 *
 * Permission to use, copy, modify, and distribute this
 * software and its documentation for any purpose and without
 * fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting
 * documentation, and that the name of M.I.T. not be used in
 * advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.
 * M.I.T. makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is"
 * without express or implied warranty.
 * --------------------------------------------------------------------
 */

#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <netdb.h>

#include "libxymon.h"

#include <ares.h>
#include <ares_dns.h>
#include <ares_version.h>

/*
 * c-ares >= 1.22 exposes a public, structured DNS message parser
 * (ares_dns_parse() / ares_dns_record_t) instead of requiring hand-rolled
 * wire-format parsing. Use it when available; otherwise fall back to the
 * legacy adig-derived parser below. Several widely used platforms (Debian
 * 12, RHEL/Rocky/Alma 8 and 9) still ship c-ares below this floor, so the
 * legacy path must be kept for the foreseeable future.
 */
#if ARES_VERSION >= 0x011600
#include <ares_dns_record.h>
#define XYMON_DNS_USE_ARES_DNS_RECORD 1
#endif

#include "dns2.h"

/* Some systems (AIX, HP-UX) don't know the DNS T_SRV record */
#ifndef T_SRV
#define T_SRV 33
#endif

/*
 * Not every platform's <arpa/nameser_compat.h> defines these newer type
 * macros (issue #235: CAA, TLSA, SVCB/HTTPS, DNSKEY/DS). Without a local
 * fallback, dns_name_type() silently mismapped "dns=CAA:name" (and friends)
 * to T_A, since the name wasn't found in the types[] table below. Numeric
 * values are the IANA-assigned DNS RR type numbers (RFC 4034, 6698, 6844,
 * 9460), matching ares_dns_rec_type_t's ARES_REC_TYPE_* values.
 */
#ifndef T_DS
#define T_DS 43
#endif
#ifndef T_DNSKEY
#define T_DNSKEY 48
#endif
#ifndef T_TLSA
#define T_TLSA 52
#endif
#ifndef T_SVCB
#define T_SVCB 64
#endif
#ifndef T_HTTPS
#define T_HTTPS 65
#endif
#ifndef T_CAA
#define T_CAA 257
#endif

static char msg[1024];

static const char *type_name(int type);
static const char *class_name(int dnsclass);
#ifdef XYMON_DNS_USE_ARES_DNS_RECORD
static void dns_render_arespec(const unsigned char *abuf, int alen, dns_resp_t *response);
static void dns_render_rr_arespec(const ares_dns_rr_t *rr, dns_resp_t *response);
#else
static void dns_render_legacy(const unsigned char *abuf, int alen, dns_resp_t *response);
static const unsigned char *display_question(const unsigned char *aptr,
					     const unsigned char *abuf, int alen,
					     dns_resp_t *response);
static const unsigned char *display_rr(const unsigned char *aptr,
				       const unsigned char *abuf, int alen,
				       dns_resp_t *response);
#endif

struct nv {
  const char *name;
  int value;
};

static const struct nv flags[] = {
  { "usevc",		ARES_FLAG_USEVC },
  { "primary",		ARES_FLAG_PRIMARY },
  { "igntc",		ARES_FLAG_IGNTC },
  { "norecurse",	ARES_FLAG_NORECURSE },
  { "stayopen",		ARES_FLAG_STAYOPEN },
  { "noaliases",	ARES_FLAG_NOALIASES }
};
static const int nflags = sizeof(flags) / sizeof(flags[0]);

static const struct nv classes[] = {
  { "IN",	C_IN },
  { "CHAOS",	C_CHAOS },
  { "HS",	C_HS },
  { "ANY",	C_ANY }
};
static const int nclasses = sizeof(classes) / sizeof(classes[0]);

static const struct nv types[] = {
  { "A",	T_A },
  { "NS",	T_NS },
  { "MD",	T_MD },
  { "MF",	T_MF },
  { "CNAME",	T_CNAME },
  { "SOA",	T_SOA },
  { "MB",	T_MB },
  { "MG",	T_MG },
  { "MR",	T_MR },
  { "NULL",	T_NULL },
  { "WKS",	T_WKS },
  { "PTR",	T_PTR },
  { "HINFO",	T_HINFO },
  { "MINFO",	T_MINFO },
  { "MX",	T_MX },
  { "TXT",	T_TXT },
  { "RP",	T_RP },
  { "AFSDB",	T_AFSDB },
  { "X25",	T_X25 },
  { "ISDN",	T_ISDN },
  { "RT",	T_RT },
  { "NSAP",	T_NSAP },
  { "NSAP_PTR",	T_NSAP_PTR },
  { "SIG",	T_SIG },
  { "KEY",	T_KEY },
  { "PX",	T_PX },
  { "GPOS",	T_GPOS },
  { "AAAA",	T_AAAA },
  { "LOC",	T_LOC },
  { "SRV",	T_SRV },
  { "AXFR",	T_AXFR },
  { "MAILB",	T_MAILB },
  { "MAILA",	T_MAILA },
  { "ANY",	T_ANY },
  { "DS",	T_DS },
  { "DNSKEY",	T_DNSKEY },
  { "TLSA",	T_TLSA },
  { "SVCB",	T_SVCB },
  { "HTTPS",	T_HTTPS },
  { "CAA",	T_CAA }
};
static const int ntypes = sizeof(types) / sizeof(types[0]);

static const char *opcodes[] = {
  "QUERY", "IQUERY", "STATUS", "(reserved)", "NOTIFY",
  "(unknown)", "(unknown)", "(unknown)", "(unknown)",
  "UPDATEA", "UPDATED", "UPDATEDA", "UPDATEM", "UPDATEMA",
  "ZONEINIT", "ZONEREF"
};

static const char *rcodes[] = {
  "NOERROR", "FORMERR", "SERVFAIL", "NXDOMAIN", "NOTIMP", "REFUSED",
  "(unknown)", "(unknown)", "(unknown)", "(unknown)", "(unknown)",
  "(unknown)", "(unknown)", "(unknown)", "(unknown)", "NOCHANGE"
};

void dns_detail_callback(void *arg, int status, int timeouts, unsigned char *abuf, int alen)
{
	dns_resp_t *response = (dns_resp_t *) arg;

	clearstrbuffer(response->msgbuf);
	response->msgstatus = status;

	/*
	 * Display an error message if there was an error, but only stop if
	 * we actually didn't get an answer buffer.
	 */
	switch (status) {
	  case ARES_SUCCESS: 
		  break;
	  case ARES_ENODATA:
		  addtobuffer(response->msgbuf, "No data returned from server\n");
		  if (!abuf) return;
		  break;
	  case ARES_EFORMERR:
		  addtobuffer(response->msgbuf, "Server could not understand query\n");
		  if (!abuf) return;
		  break;
	  case ARES_ESERVFAIL:
		  addtobuffer(response->msgbuf, "Server failed\n");
		  if (!abuf) return;
		  break;
	  case ARES_ENOTFOUND:
		  addtobuffer(response->msgbuf, "Name not found\n");
		  if (!abuf) return;
		  break;
	  case ARES_ENOTIMP:
		  addtobuffer(response->msgbuf, "Not implemented\n");
		  if (!abuf) return;
		  break;
	  case ARES_EREFUSED:
		  addtobuffer(response->msgbuf, "Server refused query\n");
		  if (!abuf) return;
		  break;
	  case ARES_EBADNAME:
		  addtobuffer(response->msgbuf, "Invalid name in query\n");
		  if (!abuf) return;
		  break;
	  case ARES_ETIMEOUT:
		  addtobuffer(response->msgbuf, "Timeout\n");
		  if (!abuf) return;
		  break;
	  case ARES_ECONNREFUSED:
		  addtobuffer(response->msgbuf, "Server unavailable\n");
		  if (!abuf) return;
		  break;
	  case ARES_ENOMEM:
		  addtobuffer(response->msgbuf, "Out of memory\n");
		  if (!abuf) return;
		  break;
	  case ARES_EDESTRUCTION:
		  addtobuffer(response->msgbuf, "Timeout (channel destroyed)\n");
		  if (!abuf) return;
		  break;
	  default:
		  addtobuffer(response->msgbuf, "Undocumented ARES return code\n");
		  if (!abuf) return;
		  break;
	}

	/* Won't happen, but check anyway, for safety. */
	if (alen < HFIXEDSZ) return;

#ifdef XYMON_DNS_USE_ARES_DNS_RECORD
	dns_render_arespec(abuf, alen, response);
#else
	dns_render_legacy(abuf, alen, response);
#endif
}

#ifndef XYMON_DNS_USE_ARES_DNS_RECORD
static void dns_render_legacy(const unsigned char *abuf, int alen, dns_resp_t *response)
{
	int id, qr, opcode, aa, tc, rd, ra, rcode;
	unsigned int qdcount, ancount, nscount, arcount, i;
	const unsigned char *aptr;

	/* Parse the answer header. */
	id = DNS_HEADER_QID(abuf);
	qr = DNS_HEADER_QR(abuf);
	opcode = DNS_HEADER_OPCODE(abuf);
	aa = DNS_HEADER_AA(abuf);
	tc = DNS_HEADER_TC(abuf);
	rd = DNS_HEADER_RD(abuf);
	ra = DNS_HEADER_RA(abuf);
	rcode = DNS_HEADER_RCODE(abuf);
	qdcount = DNS_HEADER_QDCOUNT(abuf);
	ancount = DNS_HEADER_ANCOUNT(abuf);
	nscount = DNS_HEADER_NSCOUNT(abuf);
	arcount = DNS_HEADER_ARCOUNT(abuf);

	/* Display the answer header. */
	snprintf(msg, sizeof(msg), "id: %d\n", id);
	addtobuffer(response->msgbuf, msg);
	snprintf(msg, sizeof(msg), "flags: %s%s%s%s%s\n",
		qr ? "qr " : "",
		aa ? "aa " : "",
		tc ? "tc " : "",
		rd ? "rd " : "",
		ra ? "ra " : "");
	addtobuffer(response->msgbuf, msg);
	snprintf(msg, sizeof(msg), "opcode: %s\n", opcodes[opcode]);
	addtobuffer(response->msgbuf, msg);
	snprintf(msg, sizeof(msg), "rcode: %s\n", rcodes[rcode]);
	addtobuffer(response->msgbuf, msg);

	/* Display the questions. */
	addtobuffer(response->msgbuf, "Questions:\n");
	aptr = abuf + HFIXEDSZ;
	for (i = 0; i < qdcount; i++) {
		aptr = display_question(aptr, abuf, alen, response);
		if (aptr == NULL) return;
	}

	/* Display the answers. */
	addtobuffer(response->msgbuf, "Answers:\n");
	for (i = 0; i < ancount; i++) {
		aptr = display_rr(aptr, abuf, alen, response);
		if (aptr == NULL) return;
	}

	/* Display the NS records. */
	addtobuffer(response->msgbuf, "NS records:\n");
	for (i = 0; i < nscount; i++) {
		aptr = display_rr(aptr, abuf, alen, response);
		if (aptr == NULL) return;
	}

	/* Display the additional records. */
	addtobuffer(response->msgbuf, "Additional records:\n");
	for (i = 0; i < arcount; i++) {
		aptr = display_rr(aptr, abuf, alen, response);
		if (aptr == NULL) return;
	}

	return;
}

static const unsigned char *display_question(const unsigned char *aptr,
					     const unsigned char *abuf, int alen,
					     dns_resp_t *response)
{
	char *name;
	int type, dnsclass, status;
	long len;

	/* Parse the question name. */
	status = ares_expand_name(aptr, abuf, alen, &name, &len);
	if (status != ARES_SUCCESS) return NULL;
	aptr += len;

	/* Make sure there's enough data after the name for the fixed part
	 * of the question.
	 */
	if (aptr + QFIXEDSZ > abuf + alen) {
		xfree(name);
		return NULL;
	}

	/* Parse the question type and class. */
	type = DNS_QUESTION_TYPE(aptr);
	dnsclass = DNS_QUESTION_CLASS(aptr);
	aptr += QFIXEDSZ;

	/*
	 * Display the question, in a format sort of similar to how we will
	 * display RRs.
	 */
	snprintf(msg, sizeof(msg), "\t%-15s.\t", name);
	addtobuffer(response->msgbuf, msg);
	if (dnsclass != C_IN) {
		snprintf(msg, sizeof(msg), "\t%s", class_name(dnsclass));
		addtobuffer(response->msgbuf, msg);
	}
	snprintf(msg, sizeof(msg), "\t%s\n", type_name(type));
	addtobuffer(response->msgbuf, msg);
	xfree(name);
	return aptr;
}

static const unsigned char *display_rr(const unsigned char *aptr,
				       const unsigned char *abuf, int alen,
				       dns_resp_t *response)
{
	const unsigned char *p;
	char *name;
	int type, dnsclass, ttl, dlen, status;
	long len;
	struct in_addr addr;
	struct in6_addr addr6;

	/* Parse the RR name. */
	status = ares_expand_name(aptr, abuf, alen, &name, &len);
	if (status != ARES_SUCCESS) return NULL;
	aptr += len;

	/* Make sure there is enough data after the RR name for the fixed
	* part of the RR.
	*/
	if (aptr + RRFIXEDSZ > abuf + alen) {
		xfree(name);
		return NULL;
	}

	/* Parse the fixed part of the RR, and advance to the RR data field. */
	type = DNS_RR_TYPE(aptr);
	dnsclass = DNS_RR_CLASS(aptr);
	ttl = DNS_RR_TTL(aptr);
	dlen = DNS_RR_LEN(aptr);
	aptr += RRFIXEDSZ;
	if (aptr + dlen > abuf + alen) {
		xfree(name);
		return NULL;
	}

	/* Display the RR name, class, and type. */
	snprintf(msg, sizeof(msg), "\t%-15s.\t%d", name, ttl);
	addtobuffer(response->msgbuf, msg);
	if (dnsclass != C_IN) {
		snprintf(msg, sizeof(msg), "\t%s", class_name(dnsclass));
		addtobuffer(response->msgbuf, msg);
	}
	snprintf(msg, sizeof(msg), "\t%s", type_name(type));
	addtobuffer(response->msgbuf, msg);
	xfree(name);

	/* Display the RR data.  Don't touch aptr. */
	switch (type) {
	  case T_CNAME:
	  case T_MB:
	  case T_MD:
	  case T_MF:
	  case T_MG:
	  case T_MR:
	  case T_NS:
	  case T_PTR:
		/* For these types, the RR data is just a domain name. */
		status = ares_expand_name(aptr, abuf, alen, &name, &len);
		if (status != ARES_SUCCESS) return NULL;
		snprintf(msg, sizeof(msg), "\t%s.", name);
		addtobuffer(response->msgbuf, msg);
		xfree(name);
		break;

	  case T_HINFO:
		/* The RR data is two length-counted character strings. */
		p = aptr;
		len = *p;
		if (p + len + 1 > aptr + dlen) return NULL;
		snprintf(msg, sizeof(msg), "\t%.*s", (int) len, p + 1);
		addtobuffer(response->msgbuf, msg);
		p += len + 1;
		len = *p;
		if (p + len + 1 > aptr + dlen) return NULL;
		snprintf(msg, sizeof(msg), "\t%.*s", (int) len, p + 1);
		addtobuffer(response->msgbuf, msg);
		break;

	  case T_MINFO:
		/* The RR data is two domain names. */
		p = aptr;
		status = ares_expand_name(p, abuf, alen, &name, &len);
		if (status != ARES_SUCCESS) return NULL;
		snprintf(msg, sizeof(msg), "\t%s.", name);
		addtobuffer(response->msgbuf, msg);
		xfree(name);
		p += len;
		status = ares_expand_name(p, abuf, alen, &name, &len);
		if (status != ARES_SUCCESS) return NULL;
		snprintf(msg, sizeof(msg), "\t%s.", name);
		addtobuffer(response->msgbuf, msg);
		xfree(name);
		break;

	  case T_MX:
		/* The RR data is two bytes giving a preference ordering, and then a domain name.  */
		if (dlen < 2) return NULL;
		snprintf(msg, sizeof(msg), "\t%d", (aptr[0] << 8) | aptr[1]);
		addtobuffer(response->msgbuf, msg);
		status = ares_expand_name(aptr + 2, abuf, alen, &name, &len);
		if (status != ARES_SUCCESS) return NULL;
		snprintf(msg, sizeof(msg), "\t%s.", name);
		addtobuffer(response->msgbuf, msg);
		xfree(name);
		break;

	  case T_SOA:
		/*
		 * The RR data is two domain names and then five four-byte
		 * numbers giving the serial number and some timeouts.
		 */
		p = aptr;
		status = ares_expand_name(p, abuf, alen, &name, &len);
		if (status != ARES_SUCCESS) return NULL;
		snprintf(msg, sizeof(msg), "\t%s.\n", name);
		addtobuffer(response->msgbuf, msg);
		xfree(name);
		p += len;
		status = ares_expand_name(p, abuf, alen, &name, &len);
		if (status != ARES_SUCCESS) return NULL;
		snprintf(msg, sizeof(msg), "\t\t\t\t\t\t%s.\n", name);
		addtobuffer(response->msgbuf, msg);
		xfree(name);
		p += len;
		if (p + 20 > aptr + dlen) return NULL;
		snprintf(msg, sizeof(msg), "\t\t\t\t\t\t( %d %d %d %d %d )",
			(p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3],
			(p[4] << 24) | (p[5] << 16) | (p[6] << 8) | p[7],
			(p[8] << 24) | (p[9] << 16) | (p[10] << 8) | p[11],
			(p[12] << 24) | (p[13] << 16) | (p[14] << 8) | p[15],
			(p[16] << 24) | (p[17] << 16) | (p[18] << 8) | p[19]);
		addtobuffer(response->msgbuf, msg);
		break;

	  case T_TXT:
		/* The RR data is one or more length-counted character strings. */
		p = aptr;
		while (p < aptr + dlen) {
			len = *p;
			if (p + len + 1 > aptr + dlen) return NULL;
			snprintf(msg, sizeof(msg), "\t%.*s", (int)len, p + 1);
			addtobuffer(response->msgbuf, msg);
			p += len + 1;
		}
		break;

	  case T_A:
		/* The RR data is a four-byte Internet address. */
		if (dlen != 4) return NULL;
		memcpy(&addr, aptr, sizeof(struct in_addr));
		snprintf(msg, sizeof(msg), "\t%s", inet_ntoa(addr));
		addtobuffer(response->msgbuf, msg);
		break;

	  case T_AAAA:
		/* The RR data is a 16-byte IPv6 address. */
		if (dlen != 16) return NULL;
		memcpy(&addr6, aptr, sizeof(struct in6_addr));
		addtobuffer_many(response->msgbuf, "\t", inet_ntop(AF_INET6,&addr6,msg,sizeof(msg)), NULL);
		break;

	  case T_WKS:
		/* Not implemented yet */
		break;

	  case T_SRV:
		/*
		 * The RR data is three two-byte numbers representing the
		 * priority, weight, and port, followed by a domain name.
		 */
      
		snprintf(msg, sizeof(msg), "\t%d", DNS__16BIT(aptr));
		addtobuffer(response->msgbuf, msg);
		snprintf(msg, sizeof(msg), " %d", DNS__16BIT(aptr + 2));
		addtobuffer(response->msgbuf, msg);
		snprintf(msg, sizeof(msg), " %d", DNS__16BIT(aptr + 4));
		addtobuffer(response->msgbuf, msg);
      
		status = ares_expand_name(aptr + 6, abuf, alen, &name, &len);
		if (status != ARES_SUCCESS) return NULL;
		snprintf(msg, sizeof(msg), "\t%s.", name);
		addtobuffer(response->msgbuf, msg);
		xfree(name);
		break;
      
	  default:
		snprintf(msg, sizeof(msg), "\t[Unknown RR; cannot parse]");
		addtobuffer(response->msgbuf, msg);
	}
	snprintf(msg, sizeof(msg), "\n");
	addtobuffer(response->msgbuf, msg);

	return aptr + dlen;
}
#endif /* !XYMON_DNS_USE_ARES_DNS_RECORD */

#ifdef XYMON_DNS_USE_ARES_DNS_RECORD
/*
 * Structured-record renderer (c-ares >= 1.22). Produces the same text
 * output as the legacy adig-derived parser above for the record types it
 * shares with it, but reads fields via ares_dns_parse()/ares_dns_record_t
 * accessors instead of hand-parsing the wire format. It additionally
 * decodes CAA, TLSA, and SVCB/HTTPS (issue #235), which the legacy parser
 * never supported at all. Types c-ares has no dedicated field parser for
 * (e.g. DS/DNSKEY on this c-ares version, or the pre-1987
 * MB/MD/MF/MG/MR/MINFO types, and WKS which the legacy parser never
 * rendered either) come back wrapped as ARES_REC_TYPE_RAW_RR and are shown
 * as a hex dump rather than fully decoded.
 */
static void dns_render_arespec(const unsigned char *abuf, int alen, dns_resp_t *response)
{
	ares_dns_record_t *dnsrec = NULL;
	unsigned short flags;
	int opcode, rcode;
	size_t qcnt, ancnt, nscnt, arcnt, i;

	if (ares_dns_parse(abuf, (size_t)alen, 0, &dnsrec) != ARES_SUCCESS) return;

	flags = ares_dns_record_get_flags(dnsrec);
	opcode = (int)ares_dns_record_get_opcode(dnsrec);
	rcode = (int)ares_dns_record_get_rcode(dnsrec);

	/* Display the answer header. */
	snprintf(msg, sizeof(msg), "id: %d\n", ares_dns_record_get_id(dnsrec));
	addtobuffer(response->msgbuf, msg);
	snprintf(msg, sizeof(msg), "flags: %s%s%s%s%s\n",
		(flags & ARES_FLAG_QR) ? "qr " : "",
		(flags & ARES_FLAG_AA) ? "aa " : "",
		(flags & ARES_FLAG_TC) ? "tc " : "",
		(flags & ARES_FLAG_RD) ? "rd " : "",
		(flags & ARES_FLAG_RA) ? "ra " : "");
	addtobuffer(response->msgbuf, msg);
	/* Extended (EDNS/TSIG) op/rcodes can exceed the legacy tables' bounds. */
	snprintf(msg, sizeof(msg), "opcode: %s\n",
		((opcode >= 0) && (opcode < (int)(sizeof(opcodes) / sizeof(opcodes[0])))) ? opcodes[opcode] : "(unknown)");
	addtobuffer(response->msgbuf, msg);
	snprintf(msg, sizeof(msg), "rcode: %s\n",
		((rcode >= 0) && (rcode < (int)(sizeof(rcodes) / sizeof(rcodes[0])))) ? rcodes[rcode] : "(unknown)");
	addtobuffer(response->msgbuf, msg);


	/* Display the questions. */
	addtobuffer(response->msgbuf, "Questions:\n");
	qcnt = ares_dns_record_query_cnt(dnsrec);
	for (i = 0; i < qcnt; i++) {
		const char *qname = NULL;
		ares_dns_rec_type_t qtype = 0;
		ares_dns_class_t qclass = 0;

		if (ares_dns_record_query_get(dnsrec, i, &qname, &qtype, &qclass) != ARES_SUCCESS) continue;

		snprintf(msg, sizeof(msg), "\t%-15s.\t", (qname ? qname : ""));
		addtobuffer(response->msgbuf, msg);
		if ((int)qclass != C_IN) {
			snprintf(msg, sizeof(msg), "\t%s", class_name((int)qclass));
			addtobuffer(response->msgbuf, msg);
		}
		snprintf(msg, sizeof(msg), "\t%s\n", type_name((int)qtype));
		addtobuffer(response->msgbuf, msg);
	}

	ancnt = ares_dns_record_rr_cnt(dnsrec, ARES_SECTION_ANSWER);
	nscnt = ares_dns_record_rr_cnt(dnsrec, ARES_SECTION_AUTHORITY);
	arcnt = ares_dns_record_rr_cnt(dnsrec, ARES_SECTION_ADDITIONAL);

	/* Display the answers. */
	addtobuffer(response->msgbuf, "Answers:\n");
	for (i = 0; i < ancnt; i++) {
		dns_render_rr_arespec(ares_dns_record_rr_get_const(dnsrec, ARES_SECTION_ANSWER, i), response);
	}

	/* Display the NS records. */
	addtobuffer(response->msgbuf, "NS records:\n");
	for (i = 0; i < nscnt; i++) {
		dns_render_rr_arespec(ares_dns_record_rr_get_const(dnsrec, ARES_SECTION_AUTHORITY, i), response);
	}

	/* Display the additional records. */
	addtobuffer(response->msgbuf, "Additional records:\n");
	for (i = 0; i < arcnt; i++) {
		dns_render_rr_arespec(ares_dns_record_rr_get_const(dnsrec, ARES_SECTION_ADDITIONAL, i), response);
	}

	ares_dns_record_destroy(dnsrec);
}

static void dns_render_rr_arespec(const ares_dns_rr_t *rr, dns_resp_t *response)
{
	int type, dnsclass, displaytype;
	const char *name;

	if (rr == NULL) return;

	name = ares_dns_rr_get_name(rr);
	type = (int)ares_dns_rr_get_type(rr);
	dnsclass = (int)ares_dns_rr_get_class(rr);
	/*
	 * RAW_RR is c-ares's "I don't have a dedicated parser for this type"
	 * wrapper (e.g. DS/DNSKEY on this c-ares version); the real wire type
	 * number is available separately so the header can still show the
	 * correct type name instead of "(unknown)".
	 */
	displaytype = (type == ARES_REC_TYPE_RAW_RR) ? (int)ares_dns_rr_get_u16(rr, ARES_RR_RAW_RR_TYPE) : type;

	snprintf(msg, sizeof(msg), "\t%-15s.\t%d", (name ? name : ""), ares_dns_rr_get_ttl(rr));
	addtobuffer(response->msgbuf, msg);
	if (dnsclass != C_IN) {
		snprintf(msg, sizeof(msg), "\t%s", class_name(dnsclass));
		addtobuffer(response->msgbuf, msg);
	}
	snprintf(msg, sizeof(msg), "\t%s", type_name(displaytype));
	addtobuffer(response->msgbuf, msg);

	switch (type) {
	  case ARES_REC_TYPE_CNAME:
		snprintf(msg, sizeof(msg), "\t%s.", ares_dns_rr_get_str(rr, ARES_RR_CNAME_CNAME));
		addtobuffer(response->msgbuf, msg);
		break;

	  case ARES_REC_TYPE_NS:
		snprintf(msg, sizeof(msg), "\t%s.", ares_dns_rr_get_str(rr, ARES_RR_NS_NSDNAME));
		addtobuffer(response->msgbuf, msg);
		break;

	  case ARES_REC_TYPE_PTR:
		snprintf(msg, sizeof(msg), "\t%s.", ares_dns_rr_get_str(rr, ARES_RR_PTR_DNAME));
		addtobuffer(response->msgbuf, msg);
		break;

	  case ARES_REC_TYPE_HINFO:
		snprintf(msg, sizeof(msg), "\t%s", ares_dns_rr_get_str(rr, ARES_RR_HINFO_CPU));
		addtobuffer(response->msgbuf, msg);
		snprintf(msg, sizeof(msg), "\t%s", ares_dns_rr_get_str(rr, ARES_RR_HINFO_OS));
		addtobuffer(response->msgbuf, msg);
		break;

	  case ARES_REC_TYPE_MX:
		snprintf(msg, sizeof(msg), "\t%d", ares_dns_rr_get_u16(rr, ARES_RR_MX_PREFERENCE));
		addtobuffer(response->msgbuf, msg);
		snprintf(msg, sizeof(msg), "\t%s.", ares_dns_rr_get_str(rr, ARES_RR_MX_EXCHANGE));
		addtobuffer(response->msgbuf, msg);
		break;

	  case ARES_REC_TYPE_SOA:
		snprintf(msg, sizeof(msg), "\t%s.\n", ares_dns_rr_get_str(rr, ARES_RR_SOA_MNAME));
		addtobuffer(response->msgbuf, msg);
		snprintf(msg, sizeof(msg), "\t\t\t\t\t\t%s.\n", ares_dns_rr_get_str(rr, ARES_RR_SOA_RNAME));
		addtobuffer(response->msgbuf, msg);
		snprintf(msg, sizeof(msg), "\t\t\t\t\t\t( %d %d %d %d %d )",
			(int)ares_dns_rr_get_u32(rr, ARES_RR_SOA_SERIAL),
			(int)ares_dns_rr_get_u32(rr, ARES_RR_SOA_REFRESH),
			(int)ares_dns_rr_get_u32(rr, ARES_RR_SOA_RETRY),
			(int)ares_dns_rr_get_u32(rr, ARES_RR_SOA_EXPIRE),
			(int)ares_dns_rr_get_u32(rr, ARES_RR_SOA_MINIMUM));
		addtobuffer(response->msgbuf, msg);
		break;

	  case ARES_REC_TYPE_TXT:
	  {
		size_t cnt = ares_dns_rr_get_abin_cnt(rr, ARES_RR_TXT_DATA);
		size_t j;
		for (j = 0; j < cnt; j++) {
			size_t len = 0;
			const unsigned char *data = ares_dns_rr_get_abin(rr, ARES_RR_TXT_DATA, j, &len);
			snprintf(msg, sizeof(msg), "\t%.*s", (int)len, (data ? (const char *)data : ""));
			addtobuffer(response->msgbuf, msg);
		}
		break;
	  }

	  case ARES_REC_TYPE_A:
	  {
		const struct in_addr *addr = ares_dns_rr_get_addr(rr, ARES_RR_A_ADDR);
		if (addr) {
			snprintf(msg, sizeof(msg), "\t%s", inet_ntoa(*addr));
			addtobuffer(response->msgbuf, msg);
		}
		break;
	  }

	  case ARES_REC_TYPE_AAAA:
	  {
		const struct ares_in6_addr *addr6 = ares_dns_rr_get_addr6(rr, ARES_RR_AAAA_ADDR);
		if (addr6) {
			struct in6_addr localaddr6;
			memcpy(&localaddr6, addr6, sizeof(localaddr6));
			addtobuffer_many(response->msgbuf, "\t", inet_ntop(AF_INET6, &localaddr6, msg, sizeof(msg)), NULL);
		}
		break;
	  }

	  case ARES_REC_TYPE_SRV:
		snprintf(msg, sizeof(msg), "\t%d", ares_dns_rr_get_u16(rr, ARES_RR_SRV_PRIORITY));
		addtobuffer(response->msgbuf, msg);
		snprintf(msg, sizeof(msg), " %d", ares_dns_rr_get_u16(rr, ARES_RR_SRV_WEIGHT));
		addtobuffer(response->msgbuf, msg);
		snprintf(msg, sizeof(msg), " %d", ares_dns_rr_get_u16(rr, ARES_RR_SRV_PORT));
		addtobuffer(response->msgbuf, msg);
		snprintf(msg, sizeof(msg), "\t%s.", ares_dns_rr_get_str(rr, ARES_RR_SRV_TARGET));
		addtobuffer(response->msgbuf, msg);
		break;

	  case ARES_REC_TYPE_CAA:
	  {
		size_t vlen = 0;
		const unsigned char *val = ares_dns_rr_get_bin(rr, ARES_RR_CAA_VALUE, &vlen);
		snprintf(msg, sizeof(msg), "\t%d %s \"%.*s\"",
			ares_dns_rr_get_u8(rr, ARES_RR_CAA_CRITICAL),
			ares_dns_rr_get_str(rr, ARES_RR_CAA_TAG),
			(int)vlen, (val ? (const char *)val : ""));
		addtobuffer(response->msgbuf, msg);
		break;
	  }

	  case ARES_REC_TYPE_TLSA:
	  {
		size_t dlen = 0;
		const unsigned char *data = ares_dns_rr_get_bin(rr, ARES_RR_TLSA_DATA, &dlen);
		size_t j;
		snprintf(msg, sizeof(msg), "\t%d %d %d ",
			ares_dns_rr_get_u8(rr, ARES_RR_TLSA_CERT_USAGE),
			ares_dns_rr_get_u8(rr, ARES_RR_TLSA_SELECTOR),
			ares_dns_rr_get_u8(rr, ARES_RR_TLSA_MATCH));
		addtobuffer(response->msgbuf, msg);
		for (j = 0; (data != NULL) && (j < dlen); j++) {
			snprintf(msg, sizeof(msg), "%02x", data[j]);
			addtobuffer(response->msgbuf, msg);
		}
		break;
	  }

	  case ARES_REC_TYPE_SVCB:
	  case ARES_REC_TYPE_HTTPS:
	  {
		ares_dns_rr_key_t prikey = (type == ARES_REC_TYPE_SVCB) ? ARES_RR_SVCB_PRIORITY : ARES_RR_HTTPS_PRIORITY;
		ares_dns_rr_key_t tgtkey = (type == ARES_REC_TYPE_SVCB) ? ARES_RR_SVCB_TARGET : ARES_RR_HTTPS_TARGET;
		ares_dns_rr_key_t prmkey = (type == ARES_REC_TYPE_SVCB) ? ARES_RR_SVCB_PARAMS : ARES_RR_HTTPS_PARAMS;
		size_t ocnt = ares_dns_rr_get_opt_cnt(rr, prmkey);
		size_t j;

		snprintf(msg, sizeof(msg), "\t%d\t%s.", ares_dns_rr_get_u16(rr, prikey), ares_dns_rr_get_str(rr, tgtkey));
		addtobuffer(response->msgbuf, msg);
		for (j = 0; j < ocnt; j++) {
			const unsigned char *oval = NULL;
			size_t olen = 0;
			unsigned short okey = ares_dns_rr_get_opt(rr, prmkey, j, &oval, &olen);
			const char *oname = ares_dns_opt_get_name(prmkey, okey);
			size_t k;

			if (oname) snprintf(msg, sizeof(msg), " %s", oname);
			else       snprintf(msg, sizeof(msg), " key%u", okey);
			addtobuffer(response->msgbuf, msg);
			if (olen > 0) {
				addtobuffer(response->msgbuf, "=");
				for (k = 0; (oval != NULL) && (k < olen); k++) {
					snprintf(msg, sizeof(msg), "%02x", oval[k]);
					addtobuffer(response->msgbuf, msg);
				}
			}
		}
		break;
	  }

	  case ARES_REC_TYPE_RAW_RR:
	  {
		/* c-ares has no dedicated field parser for this wire type (e.g.
		 * DS/DNSKEY on this c-ares version); render the raw bytes as hex
		 * rather than a blank "cannot parse" -- displaytype above already
		 * showed the real type name/number in the header. */
		size_t rlen = 0;
		const unsigned char *rdata = ares_dns_rr_get_bin(rr, ARES_RR_RAW_RR_DATA, &rlen);
		size_t j;
		addtobuffer(response->msgbuf, "\t[");
		for (j = 0; (rdata != NULL) && (j < rlen); j++) {
			snprintf(msg, sizeof(msg), "%02x", rdata[j]);
			addtobuffer(response->msgbuf, msg);
		}
		addtobuffer(response->msgbuf, "]");
		break;
	  }

	  default:
		snprintf(msg, sizeof(msg), "\t[Unknown RR; cannot parse]");
		addtobuffer(response->msgbuf, msg);
	}
	snprintf(msg, sizeof(msg), "\n");
	addtobuffer(response->msgbuf, msg);
}
#endif /* XYMON_DNS_USE_ARES_DNS_RECORD */

static const char *type_name(int type)
{
	int i;

	for (i = 0; i < ntypes; i++) {
		if (types[i].value == type) return types[i].name;
	}
	return "(unknown)";
}

static const char *class_name(int dnsclass)
{
	int i;

	for (i = 0; i < nclasses; i++) {
		if (classes[i].value == dnsclass) return classes[i].name;
	}
	return "(unknown)";
}

int dns_name_type(char *name)
{
	int i;

	for (i = 0; i < ntypes; i++) {
		if (strcasecmp(types[i].name, name) == 0) return types[i].value;
	}
	return T_A;
}

