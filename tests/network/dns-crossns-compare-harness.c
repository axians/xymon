/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Drives the real dns2.c cross-nameserver response comparator. */

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ares.h>
#include <ares_version.h>
#if ARES_VERSION >= 0x012000
#include <ares_dns_record.h>
#define TEST_STRUCTURED_DNS 1
#endif

const char *xfreenullstr = "xfree called with NULL\n";

void errprintf(const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
}

#ifdef TEST_STRUCTURED_DNS
extern int dns_response_content_equal(ares_dns_record_t *a, ares_dns_record_t *b,
				      int blank_soa_serial);
#endif
extern int dns_response_matches(const char *response, const char *pattern,
				char *error, size_t errorlen);
extern int dns_decode_content_pattern(char *pattern, unsigned char **decoded,
				      char *error, size_t errorlen);

#ifdef TEST_STRUCTURED_DNS
static ares_dns_record_t *make_caa(const char *owner, ares_dns_rcode_t rcode,
				   const char *value, unsigned char glue)
{
	ares_dns_record_t *dnsrec = NULL;
	ares_dns_rr_t *rr = NULL;
	struct in_addr addr;

	if (ares_dns_record_create(&dnsrec, 1, ARES_FLAG_QR, ARES_OPCODE_QUERY, rcode) != ARES_SUCCESS)
		return NULL;
	if (ares_dns_record_rr_add(&rr, dnsrec, ARES_SECTION_ANSWER, owner,
				   ARES_REC_TYPE_CAA, ARES_CLASS_IN, 300) != ARES_SUCCESS)
		return NULL;
	ares_dns_rr_set_u8(rr, ARES_RR_CAA_CRITICAL, 0);
	ares_dns_rr_set_str(rr, ARES_RR_CAA_TAG, "issue");
	ares_dns_rr_set_bin(rr, ARES_RR_CAA_VALUE,
			    (const unsigned char *)value, strlen(value));

	if (ares_dns_record_rr_add(&rr, dnsrec, ARES_SECTION_ADDITIONAL, "glue.example",
				   ARES_REC_TYPE_A, ARES_CLASS_IN, 60) != ARES_SUCCESS)
		return NULL;
	memset(&addr, glue, sizeof(addr));
	ares_dns_rr_set_addr(rr, ARES_RR_A_ADDR, &addr);
	return dnsrec;
}
#endif

static int check(const char *name, int condition)
{
	if (condition) return 0;
	fprintf(stderr, "FAIL %s\n", name);
	return 1;
}

