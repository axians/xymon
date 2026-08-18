/*----------------------------------------------------------------------------*/
/* Xymon live status-change dashboard.                                        */
/*----------------------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libxymon.h"

static void html_page(void)
{
	const char *skin = xgetenv("XYMONSKIN");
	const char *cgiurl = xgetenv("CGIBINURL");

	printf("Content-Type: %s\nCache-Control: no-store\n\n", xgetenv("HTMLCONTENTTYPE"));
	puts("<!doctype html>");
	puts("<html lang=\"en\"><head><meta charset=\"utf-8\">");
	puts("<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
	puts("<meta name=\"color-scheme\" content=\"dark\">");
	puts("<title>Xymon Live</title>");
	printf("<link rel=\"icon\" href=\"%s/favicon-green.ico\">\n", skin);
	printf("<link rel=\"stylesheet\" href=\"%s/xymonlive.css\">\n", skin);
	printf("</head><body><main data-cgi-url=\"%s\" data-websocket-path=\"%s/xymonlive-ws\">\n", cgiurl, cgiurl);
	puts("<section class=\"controls-panel\"><div class=\"section-heading controls-heading\"><h2>Event controls</h2><div class=\"heading-actions live-actions\" title=\"Green means connected to the live event stream; red means disconnected and retrying; yellow means connecting or paused. Pause stops updates and reconnect attempts.\"><h1>Xymon Live</h1><div class=\"connection\"><span id=\"connection-dot\"></span><span id=\"connection-text\">Connecting</span><time id=\"updated\">0000-00-00 --:--:--</time></div><button class=\"fold\" type=\"button\" data-target=\"controls-content\" aria-expanded=\"true\" aria-label=\"Fold event controls\" title=\"Fold event controls\"><span></span></button></div></div>");
	puts("<div id=\"controls-content\"><div class=\"toolbar\"><label class=\"regex\"><span>Filter</span><input id=\"regex-filter\" type=\"text\" placeholder=\"Plain text or regular expression\" autocomplete=\"off\" aria-describedby=\"regex-error\" title=\"Filter host or service names with case-insensitive plain text or a regular expression\"></label>");
	puts("<label class=\"regex-scope\"><span>Match</span><select id=\"regex-scope\" title=\"Choose whether Filter matches host, service, message, or any field\"><option value=\"host\">Host</option><option value=\"service\">Service</option><option value=\"message\">Message</option><option value=\"any\" selected>Any</option></select></label>");
	puts("<label class=\"event-kind\"><span>Type</span><select id=\"event-kind\" title=\"Choose status transitions, disable comments, acknowledgements, or all events\"><option value=\"all\" selected>All</option><option value=\"status\">Status</option><option value=\"disable\">Disable</option><option value=\"ack\">Ack</option></select></label>");
	puts("<label class=\"event-limit\"><span>Rows</span><select id=\"event-limit\" title=\"Maximum matching transitions rendered in this browser\"><option value=\"25\">25</option><option value=\"50\">50</option><option value=\"100\" selected>100</option><option value=\"250\">250</option><option value=\"500\">500</option></select></label>");
	puts("<button id=\"pause\" class=\"command\" type=\"button\" title=\"Pause live updates and reconnect attempts\">Pause</button></div>");
	puts("<div id=\"regex-error\" class=\"filter-error\" role=\"alert\" hidden></div></div></section>");
	puts("<div class=\"workspace changes-only\"><aside><div class=\"section-heading\"><h2 title=\"Timestamps stay bold for five minutes after each event\">Recent changes</h2><div class=\"heading-actions\"><span id=\"event-count\" title=\"Matching events displayed after filters and the Rows limit\">0 shown</span><button id=\"message-display\" type=\"button\" aria-pressed=\"true\" title=\"Hide status message text from recent changes\">Messages</button><button id=\"color-display\" type=\"button\" aria-pressed=\"false\" title=\"Show transition colors as static GIFs\">GIFs</button><button id=\"clear\" type=\"button\" title=\"Clear recent changes from this browser view\">Clear</button><button class=\"fold\" type=\"button\" data-target=\"events-content\" aria-expanded=\"true\" aria-label=\"Fold recent changes list\" title=\"Fold recent changes list\"><span></span></button></div></div>");
	puts("<div id=\"events-content\"><div id=\"empty\" class=\"empty\">Waiting for status changes</div><ol id=\"events\"></ol></div></aside></div></main>");
	printf("<script src=\"%s/xymonlive.js\"></script>\n", skin);
	puts("</body></html>");
}

int main(int argc, char **argv)
{
	char *envarea = NULL;
	int argi;

	for (argi = 1; argi < argc; argi++) {
		if (argnmatch(argv[argi], "--area=")) envarea = strchr(argv[argi], '=') + 1;
	}
	for (argi = 1; argi < argc; argi++) {
		if (argnmatch(argv[argi], "--env=")) loadenv(strchr(argv[argi], '=') + 1, envarea);
		else if (argnmatch(argv[argi], "--area=")) ;
		else if (strcmp(argv[argi], "--debug") == 0) debug = 1;
		else {
			fprintf(stderr, "Usage: %s [--env=FILENAME] [--area=NAME] [--debug]\n", argv[0]);
			return 1;
		}
	}
	html_page();
	return 0;
}