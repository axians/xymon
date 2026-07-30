/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filter.h"

#define CHECK(condition, description) do { \
	if (!(condition)) { \
		fprintf(stderr, "FAIL: %s\n", description); \
		return 1; \
	} \
} while (0)

static char *make_extcombo(const char *first, const char *second)
{
	const size_t payload_offset = 64;
	size_t first_end = payload_offset + strlen(first);
	size_t second_end = first_end + strlen(second);
	char *message = (char *)malloc(second_end + 1);

	if (!message) return NULL;
	memset(message, ' ', payload_offset);
	snprintf(message, payload_offset, "extcombo %lu %lu %lu\n",
		 (unsigned long)payload_offset, (unsigned long)first_end,
		 (unsigned long)second_end);
	memset(message + strlen(message), ' ', payload_offset - strlen(message));
	memcpy(message + payload_offset, first, strlen(first));
	memcpy(message + first_end, second, strlen(second));
	message[second_end] = '\0';
	return message;
}

int main(void)
{
	proxy_filter_t *filter;
	char *extcombo;
	char *nested_extcombo;
	char *separator;
	char error[256];

	filter = proxy_filter_create();
	CHECK(filter != NULL, "filter allocation");
	CHECK(proxy_filter_allows(filter, "drop host.example.com"),
	      "an unconfigured filter must allow every message");
	CHECK(proxy_filter_add_commands(filter, "client,status,data",
					error, sizeof(error)) == 0,
	      "valid command allowlist");
	CHECK(proxy_filter_allows(filter, "client host.example.com.linux linux"),
	      "client command");
	CHECK(proxy_filter_allows(filter,
				  "client/local host.example.com.linux linux"),
	      "client collector suffix");
	CHECK(proxy_filter_allows(filter,
				  "status+10m/group:ops host.example.com.cpu green OK"),
	      "status lifetime and group suffixes");
	CHECK(proxy_filter_allows(filter,
				  "status/group:abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz host.example.com.cpu green OK"),
	      "long status group suffix");
	CHECK(proxy_filter_allows(filter,
				  "client/abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz host.example.com.linux linux"),
	      "long client collector suffix");
	CHECK(proxy_filter_allows(filter, "DATA host.example.com.vmstat\n1 2 3"),
	      "command matching is case-insensitive");
	CHECK(!proxy_filter_allows(filter, "drop host.example.com"),
	      "command outside allowlist");
	CHECK(!proxy_filter_allows(filter, "clientlog host.example.com"),
	      "client-prefixed query is not client data");
	CHECK(proxy_filter_allows(filter,
				  "combo\nstatus host.cpu green OK\n\n"
				  "status+10m host.disk yellow full"),
	      "legacy combo whose embedded statuses are allowed");
	CHECK(!proxy_filter_allows(filter,
				   "combo\ndrop host.example.com"),
	      "legacy combo with a non-status payload");
	CHECK(!proxy_filter_allows(filter, "combo\n"),
	      "empty legacy combo");

	extcombo = make_extcombo("status host.cpu green OK",
				 "client host.example.com.linux linux");
	CHECK(extcombo != NULL, "extcombo allocation");
	CHECK(proxy_filter_allows(filter, extcombo),
	      "extcombo whose embedded commands are allowed");
	separator = strchr(extcombo + 9, ' ');
	CHECK(separator != NULL, "extcombo offset separator");
	*separator = '\t';
	CHECK(!proxy_filter_allows(filter, extcombo),
	      "extcombo offsets require xymond-compatible separators");
	*separator = ' ';
	nested_extcombo = make_extcombo(extcombo,
				       "status host.memory green OK");
	CHECK(nested_extcombo != NULL, "nested extcombo allocation");
	CHECK(proxy_filter_allows(filter, nested_extcombo),
	      "nested extcombo whose embedded commands are allowed");
	free(nested_extcombo);
	free(extcombo);
	extcombo = make_extcombo("status host.cpu green OK",
				 "drop host.example.com");
	CHECK(extcombo != NULL, "mixed extcombo allocation");
	CHECK(!proxy_filter_allows(filter, extcombo),
	      "extcombo with a forbidden embedded command");
	free(extcombo);
	CHECK(!proxy_filter_allows(filter,
				   "extcombo 64 999\nstatus host.cpu green OK"),
	      "extcombo offset outside the message");
	CHECK(!proxy_filter_allows(filter, "extcombo 20\nstatus host.cpu green OK"),
	      "extcombo without an end offset");
	CHECK(!proxy_filter_allows(filter,
				   "extcombo +20 44\n     status host.cpu green OK"),
	      "extcombo offsets must be unsigned decimal tokens");
	CHECK(proxy_filter_add_commands(filter, "usermsg", error,
					sizeof(error)) == 0,
	      "repeated command options append rules");
	CHECK(proxy_filter_allows(filter, "usermsg example payload"),
	      "command appended by a repeated option");
	CHECK(proxy_filter_add_hostname(filter, "host.example.com", error,
					 sizeof(error)) == 0,
	      "valid hostname allowlist entry");
	CHECK(proxy_filter_allows(filter, "status host.example.com.cpu green OK"),
	      "status for an allowed hostname");
	CHECK(proxy_filter_allows(filter, "DATA HOST.EXAMPLE.COM.vmstat\n1 2 3"),
	      "hostname matching is case-insensitive");
	CHECK(proxy_filter_allows(filter,
				  "client/local host.example.com.linux linux"),
	      "client for an allowed hostname");
	CHECK(!proxy_filter_allows(filter, "status other.example.com.cpu green OK"),
	      "status for a hostname outside the scoped hosts file");
	CHECK(!proxy_filter_allows(filter, "usermsg host.example.com payload"),
	      "commands without a defined hostname format fail closed");
	extcombo = make_extcombo("status host.example.com.cpu green OK",
				 "client other.example.com.linux linux");
	CHECK(extcombo != NULL, "mixed-host extcombo allocation");
	CHECK(!proxy_filter_allows(filter, extcombo),
	      "extcombo with a hostname outside the scoped hosts file");
	free(extcombo);
	proxy_filter_destroy(filter);

	filter = proxy_filter_create();
	CHECK(proxy_filter_add_commands(filter, "combo", error,
					sizeof(error)) != 0,
	      "combo cannot be configured in a command allowlist");
	CHECK(proxy_filter_add_commands(filter, "extcombo", error,
					sizeof(error)) != 0,
	      "extcombo cannot be configured in a command allowlist");
	CHECK(proxy_filter_add_commands(filter, "schedule", error,
					sizeof(error)) != 0,
	      "schedule cannot bypass filtering with a nested command");
	proxy_filter_destroy(filter);

	puts("PASS: xymonproxy command filtering is opt-in and exact");
	return 0;
}