/*----------------------------------------------------------------------------*/
/* Xymon monitor network test tool.                                           */
/*                                                                            */
/* HTTP/2 support for xymonnet's HTTP tests, built on top of libnghttp2.      */
/*                                                                            */
/* xymonnet builds its HTTP request as ordinary HTTP/1.1 text and parses the  */
/* response as HTTP/1.1 text. This module sits in between when a connection   */
/* negotiated ALPN "h2": it re-parses the generated HTTP/1.1 request into an  */
/* HTTP/2 request submitted through libnghttp2, and it re-assembles the       */
/* HTTP/2 response back into HTTP/1.1-shaped text that is fed to the existing  */
/* response callback. That way none of the surrounding HTTP test logic needs  */
/* to know which protocol version was actually used on the wire.             */
/*                                                                            */
/* When built without nghttp2 (or without OpenSSL, since h2 here is only ever */
/* spoken over TLS) these functions compile to stubs reporting "unavailable". */
/*                                                                            */
/* Copyright (C) 2003-2011 Henrik Storner <henrik@hswn.dk>                    */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*----------------------------------------------------------------------------*/

#include "config.h"

#include "http2.h"

#if defined(HAVE_NGHTTP2) && defined(HAVE_OPENSSL)

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#include <nghttp2/nghttp2.h>

#include "contest.h"

/*
 * Per-connection HTTP/2 state, hung off tcptest_t->h2session as a void*.
 */
typedef struct h2conn_t {
	nghttp2_session *session;
	tcptest_t *item;
	int32_t stream_id;

	/* Request body (for POST/SOAP), owned by the caller's sendtxt buffer */
	const unsigned char *reqbody;
	size_t reqbodylen;
	size_t reqbodyofs;

	/* Outbound bytes produced by nghttp2 but not yet written to the socket */
	unsigned char *pend;
	size_t pendlen;
	size_t pendofs;

	/* Response being assembled */
	int status;			/* :status from the response */
	strbuffer_t *resphdrs;		/* collected "name: value\r\n" lines */
	strbuffer_t *respbody;		/* response body bytes */
	int stream_closed;		/* set when the response stream ends */
	int response_emitted;		/* guard against emitting twice */
} h2conn_t;


static int h2_ssl_write(h2conn_t *h2, const unsigned char *buf, size_t len)
{
	tcptest_t *item = h2->item;
	int res;

	res = SSL_write(item->ssldata, buf, len);
	if (res < 0) {
		switch (SSL_get_error(item->ssldata, res)) {
		  case SSL_ERROR_WANT_READ:
		  case SSL_ERROR_WANT_WRITE:
			return 0;	/* Try again later */
		  default:
			return -1;	/* Hard error */
		}
	}

	item->byteswritten += res;
	tcp_stats_written += res;
	return res;
}


/* nghttp2 data-provider callback: hand the request body to the library. */
static ssize_t h2_body_read_cb(nghttp2_session *session, int32_t stream_id,
			       uint8_t *buf, size_t length, uint32_t *data_flags,
			       nghttp2_data_source *source, void *user_data)
{
	h2conn_t *h2 = (h2conn_t *)source->ptr;
	size_t remaining = h2->reqbodylen - h2->reqbodyofs;
	size_t n = (remaining < length) ? remaining : length;

	if (n > 0) {
		memcpy(buf, h2->reqbody + h2->reqbodyofs, n);
		h2->reqbodyofs += n;
	}
	if (h2->reqbodyofs >= h2->reqbodylen) *data_flags |= NGHTTP2_DATA_FLAG_EOF;

	return (ssize_t)n;
}


