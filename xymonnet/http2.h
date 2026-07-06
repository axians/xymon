/*----------------------------------------------------------------------------*/
/* Xymon monitor network test tool.                                           */
/*                                                                            */
/* HTTP/2 support for xymonnet's HTTP tests, built on top of libnghttp2.      */
/* The functions here translate between xymonnet's existing HTTP/1.1 request  */
/* text and response-parsing code and the binary HTTP/2 framing spoken on the */
/* wire, so the rest of the HTTP test logic is unaware of the protocol used.  */
/*                                                                            */
/* Copyright (C) 2003-2011 Henrik Storner <henrik@hswn.dk>                    */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*----------------------------------------------------------------------------*/

#ifndef __HTTP2_H_
#define __HTTP2_H_

/* Returns 1 if this build has HTTP/2 support compiled in, 0 otherwise. */
extern int http2_available(void);

/*
 * Start an HTTP/2 exchange on an already-TLS-connected test item whose ALPN
 * negotiated "h2". Sets up the nghttp2 session and submits the request that
 * was built (as HTTP/1.1 text) in item->sendtxt. Returns 0 on success, -1 on
 * error (item->errcode is set).
 */
extern int http2_start(void *item);

/*
 * Flush any pending HTTP/2 output frames to the socket. Returns 0 when all
 * currently-queued output has been written, 1 if more remains to be sent
 * later, -1 on I/O error.
 */
extern int http2_senddata(void *item);

/*
 * Feed inbound bytes to the nghttp2 session. Response headers/body are
 * synthesized back into HTTP/1.1-shaped text and pushed through the item's
 * datacallback. Returns 1 when the response stream is complete, 0 if more
 * data is expected, -1 on error.
 */
extern int http2_recvdata(void *item, char *buf, int len);

/* Free the nghttp2 session and any buffers attached to the item. */
extern void http2_cleanup(void *item);

#endif
