/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef __XYMONPROXY_FILTER_H__
#define __XYMONPROXY_FILTER_H__

#include <stddef.h>

typedef struct proxy_filter_t proxy_filter_t;

extern proxy_filter_t *proxy_filter_create(void);
extern void proxy_filter_destroy(proxy_filter_t *filter);
extern int proxy_filter_add_commands(proxy_filter_t *filter, const char *spec,
				     char *error, size_t error_size);
extern int proxy_filter_add_hostname(proxy_filter_t *filter, const char *hostname,
				     char *error, size_t error_size);
extern int proxy_filter_allows(const proxy_filter_t *filter, const char *message);

#endif