static int h2_on_header_cb(nghttp2_session *session, const nghttp2_frame *frame,
			   const uint8_t *name, size_t namelen,
			   const uint8_t *value, size_t valuelen,
			   uint8_t flags, void *user_data)
{
	h2conn_t *h2 = (h2conn_t *)user_data;

	if (frame->hd.type != NGHTTP2_HEADERS) return 0;
	if (frame->headers.cat != NGHTTP2_HCAT_RESPONSE) return 0;

	if ((namelen == 7) && (memcmp(name, ":status", 7) == 0)) {
		h2->status = atoi((const char *)value);
		return 0;
	}

	/* Skip HTTP/2 pseudo-headers and length/framing headers we regenerate. */
	if ((namelen > 0) && (name[0] == ':')) return 0;
	if ((namelen == 14) && (strncasecmp((const char *)name, "content-length", 14) == 0)) return 0;
	if ((namelen == 17) && (strncasecmp((const char *)name, "transfer-encoding", 17) == 0)) return 0;

	addtobufferraw(h2->resphdrs, (char *)name, namelen);
	addtobuffer(h2->resphdrs, ": ");
	addtobufferraw(h2->resphdrs, (char *)value, valuelen);
	addtobuffer(h2->resphdrs, "\r\n");

	return 0;
}


static int h2_on_data_chunk_cb(nghttp2_session *session, uint8_t flags,
			       int32_t stream_id, const uint8_t *data,
			       size_t len, void *user_data)
{
	h2conn_t *h2 = (h2conn_t *)user_data;

	addtobufferraw(h2->respbody, (char *)data, len);
	return 0;
}


static int h2_on_stream_close_cb(nghttp2_session *session, int32_t stream_id,
				 uint32_t error_code, void *user_data)
{
	h2conn_t *h2 = (h2conn_t *)user_data;

	if (stream_id == h2->stream_id) h2->stream_closed = 1;
	return 0;
}


/*
 * Re-parse the HTTP/1.1 request text in item->sendtxt into HTTP/2 header
 * name/value pairs and submit the request. request text format is
 * deterministic (we generated it in httptest.c):
 *   METHOD SP PATH SP HTTP/1.1 CRLF
 *   Name: Value CRLF   (repeated)
 *   CRLF
 *   [body]
 */
static int h2_submit_request(h2conn_t *h2)
{
	char *reqcopy, *line, *saveptr, *body = NULL;
	char *method = NULL, *path = NULL, *authority = NULL, *scheme = "https";
	nghttp2_nv *nva = NULL;
	int nvcount = 0, nvmax = 8;
	int32_t sid;
	nghttp2_data_provider data_prd, *data_prd_ptr = NULL;

	reqcopy = strdup((char *)h2->item->sendtxt);

	/* Split headers from body at the blank line. */
	body = strstr(reqcopy, "\r\n\r\n");
	if (body) { *body = '\0'; body += 4; }

	/*
	 * If there is a request body, keep a private heap copy alive for the
	 * data-provider callback (which runs later, during http2_senddata,
	 * after reqcopy is freed).
	 */
	if (body && *body) {
		h2->reqbody = (const unsigned char *)strdup(body);
		h2->reqbodylen = strlen(body);
		h2->reqbodyofs = 0;
		data_prd.source.ptr = h2;
		data_prd.read_callback = h2_body_read_cb;
		data_prd_ptr = &data_prd;
	}

	nva = (nghttp2_nv *)calloc(nvmax, sizeof(nghttp2_nv));

	/* First line: METHOD PATH HTTP/x */
	line = strtok_r(reqcopy, "\r\n", &saveptr);
	if (line) {
		method = strtok(line, " ");
		if (method) path = strtok(NULL, " ");
	}
	if (!method || !path) { xfree(reqcopy); free(nva); return -1; }

	/* Remaining lines: headers */
	while ((line = strtok_r(NULL, "\r\n", &saveptr)) != NULL) {
		char *colon = strchr(line, ':');
		char *hname, *hval;
		size_t hnlen;

		if (!colon) continue;
		*colon = '\0';
		hname = line;
		hval = colon + 1;
		while (*hval == ' ' || *hval == '\t') hval++;
		hnlen = strlen(hname);

		/* Host becomes the :authority pseudo-header. */
		if (strcasecmp(hname, "Host") == 0) { authority = hval; continue; }

		/* Connection-specific headers are forbidden in HTTP/2. */
		if (strcasecmp(hname, "Connection") == 0) continue;
		if (strcasecmp(hname, "Keep-Alive") == 0) continue;
		if (strcasecmp(hname, "Proxy-Connection") == 0) continue;
		if (strcasecmp(hname, "Transfer-Encoding") == 0) continue;
		if (strcasecmp(hname, "Upgrade") == 0) continue;
		/* Length is implied by the DATA framing; drop it to avoid conflicts. */
		if (strcasecmp(hname, "Content-Length") == 0) continue;

		/* HTTP/2 requires lowercase header field names. */
		{ char *lc; for (lc = hname; *lc; lc++) *lc = tolower((int)*lc); }

		if (nvcount >= nvmax) {
			nvmax *= 2;
			nva = (nghttp2_nv *)realloc(nva, nvmax * sizeof(nghttp2_nv));
		}
		nva[nvcount].name = (uint8_t *)hname;
		nva[nvcount].namelen = hnlen;
		nva[nvcount].value = (uint8_t *)hval;
		nva[nvcount].valuelen = strlen(hval);
		nva[nvcount].flags = NGHTTP2_NV_FLAG_NONE;
		nvcount++;
	}

	/*
	 * Prepend the required pseudo-headers. We build a second array so the
	 * pseudo-headers come first (HTTP/2 requires that ordering).
	 */
	{
		nghttp2_nv *full = (nghttp2_nv *)calloc(nvcount + 4, sizeof(nghttp2_nv));
		int i, n = 0;

#define ADD_PSEUDO(nm, vl) do { \
		full[n].name = (uint8_t *)(nm); full[n].namelen = strlen(nm); \
		full[n].value = (uint8_t *)(vl); full[n].valuelen = strlen(vl); \
		full[n].flags = NGHTTP2_NV_FLAG_NONE; n++; } while (0)

		ADD_PSEUDO(":method", method);
		ADD_PSEUDO(":path", path);
		ADD_PSEUDO(":scheme", scheme);
		if (authority) ADD_PSEUDO(":authority", authority);
#undef ADD_PSEUDO

		for (i = 0; i < nvcount; i++) full[n++] = nva[i];

		sid = nghttp2_submit_request(h2->session, NULL, full, n, data_prd_ptr, h2);
		free(full);
	}

	free(nva);
	xfree(reqcopy);

	if (sid < 0) return -1;
	h2->stream_id = sid;
	return 0;
}