int main(void)
{
#ifdef TEST_STRUCTURED_DNS
	ares_dns_record_t *base = make_caa("Example.COM", ARES_RCODE_NOERROR, "letsencrypt.org", 1);
	ares_dns_record_t *same = make_caa("example.com", ARES_RCODE_NOERROR, "letsencrypt.org", 2);
	ares_dns_record_t *rdata = make_caa("example.com", ARES_RCODE_NOERROR, "other.example", 1);
	ares_dns_record_t *rcode = make_caa("example.com", ARES_RCODE_SERVFAIL, "letsencrypt.org", 1);
#endif
	char error[128];
	char escaped_pattern[] = "192\\\\.0\\\\.2\\\\.10";
	char escaped_newline[] = "first\\nsecond";
	char unknown_escape[] = "192\\.0";
	char invalid_hex_escape[] = "value\\xZZ";
	char trailing_escape[] = "value\\";
	char nul_escape[] = "value\\x00ignored";
	unsigned char *decoded_pattern = NULL;
	unsigned char *decoded_newline = NULL;
	unsigned char *decoded_nul = NULL;
	int failures = 0;

#ifdef TEST_STRUCTURED_DNS
	failures += check("records created", base && same && rdata && rcode);
	if (!failures) {
		failures += check("DNS name case and additional glue are ignored",
			dns_response_content_equal(base, same, 0));
		failures += check("CAA RDATA differences are detected",
			!dns_response_content_equal(base, rdata, 0));
		failures += check("RCODE differences are detected",
			!dns_response_content_equal(base, rcode, 0));
	}
#endif
	failures += check("valid Xymon regex escaping is accepted",
		dns_decode_content_pattern(escaped_pattern, &decoded_pattern, error, sizeof(error)) == 1);
	failures += check("valid Xymon newline escaping is accepted",
		dns_decode_content_pattern(escaped_newline, &decoded_newline, error, sizeof(error)) == 1);
	failures += check("Xymon escaping preserves a POSIX literal dot",
		strcmp((char *)decoded_pattern, "192\\.0\\.2\\.10") == 0);
	failures += check("Xymon newline escaping is decoded",
		strcmp((char *)decoded_newline, "first\nsecond") == 0);
	failures += check("decoded DNS content expression matches",
		dns_response_matches("Questions:\nexample.com. A\nAnswers:\nexample.com. A 192.0.2.10\nNS records:\n", (char *)decoded_pattern,
				     error, sizeof(error)) == 1);
	failures += check("unknown Xymon escape is rejected",
		dns_decode_content_pattern(unknown_escape, &decoded_pattern, error, sizeof(error)) == 0 && error[0]);
	failures += check("invalid Xymon hex escape is rejected",
		dns_decode_content_pattern(invalid_hex_escape, &decoded_pattern, error, sizeof(error)) == 0 && error[0]);
	failures += check("trailing Xymon escape is rejected",
		dns_decode_content_pattern(trailing_escape, &decoded_pattern, error, sizeof(error)) == 0 && error[0]);
	failures += check("NUL Xymon escape is rejected without returning a pattern",
		dns_decode_content_pattern(nul_escape, &decoded_nul, error, sizeof(error)) == 0 &&
		decoded_nul == NULL && error[0]);
	failures += check("question content cannot satisfy a DNS content expression",
		dns_response_matches("Questions:\nverification=ready\nAnswers:\nNS records:\n", "verification=ready",
				     error, sizeof(error)) == 0);
	failures += check("authority content can satisfy a DNS content expression",
		dns_response_matches("Answers:\nNS records:\nauthority=ready\nAdditional records:\n", "authority=ready",
				     error, sizeof(error)) == 1);
	failures += check("additional content can satisfy a DNS content expression",
		dns_response_matches("Answers:\nNS records:\nAdditional records:\nadditional=ready\n", "additional=ready",
				     error, sizeof(error)) == 1);
	failures += check("rendered response content matches",
		dns_response_matches("Answers:\nexample.com. TXT verification=ready\nNS records:\n", "verification=ready", error, sizeof(error)) == 1);
	failures += check("A response content matches",
		dns_response_matches("Answers:\nexample.com. A 192.0.2.10\nNS records:\n", "192\\.0\\.2\\.10", error, sizeof(error)) == 1);
	failures += check("MX response content matches",
		dns_response_matches("Answers:\nexample.com. MX 10 mail.example.com.\nNS records:\n", "10[[:space:]]+mail\\.example\\.com", error, sizeof(error)) == 1);
	failures += check("raw response content matches",
		dns_response_matches("Answers:\nexample.com. DS [0123456789abcdef]\nNS records:\n", "[0-9a-f]{16}", error, sizeof(error)) == 1);
	failures += check("missing response content does not match",
		dns_response_matches("Answers:\nexample.com. TXT verification=wrong\nNS records:\n", "verification=ready", error, sizeof(error)) == 0);
	failures += check("invalid content expression is rejected",
		dns_response_matches("Answers:\nexample.com. TXT verification=ready\nNS records:\n", "[", error, sizeof(error)) == -1 && error[0]);
	free(decoded_pattern);
	free(decoded_newline);

#ifdef TEST_STRUCTURED_DNS
	if (base) ares_dns_record_destroy(base);
	if (same) ares_dns_record_destroy(same);
	if (rdata) ares_dns_record_destroy(rdata);
	if (rcode) ares_dns_record_destroy(rcode);
#endif
	return failures ? 1 : 0;
}