/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filter.h"

typedef struct command_rule_t {
	char *command;
	struct command_rule_t *next;
} command_rule_t;

typedef struct hostname_rule_t {
	char *hostname;
	struct hostname_rule_t *next;
} hostname_rule_t;

struct proxy_filter_t {
	command_rule_t *commands;
	hostname_rule_t *hostnames;
};

#define MAX_ENVELOPE_DEPTH 8

static void set_error(char *error, size_t error_size, const char *format,
		      const char *value)
{
	if (error && (error_size > 0)) {
		snprintf(error, error_size, format, value);
	}
}

static int valid_command(const char *command)
{
	const unsigned char *walk;

	if (!command || !isalpha((unsigned char)*command)) return 0;
	for (walk = (const unsigned char *)command; *walk; walk++) {
		if (!isalnum(*walk) && (*walk != '-') && (*walk != '_')) return 0;
	}
	return 1;
}

static void lowercase(char *text)
{
	unsigned char *walk;

	for (walk = (unsigned char *)text; *walk; walk++) {
		*walk = (unsigned char)tolower(*walk);
	}
}

proxy_filter_t *proxy_filter_create(void)
{
	return (proxy_filter_t *)calloc(1, sizeof(proxy_filter_t));
}

void proxy_filter_destroy(proxy_filter_t *filter)
{
	command_rule_t *command;
	hostname_rule_t *hostname;

	if (!filter) return;
	while (filter->commands) {
		command = filter->commands;
		filter->commands = command->next;
		free(command->command);
		free(command);
	}
	while (filter->hostnames) {
		hostname = filter->hostnames;
		filter->hostnames = hostname->next;
		free(hostname->hostname);
		free(hostname);
	}
	free(filter);
}

int proxy_filter_add_commands(proxy_filter_t *filter, const char *spec,
			      char *error, size_t error_size)
{
	char *copy;
	char *item;
	char *comma;

	if (!filter || !spec || !*spec) {
		set_error(error, error_size, "Invalid command list: %s",
			  (spec ? spec : ""));
		return -1;
	}

	copy = strdup(spec);
	if (!copy) {
		set_error(error, error_size, "Cannot allocate command list: %s", spec);
		return -1;
	}

	item = copy;
	while (item) {
		command_rule_t *rule;

		comma = strchr(item, ',');
		if (comma) *comma = '\0';
		lowercase(item);
		if (!valid_command(item)) {
			set_error(error, error_size, "Invalid command: %s", item);
			free(copy);
			return -1;
		}
		if ((strcmp(item, "combo") == 0) ||
		    (strcmp(item, "extcombo") == 0) ||
		    (strcmp(item, "schedule") == 0)) {
			set_error(error, error_size,
				  "Unsafe nested command cannot be allowed: %s", item);
			free(copy);
			return -1;
		}

		rule = (command_rule_t *)calloc(1, sizeof(command_rule_t));
		if (!rule) {
			set_error(error, error_size,
				  "Cannot allocate command rule: %s", item);
			free(copy);
			return -1;
		}
		rule->command = strdup(item);
		if (!rule->command) {
			free(rule);
			set_error(error, error_size,
				  "Cannot allocate command rule: %s", item);
			free(copy);
			return -1;
		}
		rule->next = filter->commands;
		filter->commands = rule;

		item = (comma ? comma + 1 : NULL);
	}

	free(copy);
	return 0;
}

int proxy_filter_add_hostname(proxy_filter_t *filter, const char *hostname,
			      char *error, size_t error_size)
{
	hostname_rule_t *rule;
	char *copy;

	if (!filter || !hostname || !*hostname ||
	    (strlen(hostname) != strspn(hostname,
					 "abcdefghijklmnopqrstuvwxyz"
					 "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
					 "0123456789._-"))) {
		set_error(error, error_size, "Invalid hostname: %s",
			  (hostname ? hostname : ""));
		return -1;
	}

	copy = strdup(hostname);
	if (!copy) {
		set_error(error, error_size, "Cannot allocate hostname: %s", hostname);
		return -1;
	}
	lowercase(copy);

	rule = (hostname_rule_t *)calloc(1, sizeof(hostname_rule_t));
	if (!rule) {
		free(copy);
		set_error(error, error_size, "Cannot allocate hostname rule: %s",
			  hostname);
		return -1;
	}
	rule->hostname = copy;
	rule->next = filter->hostnames;
	filter->hostnames = rule;
	return 0;
}

static int message_command(const char *message, size_t message_length,
			   char *command, size_t command_size)
{
	size_t length;
	size_t offset;

	if (!message || (message_length == 0) || !command ||
	    (command_size < 2)) return -1;
	for (length = 0; (length < message_length); length++) {
		if (isspace((unsigned char)message[length])) break;
	}
	if (length == 0) return -1;

	if ((length > 6) && ((message[6] == '+') || (message[6] == '/'))) {
		for (offset = 0; offset < 6; offset++) {
			if (tolower((unsigned char)message[offset]) != "status"[offset]) break;
		}
		if (offset == 6) {
			memcpy(command, "status", 7);
			return 0;
		}
	}
	if ((length > 6) && (message[6] == '/')) {
		for (offset = 0; offset < 6; offset++) {
			if (tolower((unsigned char)message[offset]) != "client"[offset]) break;
		}
		if (offset == 6) {
			memcpy(command, "client", 7);
			return 0;
		}
	}

	if (length >= command_size) return -1;
	memcpy(command, message, length);
	command[length] = '\0';
	lowercase(command);

	return 0;
}

static int command_is_allowed(const proxy_filter_t *filter, const char *command)
{
	const command_rule_t *rule;

	if (!filter->commands) return 1;
	for (rule = filter->commands; rule; rule = rule->next) {
		if (strcmp(command, rule->command) == 0) return 1;
	}
	return 0;
}

