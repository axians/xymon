#ifndef __HTTPHEADERS_H_
#define __HTTPHEADERS_H_

#include <stddef.h>

extern char *load_http_headers(const char *filename, char *errmsg, size_t errmsgsz);

#endif