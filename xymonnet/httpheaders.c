#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "httpheaders.h"

#define MAX_HTTPHEADER_FILE_SIZE (64 * 1024)

static int valid_header_name(const char *start, const char *end)
{
	const char *walk;

	if (start == end) return 0;

	for (walk = start; (walk < end); walk++) {
		if (!( (*walk >= '0' && *walk <= '9') ||
		       (*walk >= 'A' && *walk <= 'Z') ||
		       (*walk >= 'a' && *walk <= 'z') ||
		       strchr("!#$%&'*+-.^_`|~", *walk) )) return 0;
	}

	return 1;
}

static int valid_header_value(const char *start, const char *end)
{
	const unsigned char *walk;

	for (walk = (const unsigned char *)start; (walk < (const unsigned char *)end); walk++) {
		if ((*walk < 0x20) && (*walk != '\t')) return 0;
		if (*walk == 0x7f) return 0;
	}

	return 1;
}

char *load_http_headers(const char *filename, char *errmsg, size_t errmsgsz)
{
	FILE *fd;
	struct stat st;
	char *raw = NULL, *result = NULL;
	size_t rawlen, outlen, pos, linestart;

	if (errmsgsz > 0) *errmsg = '\0';

	fd = fopen(filename, "rb");
	if (fd == NULL) {
		snprintf(errmsg, errmsgsz, "Cannot open HTTP header file %s: %s", filename, strerror(errno));
		return NULL;
	}

	if (fstat(fileno(fd), &st) == -1) {
		snprintf(errmsg, errmsgsz, "Cannot stat HTTP header file %s: %s", filename, strerror(errno));
		fclose(fd);
		return NULL;
	}

	if (!S_ISREG(st.st_mode)) {
		snprintf(errmsg, errmsgsz, "HTTP header file %s is not a regular file", filename);
		fclose(fd);
		return NULL;
	}

	if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
		snprintf(errmsg, errmsgsz, "HTTP header file %s must not be accessible by group or others", filename);
		fclose(fd);
		return NULL;
	}

	if ((st.st_size <= 0) || (st.st_size > MAX_HTTPHEADER_FILE_SIZE)) {
		snprintf(errmsg, errmsgsz, "HTTP header file %s must contain 1-%d bytes", filename, MAX_HTTPHEADER_FILE_SIZE);
		fclose(fd);
		return NULL;
	}

	rawlen = (size_t)st.st_size;
	raw = (char *)malloc(rawlen + 1);
	if (raw == NULL) {
		snprintf(errmsg, errmsgsz, "Cannot allocate memory for HTTP header file %s", filename);
		fclose(fd);
		return NULL;
	}

	if (fread(raw, 1, rawlen, fd) != rawlen) {
		snprintf(errmsg, errmsgsz, "Cannot read HTTP header file %s: %s", filename, strerror(errno));
		free(raw);
		fclose(fd);
		return NULL;
	}
	fclose(fd);
	raw[rawlen] = '\0';

	result = (char *)malloc((rawlen * 2) + 1);
	if (result == NULL) {
		snprintf(errmsg, errmsgsz, "Cannot allocate memory for HTTP header file %s", filename);
		free(raw);
		return NULL;
	}

	outlen = pos = linestart = 0;
	while (pos <= rawlen) {
		if ((pos == rawlen) || (raw[pos] == '\n')) {
			size_t lineend = pos;
			char *colon;

			if ((pos == rawlen) && (linestart == rawlen)) break;
			if ((lineend > linestart) && (raw[lineend-1] == '\r')) lineend--;
			if (lineend == linestart) {
				snprintf(errmsg, errmsgsz, "HTTP header file %s contains an empty header line", filename);
				goto invalid;
			}
			colon = memchr(raw + linestart, ':', lineend - linestart);
			if ((colon == NULL) || !valid_header_name(raw + linestart, colon)) {
				snprintf(errmsg, errmsgsz, "HTTP header file %s contains an invalid header name", filename);
				goto invalid;
			}
			if (!valid_header_value(colon + 1, raw + lineend)) {
				snprintf(errmsg, errmsgsz, "HTTP header file %s contains an invalid header value", filename);
				goto invalid;
			}

			if (outlen > 0) {
				result[outlen++] = '\r';
				result[outlen++] = '\n';
			}
			memcpy(result + outlen, raw + linestart, lineend - linestart);
			outlen += lineend - linestart;
			linestart = pos + 1;
		}
		pos++;
	}

	result[outlen] = '\0';
	free(raw);
	return result;

invalid:
	free(result);
	free(raw);
	return NULL;
}