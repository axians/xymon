/*----------------------------------------------------------------------------*/
/* Xymon monitor network test tool.                                           */
/*                                                                            */
/* HTTP/2 support for xymonnet's HTTP tests, built on top of libnghttp2.      */
/*                                                                            */
/* When xymonnet is built without nghttp2 (HAVE_NGHTTP2 undefined) the        */
/* functions here compile to stubs that report HTTP/2 as unavailable, so the  */
/* rest of xymonnet can be built and linked unchanged.                        */
/*                                                                            */
/* Copyright (C) 2003-2011 Henrik Storner <henrik@hswn.dk>                    */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*----------------------------------------------------------------------------*/

#include "config.h"

#include "http2.h"

#ifndef HAVE_NGHTTP2

int http2_available(void)                       { return 0; }
int http2_start(void *item)                     { return -1; }
int http2_senddata(void *item)                  { return -1; }
int http2_recvdata(void *item, char *buf, int len) { return -1; }
void http2_cleanup(void *item)                  { }

#endif  /* !HAVE_NGHTTP2 */