static int hostname_is_allowed(const proxy_filter_t *filter, const char *message,
			       size_t message_length, const char *command)
{
	const hostname_rule_t *rule;
	const char *hostname;
	const char *hostname_end;
	const char *token_end;
	size_t hostname_length;
	size_t offset;

	if (!filter->hostnames) return 1;
	if ((strcmp(command, "status") != 0) &&
	    (strcmp(command, "data") != 0) &&
	    (strcmp(command, "client") != 0)) return 0;

	hostname = message;
	while ((hostname < (message + message_length)) &&
	       !isspace((unsigned char)*hostname)) hostname++;
	while ((hostname < (message + message_length)) &&
	       isspace((unsigned char)*hostname)) hostname++;
	if (hostname == (message + message_length)) return 0;

	token_end = hostname;
	while ((token_end < (message + message_length)) &&
	       !isspace((unsigned char)*token_end)) token_end++;
	hostname_end = token_end;
	while ((hostname_end > hostname) && (*(hostname_end - 1) != '.')) {
		hostname_end--;
	}
	if ((hostname_end == hostname) || (hostname_end == token_end)) return 0;
	hostname_end--;
	hostname_length = (size_t)(hostname_end - hostname);

	for (rule = filter->hostnames; rule; rule = rule->next) {
		if (strlen(rule->hostname) != hostname_length) continue;
		for (offset = 0; offset < hostname_length; offset++) {
			unsigned char value = (unsigned char)hostname[offset];
			if (value == ',') value = '.';
			if (tolower(value) != (unsigned char)rule->hostname[offset]) break;
		}
		if (offset == hostname_length) return 1;
	}
	return 0;
}

static const char *find_bytes(const char *message, size_t message_length,
			      const char *needle, size_t needle_length)
{
	size_t offset;

	if (needle_length > message_length) return NULL;
	for (offset = 0; offset <= (message_length - needle_length); offset++) {
		if (memcmp(message + offset, needle, needle_length) == 0) {
			return message + offset;
		}
	}
	return NULL;
}

static int message_allowed(const proxy_filter_t *filter, const char *message,
			   size_t message_length, unsigned int depth);

static int combo_allowed(const proxy_filter_t *filter, const char *message,
			 size_t message_length, unsigned int depth)
{
	const char *current;
	const char *next;
	const char *message_end = message + message_length;
	char command[64];
	size_t current_length;

	if ((message_length <= 6) || (memcmp(message, "combo\n", 6) != 0)) {
		return 0;
	}

	current = message + 6;
	while (current < message_end) {
		next = find_bytes(current, (size_t)(message_end - current),
				  "\n\nstatus", 8);
		current_length = (next ? (size_t)(next - current + 1) :
				  (size_t)(message_end - current));
		if ((message_command(current, current_length, command,
				     sizeof(command)) != 0) ||
		    (strcmp(command, "status") != 0) ||
		    !message_allowed(filter, current, current_length, depth + 1)) {
			return 0;
		}
		if (!next) return 1;
		current = next + 2;
	}

	return 0;
}

static int extcombo_allowed(const proxy_filter_t *filter, const char *message,
			    size_t message_length, unsigned int depth)
{
	const char *line_end;
	const char *position;
	char *number_end;
	unsigned long parsed_offset;
	size_t start_offset;
	size_t end_offset;
	int message_count = 0;

	if ((message_length <= 9) || (memcmp(message, "extcombo ", 9) != 0)) {
		return 0;
	}
	line_end = (const char *)memchr(message, '\n', message_length);
	if (!line_end) return 0;

	position = message + 9;
	while ((position < line_end) && (*position == ' ')) position++;
	if ((position == line_end) || !isdigit((unsigned char)*position)) return 0;
	errno = 0;
	parsed_offset = strtoul(position, &number_end, 10);
	if ((number_end == position) || (number_end > line_end) || errno ||
	    (parsed_offset > (unsigned long)message_length) ||
	    (parsed_offset <= (unsigned long)(line_end - message))) return 0;
	start_offset = (size_t)parsed_offset;
	position = number_end;

	while (position < line_end) {
		while ((position < line_end) && (*position == ' ')) position++;
		if (position == line_end) break;
		if (!isdigit((unsigned char)*position)) return 0;
		errno = 0;
		parsed_offset = strtoul(position, &number_end, 10);
		if ((number_end == position) || (number_end > line_end) || errno ||
		    (parsed_offset > (unsigned long)message_length)) return 0;
		end_offset = (size_t)parsed_offset;
		if ((end_offset <= start_offset) ||
		    !message_allowed(filter, message + start_offset,
				     end_offset - start_offset, depth + 1)) return 0;
		start_offset = end_offset;
		position = number_end;
		message_count++;
	}

	return ((message_count > 0) && (start_offset == message_length));
}

static int message_allowed(const proxy_filter_t *filter, const char *message,
			   size_t message_length, unsigned int depth)
{
	char command[64];

	if (depth > MAX_ENVELOPE_DEPTH) return 0;
	if (message_command(message, message_length, command, sizeof(command)) != 0) {
		return 0;
	}
	if (strcmp(command, "combo") == 0) {
		return combo_allowed(filter, message, message_length, depth);
	}
	if (strcmp(command, "extcombo") == 0) {
		return extcombo_allowed(filter, message, message_length, depth);
	}
	return (command_is_allowed(filter, command) &&
		hostname_is_allowed(filter, message, message_length, command));
}

int proxy_filter_allows(const proxy_filter_t *filter, const char *message)
{
	if (!filter || (!filter->commands && !filter->hostnames)) return 1;
	if (!message) return 0;
	return message_allowed(filter, message, strlen(message), 0);
}