int http2_available(void)
{
	return 1;
}


int http2_start(void *itemv)
{
	tcptest_t *item = (tcptest_t *)itemv;
	h2conn_t *h2;
	nghttp2_session_callbacks *cbs;
	nghttp2_settings_entry iv[1];

	h2 = (h2conn_t *)calloc(1, sizeof(h2conn_t));
	h2->item = item;
	h2->stream_id = -1;
	h2->status = 0;
	h2->resphdrs = newstrbuffer(0);
	h2->respbody = newstrbuffer(0);
	item->h2session = h2;

	nghttp2_session_callbacks_new(&cbs);
	nghttp2_session_callbacks_set_on_header_callback(cbs, h2_on_header_cb);
	nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, h2_on_data_chunk_cb);
	nghttp2_session_callbacks_set_on_stream_close_callback(cbs, h2_on_stream_close_cb);

	if (nghttp2_session_client_new(&h2->session, cbs, h2) != 0) {
		nghttp2_session_callbacks_del(cbs);
		item->errcode = CONTEST_ESSL;
		return -1;
	}
	nghttp2_session_callbacks_del(cbs);

	/* HTTP/2 connections must open with a SETTINGS frame. */
	iv[0].settings_id = NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS;
	iv[0].value = 100;
	nghttp2_submit_settings(h2->session, NGHTTP2_FLAG_NONE, iv, 1);

	if (h2_submit_request(h2) != 0) {
		item->errcode = CONTEST_ESSL;
		return -1;
	}

	return 0;
}


