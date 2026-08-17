#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "httpheaders.h"

static int failures = 0;

static void expect(int condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		failures++;
	}
}

static void write_fixture(const char *filename, const void *data, size_t datalen, mode_t mode)
{
	FILE *fd = fopen(filename, "wb");

	if ((fd == NULL) || (fwrite(data, 1, datalen, fd) != datalen) || (fclose(fd) != 0)) {
		perror(filename);
		exit(2);
	}
	if (chmod(filename, mode) == -1) {
		perror(filename);
		exit(2);
	}
}

int main(void)
{
	char template[] = "/tmp/xymon-httpheaders.XXXXXX";
	char *workdir, filename[sizeof(template) + 16], errmsg[1024], *headers;
	static const char valid[] = "Authorization: Bearer secret\nX-API-Key: another-secret\n";
	static const char blank[] = "Authorization: Bearer secret\n\nInjected: value\n";
	static const char badname[] = "Bad Header: value\n";
	static const char nulvalue[] = "Authorization: Bearer bad\0value\n";

	workdir = mkdtemp(template);
	if (workdir == NULL) { perror("mkdtemp"); return 2; }
	snprintf(filename, sizeof(filename), "%s/headers", workdir);

	write_fixture(filename, valid, sizeof(valid) - 1, 0600);
	headers = load_http_headers(filename, errmsg, sizeof(errmsg));
	expect(headers != NULL, "valid protected header file was rejected");
	if (headers) {
		expect(strcmp(headers, "Authorization: Bearer secret\r\nX-API-Key: another-secret") == 0,
		       "headers were not normalized to HTTP CRLF separators");
		free(headers);
	}

	chmod(filename, 0640);
	headers = load_http_headers(filename, errmsg, sizeof(errmsg));
	expect(headers == NULL, "group-readable header file was accepted");
	expect(strstr(errmsg, "must not be accessible by group or others") != NULL,
	       "unsafe permissions did not produce a useful error");

	write_fixture(filename, blank, sizeof(blank) - 1, 0600);
	headers = load_http_headers(filename, errmsg, sizeof(errmsg));
	expect(headers == NULL, "header file with an empty line was accepted");

	write_fixture(filename, badname, sizeof(badname) - 1, 0600);
	headers = load_http_headers(filename, errmsg, sizeof(errmsg));
	expect(headers == NULL, "invalid HTTP header name was accepted");

	write_fixture(filename, nulvalue, sizeof(nulvalue) - 1, 0600);
	headers = load_http_headers(filename, errmsg, sizeof(errmsg));
	expect(headers == NULL, "NUL byte in HTTP header value was accepted");

	unlink(filename);
	rmdir(workdir);
	return failures ? 1 : 0;
}