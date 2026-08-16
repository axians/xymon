/*----------------------------------------------------------------------------*/
/* Xymon live status dashboard and JSON endpoint.                             */
/*----------------------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include "libxymon.h"

#define DEFAULT_CACHE_SECONDS 3

static int wants_data(void)
{
	const char *query = getenv("QUERY_STRING");

	while (query && *query) {
		if ((strncmp(query, "data=1", 6) == 0) && ((query[6] == '\0') || (query[6] == '&'))) return 1;
		query = strchr(query, '&');
		if (query) query++;
	}

	return 0;
}

static void html_page(void)
{
	const char *skin = xgetenv("XYMONSKIN");

	printf("Content-Type: %s\nCache-Control: no-store\n\n", xgetenv("HTMLCONTENTTYPE"));
	puts("<!doctype html>");
	puts("<html lang=\"en\"><head><meta charset=\"utf-8\">");
	puts("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
	puts("<meta name=\"color-scheme\" content=\"dark\">");
	puts("<title>Xymon Live</title>");
	printf("<link rel=\"icon\" href=\"%s/favicon-green.ico\">\n", skin);
	printf("<link rel=\"stylesheet\" href=\"%s/xymonlive.css\">\n", skin);
	puts("</head><body>");
	puts("<main><div class=\"top-heading\"><h1>Xymon Live</h1><section class=\"summary\" aria-label=\"Current status totals\">");
	puts("<button class=\"metric compact red\" data-color=\"red\" title=\"Red\"><span class=\"sr-only\">Red</span><strong id=\"count-red\">0</strong></button>");
	puts("<button class=\"metric compact yellow\" data-color=\"yellow\" title=\"Yellow\"><span class=\"sr-only\">Yellow</span><strong id=\"count-yellow\">0</strong></button>");
	puts("<button class=\"metric compact purple\" data-color=\"purple\" title=\"Purple\"><span class=\"sr-only\">Purple</span><strong id=\"count-purple\">0</strong></button>");
	puts("<button class=\"metric compact blue\" data-color=\"blue\" title=\"Blue\"><span class=\"sr-only\">Blue</span><strong id=\"count-blue\">0</strong></button></section><button id=\"top-menu-toggle\" class=\"top-menu-toggle\" type=\"button\" aria-controls=\"top-actions\" aria-expanded=\"false\" aria-label=\"Open top controls\" title=\"Open top controls\"><span></span></button><div id=\"top-actions\" class=\"top-actions\"><div class=\"refresh-options\" role=\"group\" aria-label=\"Refresh interval\"><span>Refresh</span><button type=\"button\" data-interval=\"3000\">3s</button><button type=\"button\" data-interval=\"5000\" aria-pressed=\"true\">5s</button><button type=\"button\" data-interval=\"10000\">10s</button><button type=\"button\" data-interval=\"30000\">30s</button></div><div class=\"connection\"><span id=\"connection-dot\"></span><span id=\"connection-text\">Connecting</span><time id=\"updated\">--:--:--</time></div></div></div>");
	puts("<section class=\"controls-panel\"><div class=\"section-heading controls-heading\"><h2>Search and filter</h2><button class=\"fold\" type=\"button\" data-target=\"toolbar-content\" aria-expanded=\"true\" aria-label=\"Fold search and filter\" title=\"Fold search and filter\"><span></span></button></div>");
	puts("<div id=\"toolbar-content\"><div class=\"toolbar\"><label class=\"search\"><span>Search</span><input id=\"search\" type=\"search\" placeholder=\"Host, test, or message\" autocomplete=\"off\"></label>");
	puts("<label class=\"regex\"><span title=\"Enter plain text or a regular expression. Matching is case-insensitive.\">Filter</span><input id=\"regex-filter\" type=\"text\" placeholder=\"Host or service\" autocomplete=\"off\" aria-describedby=\"regex-error\"></label>");
	puts("<label class=\"regex-scope\"><span>Match</span><select id=\"regex-scope\"><option value=\"host\">Host</option><option value=\"service\">Service</option><option value=\"either\" selected>Host or service</option></select></label>");
	puts("<label class=\"layout\"><span>Layout</span><select id=\"layout\"><option value=\"side-by-side\">Active left</option><option value=\"changes-left\">Recent changes left</option><option value=\"active-above\">Active above</option><option value=\"changes-above\">Recent changes above</option></select></label>");
	puts("<button id=\"pause\" class=\"command\" type=\"button\">Pause</button></div>");
	puts("<div id=\"regex-error\" class=\"filter-error\" role=\"alert\" hidden></div>");
	puts("<div id=\"error\" role=\"alert\" hidden></div></div></section>");
	puts("<div class=\"workspace layout-side-by-side\"><section class=\"status-panel\"><div class=\"section-heading status-heading\"><h2>Active statuses</h2><div class=\"heading-actions\"><span id=\"visible-count\">0 shown</span><button class=\"drag-handle\" type=\"button\" data-panel=\"status\" aria-label=\"Move active statuses\" title=\"Drag to move active statuses\"><span></span></button><button class=\"fold\" type=\"button\" data-target=\"status-content\" aria-expanded=\"true\" aria-label=\"Fold active statuses\" title=\"Fold active statuses\"><span></span></button></div></div>");
	puts("<div id=\"status-content\" class=\"table-wrap\"><table><thead><tr><th>Status</th><th>Host</th><th>Test</th><th>Since</th><th>State</th><th>Summary</th></tr></thead><tbody id=\"status-body\"></tbody></table>");
	puts("<div id=\"empty\" class=\"empty\">No matching active statuses</div></div></section>");
	puts("<aside><div class=\"section-heading\"><h2>Recent changes</h2><div class=\"heading-actions\"><button id=\"clear\" type=\"button\">Clear</button><button class=\"drag-handle\" type=\"button\" data-panel=\"changes\" aria-label=\"Move recent changes\" title=\"Drag to move recent changes\"><span></span></button><button class=\"fold\" type=\"button\" data-target=\"events\" aria-expanded=\"true\" aria-label=\"Fold recent changes\" title=\"Fold recent changes\"><span></span></button></div></div><ol id=\"events\"></ol></aside></div></main>");
	printf("<script src=\"%s/xymonlive.js\"></script>\n", skin);
	puts("</body></html>");
}

static void json_string(FILE *output, const char *value)
{
	const unsigned char *walk = (const unsigned char *)(value ? value : "");

	fputc('"', output);
	while (*walk) {
		switch (*walk) {
		  case '"': fputs("\\\"", output); break;
		  case '\\': fputs("\\\\", output); break;
		  case '\b': fputs("\\b", output); break;
		  case '\f': fputs("\\f", output); break;
		  case '\n': fputs("\\n", output); break;
		  case '\r': fputs("\\r", output); break;
		  case '\t': fputs("\\t", output); break;
		  default:
			if (*walk < 0x20) fprintf(output, "\\u%04x", *walk);
			else fputc(*walk, output);
		}
		walk++;
	}
	fputc('"', output);
}

static unsigned long cache_key(void)
{
	const unsigned char *walk;
	unsigned long result = 5381;
	const char *values[2];
	int index;

	values[0] = xgetenv("XYMONSERVER");
	values[1] = xgetenv("XYMONDPORT");
	for (index = 0; index < 2; index++) {
		walk = (const unsigned char *)(values[index] ? values[index] : "");
		while (*walk) result = ((result << 5) + result) ^ *walk++;
		result = ((result << 5) + result) ^ '|';
	}

	return result;
}

static int copy_file(FILE *input, FILE *output)
{
	char buffer[8192];
	size_t count;

	while ((count = fread(buffer, 1, sizeof(buffer), input)) > 0) {
		if (fwrite(buffer, 1, count, output) != count) return 0;
	}

	return !ferror(input);
}

static int serve_cache(const char *cachefn)
{
	FILE *cache = fopen(cachefn, "r");
	int result;

	if (!cache) return 0;
	printf("Content-Type: application/json\nCache-Control: no-store\n\n");
	result = copy_file(cache, stdout);
	fclose(cache);
	return result;
}

static int cache_is_fresh(const char *cachefn, int cache_seconds)
{
	struct stat st;
	time_t now = getcurrenttime(NULL);

	return ((cache_seconds > 0) && (stat(cachefn, &st) == 0) &&
		(st.st_mtime <= now) && ((now - st.st_mtime) < cache_seconds));
}

static int lock_cache(const char *lockfn, int wait)
{
	struct flock lockinfo;
	int lockfd = open(lockfn, O_WRONLY | O_CREAT, 0660);

	if (lockfd < 0) return -1;
	memset(&lockinfo, 0, sizeof(lockinfo));
	lockinfo.l_type = F_WRLCK;
	lockinfo.l_whence = SEEK_SET;
	if (fcntl(lockfd, wait ? F_SETLKW : F_SETLK, &lockinfo) == -1) {
		close(lockfd);
		return -1;
	}

	return lockfd;
}

static void write_json(FILE *output, char *log, time_t generated)
{
	int first = 1;
	char *line = log;

	fprintf(output, "{\"generated\":%ld,\"statuses\":[", (long)generated);
	while (line && *line) {
		char *hostname, *testname, *color, *lastchange, *logtime;
		char *acktime, *disabletime, *ackmsg, *dismsg, *summary, *eoln;

		eoln = strchr(line, '\n');
		if (eoln) *eoln = '\0';
		hostname = gettok(line, "|");
		testname = hostname ? gettok(NULL, "|") : NULL;
		color = testname ? gettok(NULL, "|") : NULL;
		lastchange = color ? gettok(NULL, "|") : NULL;
		logtime = lastchange ? gettok(NULL, "|") : NULL;
		acktime = logtime ? gettok(NULL, "|") : NULL;
		disabletime = acktime ? gettok(NULL, "|") : NULL;
		ackmsg = disabletime ? gettok(NULL, "|") : NULL;
		dismsg = ackmsg ? gettok(NULL, "|") : NULL;
		summary = dismsg ? gettok(NULL, "|") : NULL;

		if (summary) {
			nldecode(ackmsg);
			nldecode(dismsg);
			if (!first) fputc(',', output);
			first = 0;
			fputs("{\"host\":", output); json_string(output, hostname);
			fputs(",\"test\":", output); json_string(output, testname);
			fputs(",\"color\":", output); json_string(output, color);
			fprintf(output, ",\"lastChange\":%ld,\"logTime\":%ld,\"ackTime\":%ld,\"disableTime\":%ld,\"summary\":",
				atol(lastchange), atol(logtime), atol(acktime), atol(disabletime));
			json_string(output, summary);
			fputs(",\"ackMessage\":", output); json_string(output, ackmsg);
			fputs(",\"disableMessage\":", output); json_string(output, dismsg);
			fputs(",\"hostUrl\":", output); json_string(output, hostsvcurl(hostname, "info", 0));
			fputs(",\"serviceUrl\":", output); json_string(output, hostsvcurl(hostname, testname, 0));
			fputc('}', output);
		}

		if (eoln) {
			*eoln = '\n';
			line = eoln + 1;
		}
		else line = NULL;
	}
	fputs("]}\n", output);
}

int main(int argc, char **argv)
{
	int argi, result, lockfd = -1, cache_seconds = DEFAULT_CACHE_SECONDS;
	char *envarea = NULL;
	char *log;
	char cachefn[PATH_MAX], lockfn[PATH_MAX], tempfn[PATH_MAX];
	const char *tmpdir;
	FILE *cache;
	int tempfd;
	sendreturn_t *response;
	const char *request = "xymondboard color=red,yellow,purple,blue fields=hostname,testname,color,lastchange,logtime,acktime,disabletime,ackmsg,dismsg,line1";

	for (argi = 1; argi < argc; argi++) {
		if (argnmatch(argv[argi], "--env=")) {
			char *value = strchr(argv[argi], '=');
			loadenv(value + 1, envarea);
		}
		else if (argnmatch(argv[argi], "--area=")) {
			char *value = strchr(argv[argi], '=');
			envarea = strdup(value + 1);
		}
		else if (argnmatch(argv[argi], "--cache=")) {
			char *value = strchr(argv[argi], '=');
			cache_seconds = atoi(value + 1);
			if (cache_seconds < 0) cache_seconds = 0;
		}
		else if (strcmp(argv[argi], "--debug") == 0) debug = 1;
	}
	if (!wants_data()) {
		html_page();
		return 0;
	}

	tmpdir = xgetenv("XYMONTMP");
	if (!tmpdir) tmpdir = "/tmp";
	snprintf(cachefn, sizeof(cachefn), "%s/xymonlive-%08lx.json", tmpdir, cache_key());
	snprintf(lockfn, sizeof(lockfn), "%s/xymonlive-%08lx.lock", tmpdir, cache_key());
	if (cache_is_fresh(cachefn, cache_seconds)) return serve_cache(cachefn) ? 0 : 1;

	if (cache_seconds > 0) {
		lockfd = lock_cache(lockfn, 0);
		if (lockfd < 0) {
			if (serve_cache(cachefn)) return 0;
			lockfd = lock_cache(lockfn, 1);
			if (lockfd < 0) errprintf("Cannot lock xymonlive cache %s: %s\n", lockfn, strerror(errno));
		}
		if ((lockfd >= 0) && cache_is_fresh(cachefn, cache_seconds)) {
			close(lockfd);
			return serve_cache(cachefn) ? 0 : 1;
		}
	}

	response = newsendreturnbuf(1, NULL);
	result = sendmessage((char *)request, NULL, XYMON_TIMEOUT, response);
	if (result != XYMONSEND_OK) {
		if (lockfd >= 0) close(lockfd);
		if (serve_cache(cachefn)) {
			freesendreturnbuf(response);
			return 0;
		}
		printf("Status: 503\nContent-Type: application/json\nCache-Control: no-store\n\n");
		printf("{\"error\":\"Xymon status is unavailable\",\"result\":%d}\n", result);
		freesendreturnbuf(response);
		return 1;
	}

	log = getsendreturnstr(response, 1);
	freesendreturnbuf(response);
	if ((cache_seconds > 0) && (lockfd >= 0)) {
		snprintf(tempfn, sizeof(tempfn), "%s/xymonlive-%08lx.XXXXXX", tmpdir, cache_key());
		tempfd = mkstemp(tempfn);
		cache = (tempfd >= 0) ? fdopen(tempfd, "w") : NULL;
		if (cache) {
			write_json(cache, log, getcurrenttime(NULL));
			if ((fclose(cache) == 0) && (rename(tempfn, cachefn) == 0)) {
				close(lockfd);
				if (log) xfree(log);
				return serve_cache(cachefn) ? 0 : 1;
			}
			unlink(tempfn);
		}
		else {
			if (tempfd >= 0) close(tempfd);
			errprintf("Cannot create xymonlive cache %s: %s\n", tempfn, strerror(errno));
		}
		close(lockfd);
	}

	printf("Content-Type: application/json\nCache-Control: no-store\n\n");
	write_json(stdout, log, getcurrenttime(NULL));
	if (log) xfree(log);
	return 0;
}