int http2_senddata(void *itemv)
{
	tcptest_t *item = (tcptest_t *)itemv;
	h2conn_t *h2 = (h2conn_t *)item->h2session;
	const uint8_t *data;
	ssize_t n;

	if (!h2) return -1;

	/* Flush any previously-buffered partial write first. */
	if (h2->pendlen > h2->pendofs) {
		int w = h2_ssl_write(h2, h2->pend + h2->pendofs, h2->pendlen - h2->pendofs);
		if (w < 0) return -1;
		h2->pendofs += w;
		if (h2->pendofs < h2->pendlen) return 1;	/* Still more to send */
		xfree(h2->pend); h2->pend = NULL; h2->pendlen = h2->pendofs = 0;
	}

	while ((n = nghttp2_session_mem_send(h2->session, &data)) > 0) {
		int w = h2_ssl_write(h2, data, n);
		if (w < 0) return -1;
		if (w < n) {
			/* Socket not ready for the rest - buffer it for later. */
			h2->pendlen = n - w;
			h2->pendofs = 0;
			h2->pend = (unsigned char *)malloc(h2->pendlen);
			memcpy(h2->pend, data + w, h2->pendlen);
			return 1;
		}
	}
	if (n < 0) return -1;

	/* All queued output written; is there more the session wants to send? */
	return (nghttp2_session_want_write(h2->session) ? 1 : 0);
}


/*
 * Assemble the collected response into HTTP/1.1-shaped text and push it
 * through the item's data callback, exactly as if it had arrived from an
 * HTTP/1.1 server. A synthetic Content-Length lets the existing parser know
 * when the body is complete.
 */
static void h2_emit_response(h2conn_t *h2)
{
	tcptest_t *item = h2->item;
	strbuffer_t *hdr;
	char statusline[64];
	char clen[64];

	if (h2->response_emitted) return;
	h2->response_emitted = 1;

	hdr = newstrbuffer(0);
	snprintf(statusline, sizeof(statusline), "HTTP/2 %d \r\n", h2->status);
	addtobuffer(hdr, statusline);
	if (STRBUFLEN(h2->resphdrs) > 0) addtostrbuffer(hdr, h2->resphdrs);
	snprintf(clen, sizeof(clen), "Content-Length: %d\r\n", (int)STRBUFLEN(h2->respbody));
	addtobuffer(hdr, clen);
	addtobuffer(hdr, "\r\n");

	if (item->datacallback) {
		item->datacallback((unsigned char *)STRBUF(hdr), STRBUFLEN(hdr), item->priv);
		if (STRBUFLEN(h2->respbody) > 0)
			item->datacallback((unsigned char *)STRBUF(h2->respbody), STRBUFLEN(h2->respbody), item->priv);
	}

	freestrbuffer(hdr);
}


int http2_recvdata(void *itemv, char *buf, int len)
{
	tcptest_t *item = (tcptest_t *)itemv;
	h2conn_t *h2 = (h2conn_t *)item->h2session;
	ssize_t r;

	if (!h2) return -1;

	r = nghttp2_session_mem_recv(h2->session, (const uint8_t *)buf, len);
	if (r < 0) { item->errcode = CONTEST_EIO; return -1; }

	/* Receiving may have queued SETTINGS-ack / WINDOW_UPDATE / PING output. */
	if (http2_senddata(item) < 0) return -1;

	if (h2->stream_closed) {
		h2_emit_response(h2);
		return 1;
	}
	return 0;
}


void http2_cleanup(void *itemv)
{
	tcptest_t *item = (tcptest_t *)itemv;
	h2conn_t *h2 = (h2conn_t *)item->h2session;

	if (!h2) return;

	if (h2->session) nghttp2_session_del(h2->session);
	if (h2->resphdrs) freestrbuffer(h2->resphdrs);
	if (h2->respbody) freestrbuffer(h2->respbody);
	if (h2->pend) xfree(h2->pend);
	if (h2->reqbody) free((void *)h2->reqbody);
	xfree(h2);
	item->h2session = NULL;
}

#else	/* !(HAVE_NGHTTP2 && HAVE_OPENSSL) */

int http2_available(void)                          { return 0; }
int http2_start(void *item)                        { return -1; }
int http2_senddata(void *item)                     { return -1; }
int http2_recvdata(void *item, char *buf, int len) { return -1; }
void http2_cleanup(void *item)                     { }

#endif
