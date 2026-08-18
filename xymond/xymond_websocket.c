/*----------------------------------------------------------------------------*/
/* Xymon status-change WebSocket gateway.                                     */
/*----------------------------------------------------------------------------*/

#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

#include "libxymon.h"
#include "xymond_worker.h"

#define DEFAULT_PORT 1985
#define MAX_CLIENTS 128
#define REQUEST_SIZE 8192
#define EVENT_SIZE 2048
#define MESSAGE_SIZE 512
#define EVENT_RING_SIZE 500
#define OUTPUT_SIZE 262144
#define HANDSHAKE_TIMEOUT 10
#define XYMOND_STALE_TIMEOUT 90

typedef struct {
	char *text;
	unsigned int references;
} websocket_event_t;

typedef struct {
	int fd;
	int upgraded;
	int awaiting_pong;
	int close_after_write;
	int replay_count;
	int replay_next;
	size_t used;
	size_t output_used;
	size_t output_sent;
	time_t last_response;
	websocket_event_t *replay[EVENT_RING_SIZE];
	char request[REQUEST_SIZE];
	char output[OUTPUT_SIZE];
} websocket_client_t;

static volatile sig_atomic_t running = 1;

static websocket_event_t *new_event(const char *text)
{
	websocket_event_t *event = malloc(sizeof(*event));

	if (!event) return NULL;
	event->text = strdup(text);
	if (!event->text) { free(event); return NULL; }
	event->references = 1;
	return event;
}

static void retain_event(websocket_event_t *event)
{
	event->references++;
}

static void release_event(websocket_event_t *event)
{
	if (event && (--event->references == 0)) {
		free(event->text);
		free(event);
	}
}

static void stop_running(int signum)
{
	running = 0;
}

static int write_fd_all(int fd, const void *data, size_t length)
{
	const char *walk = data;

	while (length > 0) {
		ssize_t written = write(fd, walk, length);
		if ((written < 0) && (errno == EINTR)) continue;
		if (written <= 0) return -1;
		walk += written;
		length -= written;
	}
	return 0;
}

static int set_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL, 0);
	return ((flags == -1) || (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)) ? -1 : 0;
}

static int queue_bytes(websocket_client_t *client, const void *data, size_t length)
{
	if (length > (sizeof(client->output) - client->output_used)) return -1;
	memcpy(client->output + client->output_used, data, length);
	client->output_used += length;
	return 0;
}

static int flush_output(websocket_client_t *client)
{
	while (client->output_sent < client->output_used) {
		ssize_t written = send(client->fd, client->output + client->output_sent,
			client->output_used - client->output_sent, 0);
		if ((written < 0) && (errno == EINTR)) continue;
		if ((written < 0) && ((errno == EAGAIN) || (errno == EWOULDBLOCK))) return 0;
		if (written <= 0) return -1;
		client->output_sent += written;
	}
	client->output_used = client->output_sent = 0;
	return 0;
}

static char *header_value(const char *request, const char *name, char *value, size_t valuesz)
{
	const char *line = request;
	size_t namelen = strlen(name);

	while (line && *line) {
		const char *end = strstr(line, "\r\n");
		const char *start;
		size_t length;

		if (!end || (end == line)) break;
		if ((strncasecmp(line, name, namelen) == 0) && (line[namelen] == ':')) {
			start = line + namelen + 1;
			while ((*start == ' ') || (*start == '\t')) start++;
			length = end - start;
			if (length >= valuesz) length = valuesz - 1;
			memcpy(value, start, length);
			value[length] = '\0';
			return value;
		}
		line = end + 2;
	}

	return NULL;
}

static int header_has_token(const char *value, const char *wanted)
{
	const char *start = value;
	size_t wanted_length = strlen(wanted);

	while (*start) {
		const char *end;
		while ((*start == ' ') || (*start == '\t') || (*start == ',')) start++;
		end = strchr(start, ',');
		if (!end) end = start + strlen(start);
		while ((end > start) && ((end[-1] == ' ') || (end[-1] == '\t'))) end--;
		if (((size_t)(end - start) == wanted_length) && (strncasecmp(start, wanted, wanted_length) == 0)) return 1;
		start = (*end == ',') ? end + 1 : end;
	}
	return 0;
}

static int same_origin(const char *host, const char *origin, const char *expected)
{
	const char *authority;
	const char *end;
	size_t length;
	size_t expected_length;

	if (strncasecmp(expected, "http://", 7) == 0) authority = expected + 7;
	else if (strncasecmp(expected, "https://", 8) == 0) authority = expected + 8;
	else return 0;

	end = strchr(authority, '/');
	length = end ? (size_t)(end - authority) : strlen(authority);
	if (!length || (end && strcmp(end, "/") != 0)) return 0;
	expected_length = strlen(expected);
	if (expected_length && (expected[expected_length - 1] == '/')) expected_length--;
	return ((strlen(host) == length) &&
		(strncasecmp(host, authority, length) == 0) &&
		(strlen(origin) == expected_length) &&
		(strncasecmp(origin, expected, expected_length) == 0));
}

static void base64_binary(const unsigned char *input, size_t length, char output[29])
{
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t inpos = 0;
	size_t outpos = 0;

	while (inpos < length) {
		unsigned int value = input[inpos++] << 16;
		int bytes = 1;

		if (inpos < length) { value |= input[inpos++] << 8; bytes++; }
		if (inpos < length) { value |= input[inpos++]; bytes++; }
		output[outpos++] = alphabet[(value >> 18) & 0x3f];
		output[outpos++] = alphabet[(value >> 12) & 0x3f];
		output[outpos++] = (bytes > 1) ? alphabet[(value >> 6) & 0x3f] : '=';
		output[outpos++] = (bytes > 2) ? alphabet[value & 0x3f] : '=';
	}
	output[outpos] = '\0';
}

static int websocket_accept(const char *key, char output[29])
{
	static const char websocket_guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	unsigned char digest[20];
	char input[REQUEST_SIZE];
	void *context;

	if (snprintf(input, sizeof(input), "%s%s", key, websocket_guid) >= (int)sizeof(input)) return -1;
	context = malloc(mySHA1_Size());
	if (!context) return -1;
	mySHA1_Init(context);
	mySHA1_Update(context, (unsigned char *)input, strlen(input));
	mySHA1_Final(digest, context);
	free(context);
	base64_binary(digest, sizeof(digest), output);
	return 0;
}

static int queue_frame(websocket_client_t *client, unsigned char opcode, const char *payload, size_t length)
{
	unsigned char header[10];
	size_t headerlen;

	header[0] = 0x80 | opcode;
	if (length <= 125) {
		header[1] = length;
		headerlen = 2;
	}
	else if (length <= 65535) {
		header[1] = 126;
		header[2] = (length >> 8) & 0xff;
		header[3] = length & 0xff;
		headerlen = 4;
	}
	else return -1;

	if (queue_bytes(client, header, headerlen) == -1) return -1;
	if (length && (queue_bytes(client, payload, length) == -1)) return -1;
	return 0;
}

static int queue_text(websocket_client_t *client, const char *payload)
{
	return queue_frame(client, 0x1, payload, strlen(payload));
}

static int queue_replay(websocket_client_t *client)
{
	while (client->replay_next < client->replay_count) {
		websocket_event_t *event = client->replay[client->replay_next];
		size_t length = strlen(event->text);
		size_t headerlen = (length <= 125) ? 2 : 4;

		if ((headerlen + length) > (sizeof(client->output) - client->output_used)) break;
		if (queue_text(client, event->text) == -1) return -1;
		release_event(event);
		client->replay[client->replay_next] = NULL;
		client->replay_next++;
	}
	if (client->replay_next == client->replay_count) {
		client->replay_count = client->replay_next = 0;
	}
	return 0;
}

static int append_replay(websocket_client_t *client, websocket_event_t *event)
{
	int remaining;
	int index;

	remaining = client->replay_count - client->replay_next;
	if (client->replay_next) {
		for (index = 0; index < remaining; index++) {
			client->replay[index] = client->replay[client->replay_next + index];
			client->replay[client->replay_next + index] = NULL;
		}
		client->replay_count = remaining;
		client->replay_next = 0;
	}
	if (client->replay_count == EVENT_RING_SIZE) return -1;
	retain_event(event);
	client->replay[client->replay_count] = event;
	client->replay_count++;
	return 0;
}

static void discard_replay(websocket_client_t *client)
{
	int index;

	for (index = client->replay_next; index < client->replay_count; index++) {
		release_event(client->replay[index]);
		client->replay[index] = NULL;
	}
	client->replay_count = client->replay_next = 0;
}

static int valid_utf8(const unsigned char *text, size_t length)
{
	size_t index = 0;

	while (index < length) {
		unsigned char first = text[index++];
		int extra;
		if (first < 0x80) continue;
		if ((first >= 0xC2) && (first <= 0xDF)) extra = 1;
		else if ((first >= 0xE0) && (first <= 0xEF)) extra = 2;
		else if ((first >= 0xF0) && (first <= 0xF4)) extra = 3;
		else return 0;
		if ((index + extra) > length) return 0;
		if ((first == 0xE0) && ((text[index] < 0xA0) || (text[index] > 0xBF))) return 0;
		if ((first == 0xED) && ((text[index] < 0x80) || (text[index] > 0x9F))) return 0;
		if ((first == 0xF0) && ((text[index] < 0x90) || (text[index] > 0xBF))) return 0;
		if ((first == 0xF4) && ((text[index] < 0x80) || (text[index] > 0x8F))) return 0;
		while (extra--) {
			if ((text[index] & 0xC0) != 0x80) return 0;
			index++;
		}
	}
	return 1;
}

static int valid_close_payload(const unsigned char *payload, size_t length)
{
	unsigned int code;
	if (length == 0) return 1;
	if (length == 1) return 0;
	code = ((unsigned int)payload[0] << 8) | payload[1];
	if (!(((code >= 1000) && (code <= 1003)) ||
	      ((code >= 1007) && (code <= 1014)) ||
	      ((code >= 3000) && (code <= 4999)))) return 0;
	return valid_utf8(payload + 2, length - 2);
}

static int upgrade_client(websocket_client_t *client, websocket_event_t **events, int eventcount,
			  const char *generation, const char *expected_origin)
{
	char key[256];
	char host[512];
	char origin[1024];
	char upgrade[64];
	char connection[256];
	char version[32];
	char accept[29];
	char response[1024];
	char hello[256];
	int index;

	if (strncmp(client->request, "GET /xymonlive HTTP/1.1\r\n", 25) != 0) return -1;
	if (!header_value(client->request, "Host", host, sizeof(host))) return -1;
	if (!header_value(client->request, "Origin", origin, sizeof(origin))) return -1;
	if (!same_origin(host, origin, expected_origin)) return -1;
	if (!header_value(client->request, "Upgrade", upgrade, sizeof(upgrade)) || (strcasecmp(upgrade, "websocket") != 0)) return -1;
	if (!header_value(client->request, "Connection", connection, sizeof(connection)) || !header_has_token(connection, "upgrade")) return -1;
	if (!header_value(client->request, "Sec-WebSocket-Version", version, sizeof(version)) || (strcmp(version, "13") != 0)) return -1;
	if (!header_value(client->request, "Sec-WebSocket-Key", key, sizeof(key))) return -1;
	if ((strlen(key) != 24) || (strcmp(key + 22, "==") != 0) || (strspn(key, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/") != 22)) return -1;
	if (websocket_accept(key, accept) == -1) return -1;

	for (index = 0; index < eventcount; index++) {
		retain_event(events[index]);
		client->replay[index] = events[index];
		client->replay_count++;
	}

	snprintf(response, sizeof(response),
		"HTTP/1.1 101 Switching Protocols\r\n"
		"Upgrade: websocket\r\nConnection: Upgrade\r\n"
		"Sec-WebSocket-Accept: %s\r\n\r\n", accept);
	if (queue_bytes(client, response, strlen(response)) == -1) return -1;
	client->upgraded = 1;
	client->used = 0;
	client->awaiting_pong = 0;
	client->last_response = time(NULL);
	snprintf(hello, sizeof(hello), "{\"type\":\"hello\",\"generation\":\"%s\"}", generation);
	if (queue_text(client, hello) == -1) return -1;
	return queue_replay(client);
}

static void close_client(websocket_client_t *client)
{
	int index;

	if (client->fd >= 0) close(client->fd);
	for (index = 0; index < client->replay_count; index++) {
		release_event(client->replay[index]);
		client->replay[index] = NULL;
	}
	client->fd = -1;
	client->upgraded = 0;
	client->awaiting_pong = 0;
	client->close_after_write = 0;
	client->replay_count = client->replay_next = 0;
	client->used = 0;
	client->output_used = client->output_sent = 0;
}

static int open_listener(const char *address, int port)
{
	struct sockaddr_in socket_address;
	int listener;
	int reuse = 1;

	listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener == -1) return -1;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	memset(&socket_address, 0, sizeof(socket_address));
	socket_address.sin_family = AF_INET;
	socket_address.sin_port = htons(port);
	if (inet_pton(AF_INET, address, &socket_address.sin_addr) != 1) { close(listener); return -1; }
	if (bind(listener, (struct sockaddr *)&socket_address, sizeof(socket_address)) == -1) { close(listener); return -1; }
	if (listen(listener, 16) == -1) { close(listener); return -1; }
	if (set_nonblocking(listener) == -1) { close(listener); return -1; }
	return listener;
}

static int append_text(char *output, size_t outputsz, size_t *used, const char *text)
{
	size_t length = strlen(text);
	if (length >= (outputsz - *used)) return -1;
	memcpy(output + *used, text, length + 1);
	*used += length;
	return 0;
}

static int append_format(char *output, size_t outputsz, size_t *used, const char *format, long value)
{
	int length;
	if (*used >= outputsz) return -1;
	length = snprintf(output + *used, outputsz - *used, format, value);
	if ((length < 0) || ((size_t)length >= (outputsz - *used))) return -1;
	*used += length;
	return 0;
}

static int json_append(char *output, size_t outputsz, size_t *used, const char *value)
{
	const unsigned char *walk = (const unsigned char *)(value ? value : "");

	if (*used >= outputsz - 1) return -1;
	output[(*used)++] = '"';
	while (*walk) {
		if (*used >= outputsz - 7) return -1;
		switch (*walk) {
		  case '"': output[(*used)++] = '\\'; output[(*used)++] = '"'; break;
		  case '\\': output[(*used)++] = '\\'; output[(*used)++] = '\\'; break;
		  case '\n': output[(*used)++] = '\\'; output[(*used)++] = 'n'; break;
		  case '\r': output[(*used)++] = '\\'; output[(*used)++] = 'r'; break;
		  case '\t': output[(*used)++] = '\\'; output[(*used)++] = 't'; break;
		  default:
			if (*walk < 0x20) *used += snprintf(output + *used, outputsz - *used, "\\u%04x", *walk);
			else output[(*used)++] = *walk;
		}
		walk++;
	}
	if (*used >= outputsz - 1) return -1;
	output[(*used)++] = '"';
	output[*used] = '\0';
	return 0;
}

static void message_first_line(const char *body, char message[MESSAGE_SIZE])
{
	size_t length;

	if (body && (strncmp(body, "status ", 7) == 0)) {
		body = strchr(body, '\n');
		if (body) body++;
	}
	length = strcspn(body ? body : "", "\r\n");
	if (length >= MESSAGE_SIZE) length = MESSAGE_SIZE - 1;
	memcpy(message, body ? body : "", length);
	while (length && !valid_utf8((unsigned char *)message, length)) length--;
	message[length] = '\0';
}

static int format_event(char *msg, int sequence, char output[EVENT_SIZE])
{
	char *metadata[16];
	char *lineend;
	char message[MESSAGE_SIZE];
	const char *kind;
	char *token;
	int count = 0;
	size_t used;

	lineend = strchr(msg, '\n');
	message_first_line(lineend ? lineend + 1 : "", message);
	if (lineend) *lineend = '\0';
	token = gettok(msg, "|");
	while (token && (count < 16)) {
		metadata[count++] = token;
		token = gettok(NULL, "|");
	}
	if ((count < 10) ||
	    ((strncmp(metadata[0], "@@stachg", 8) != 0) &&
	     (strncmp(metadata[0], "@@disable", 9) != 0) &&
	     (strncmp(metadata[0], "@@ack", 5) != 0))) {
		if (debug) fprintf(stderr, "xymond_websocket: malformed stachg metadata: count=%d marker=%s\n",
			count, count ? metadata[0] : "(none)");
		return -1;
	}
	kind = (strncmp(metadata[0], "@@ack", 5) == 0) ? "ack" :
		((strncmp(metadata[0], "@@disable", 9) == 0) ? "disable" : "status");
	if ((strcmp(kind, "disable") == 0) && (count > 11) && *metadata[11]) {
		nldecode(metadata[11]);
		message_first_line(metadata[11], message);
	}

	used = 0;
	if ((append_text(output, EVENT_SIZE, &used, "{\"type\":\"change\",\"sequence\":") == -1) ||
	    (append_format(output, EVENT_SIZE, &used, "%ld", sequence) == -1) ||
	    (append_text(output, EVENT_SIZE, &used, ",\"time\":") == -1) ||
	    (append_format(output, EVENT_SIZE, &used, "%ld", atol(metadata[1])) == -1) ||
	    (append_text(output, EVENT_SIZE, &used, ",\"host\":") == -1) ||
	    (json_append(output, EVENT_SIZE, &used, metadata[4]) == -1) ||
	    (append_text(output, EVENT_SIZE, &used, ",\"test\":") == -1) ||
	    (json_append(output, EVENT_SIZE, &used, metadata[5]) == -1) ||
	    (append_text(output, EVENT_SIZE, &used, ",\"previousColor\":") == -1) ||
	    (json_append(output, EVENT_SIZE, &used, metadata[8]) == -1) ||
	    (append_text(output, EVENT_SIZE, &used, ",\"color\":") == -1) ||
	    (json_append(output, EVENT_SIZE, &used, metadata[7]) == -1) ||
	    (append_text(output, EVENT_SIZE, &used, ",\"kind\":") == -1) ||
	    (json_append(output, EVENT_SIZE, &used, kind) == -1) ||
	    (append_text(output, EVENT_SIZE, &used, ",\"message\":") == -1) ||
	    (json_append(output, EVENT_SIZE, &used, message) == -1) ||
	    (append_text(output, EVENT_SIZE, &used, "}") == -1)) return -1;
	return 0;
}

static int process_frames(websocket_client_t *client)
{
	size_t offset = 0;

	while ((client->used - offset) >= 2) {
		unsigned char *frame = (unsigned char *)client->request + offset;
		unsigned char opcode = frame[0] & 0x0f;
		size_t length = frame[1] & 0x7f;
		size_t header = 2;
		unsigned char *mask;
		char *payload;
		size_t index;

		if (!(frame[0] & 0x80) || (frame[0] & 0x70) || !(frame[1] & 0x80)) return -1;
		if ((opcode != 0x1) && (opcode != 0x2) && (opcode != 0x8) && (opcode != 0x9) && (opcode != 0xA)) return -1;
		if (length == 126) {
			if ((client->used - offset) < 4) break;
			length = ((size_t)frame[2] << 8) | frame[3];
			header = 4;
		}
		else if (length == 127) return -1;
		if ((opcode >= 0x8) && (length > 125)) return -1;
		if (length > (REQUEST_SIZE - header - 4)) return -1;
		if ((client->used - offset) < (header + 4 + length)) break;
		mask = frame + header;
		payload = (char *)(mask + 4);
		for (index = 0; index < length; index++) payload[index] ^= mask[index % 4];
		if ((opcode == 0x8) && !valid_close_payload((unsigned char *)payload, length)) return -1;

		if (opcode == 0x8) {
			if (queue_frame(client, 0x8, payload, length) == -1) return -1;
			client->close_after_write = 1;
		}
		else if (opcode == 0x9) {
			if (queue_frame(client, 0xA, payload, length) == -1) return -1;
		}
		else if (opcode == 0xA) {
			client->awaiting_pong = 0;
			client->last_response = time(NULL);
		}
		offset += header + 4 + length;
	}
	if (offset) {
		client->used -= offset;
		memmove(client->request, client->request + offset, client->used);
	}
	return 0;
}

static void channel_reader(int outputfd)
{
	char *msg;
	int sequence;
	char event[EVENT_SIZE];

	while ((msg = get_xymond_message(C_STACHG, "xymond_websocket", &sequence, NULL)) != NULL) {
		dbgprintf("xymond_websocket: received channel sequence %d\n", sequence);
		if (strncmp(msg, "@@heartbeat", 11) == 0) {
			static const char heartbeat[] = "{\"type\":\"xymond\",\"state\":\"alive\"}\n";
			if (write_fd_all(outputfd, heartbeat, sizeof(heartbeat) - 1) == -1) break;
		}
		else if (format_event(msg, sequence, event) == 0) {
			if ((write_fd_all(outputfd, event, strlen(event)) == -1) || (write_fd_all(outputfd, "\n", 1) == -1)) break;
		}
		else dbgprintf("xymond_websocket: ignored malformed channel message\n");
	}
	close(outputfd);
	_exit(0);
}

int main(int argc, char **argv)
{
	const char *address = "127.0.0.1";
	const char *expected_origin = NULL;
	int port = DEFAULT_PORT;
	int eventpipe[2];
	int listener;
	pid_t readerpid;
	static websocket_client_t clients[MAX_CLIENTS];
	websocket_event_t *events[EVENT_RING_SIZE];
	int eventstart = 0;
	int eventcount = 0;
	char eventbuffer[EVENT_SIZE * 2];
	size_t eventused = 0;
	char generation[64];
	int channel_down = 0;
	int xymond_stale = 0;
	time_t last_xymond_response = time(NULL);
	int index;

	for (index = 1; index < argc; index++) {
		if (strncmp(argv[index], "--listen=", 9) == 0) address = argv[index] + 9;
		else if (strncmp(argv[index], "--port=", 7) == 0) port = atoi(argv[index] + 7);
		else if (strncmp(argv[index], "--origin=", 9) == 0) expected_origin = argv[index] + 9;
		else if (strcmp(argv[index], "--debug") == 0) debug = 1;
		else { fprintf(stderr, "Usage: %s --origin=ORIGIN [--listen=ADDRESS] [--port=PORT] [--debug]\n", argv[0]); return 1; }
	}
	if ((port < 1) || (port > 65535)) { fprintf(stderr, "Invalid port: %d\n", port); return 1; }
	if (!expected_origin || !*expected_origin ||
	    ((strncasecmp(expected_origin, "http://", 7) != 0) &&
	     (strncasecmp(expected_origin, "https://", 8) != 0))) {
		fprintf(stderr, "A valid --origin=http[s]://HOST[:PORT] is required\n");
		return 1;
	}

	signal(SIGTERM, stop_running);
	signal(SIGINT, stop_running);
	signal(SIGPIPE, SIG_IGN);
	for (index = 0; index < MAX_CLIENTS; index++) clients[index].fd = -1;
	for (index = 0; index < EVENT_RING_SIZE; index++) events[index] = NULL;
	snprintf(generation, sizeof(generation), "%ld-%ld", (long)time(NULL), (long)getpid());

	listener = open_listener(address, port);
	if (listener == -1) { fprintf(stderr, "Cannot listen on %s:%d: %s\n", address, port, strerror(errno)); return 1; }
	if (pipe(eventpipe) == -1) { fprintf(stderr, "Cannot create event pipe: %s\n", strerror(errno)); close(listener); return 1; }
	readerpid = fork();
	if (readerpid == -1) { fprintf(stderr, "Cannot fork channel reader: %s\n", strerror(errno)); close(listener); return 1; }
	if (readerpid == 0) {
		signal(SIGTERM, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		close(listener);
		close(eventpipe[0]);
		channel_reader(eventpipe[1]);
	}
	close(eventpipe[1]);
	set_nonblocking(eventpipe[0]);

	while (running) {
		fd_set readfds;
		fd_set writefds;
		struct timeval timeout;
		int maxfd = (listener > eventpipe[0]) ? listener : eventpipe[0];
		int ready;
		time_t now;

		FD_ZERO(&readfds);
		FD_ZERO(&writefds);
		if (listener >= 0) FD_SET(listener, &readfds);
		if (eventpipe[0] >= 0) FD_SET(eventpipe[0], &readfds);
		for (index = 0; index < MAX_CLIENTS; index++) {
			if (clients[index].fd >= 0) {
				if (!clients[index].close_after_write) FD_SET(clients[index].fd, &readfds);
				if (clients[index].output_used > clients[index].output_sent) FD_SET(clients[index].fd, &writefds);
				if (clients[index].fd > maxfd) maxfd = clients[index].fd;
			}
		}
		timeout.tv_sec = 5;
		timeout.tv_usec = 0;
		ready = select(maxfd + 1, &readfds, &writefds, NULL, &timeout);
		if ((ready < 0) && (errno == EINTR)) continue;
		if (ready < 0) break;
		now = time(NULL);
		if (!channel_down && !xymond_stale && ((now - last_xymond_response) >= XYMOND_STALE_TIMEOUT)) {
			xymond_stale = 1;
			for (index = 0; index < MAX_CLIENTS; index++) {
				if (clients[index].upgraded &&
				    (queue_text(&clients[index], "{\"type\":\"xymond\",\"state\":\"unavailable\"}") == -1)) {
					close_client(&clients[index]);
				}
			}
		}
		for (index = 0; index < MAX_CLIENTS; index++) {
			websocket_client_t *client = &clients[index];
			if ((client->fd >= 0) && !client->upgraded && ((now - client->last_response) >= HANDSHAKE_TIMEOUT)) close_client(client);
			else if (!client->upgraded) continue;
			else if (client->awaiting_pong && ((now - client->last_response) >= 60)) close_client(client);
			else if (!client->awaiting_pong && ((now - client->last_response) >= 30)) {
				if ((queue_text(client, "{\"type\":\"heartbeat\"}") == -1) ||
				    (queue_frame(client, 0x9, "", 0) == -1)) close_client(client);
				else client->awaiting_pong = 1;
			}
		}
		if (ready == 0) continue;

		for (index = 0; index < MAX_CLIENTS; index++) {
			websocket_client_t *client = &clients[index];
			if ((client->fd < 0) || !FD_ISSET(client->fd, &writefds)) continue;
			if (flush_output(client) == -1) close_client(client);
			else if (queue_replay(client) == -1) close_client(client);
			else if (client->close_after_write && (client->output_used == 0)) close_client(client);
		}

		if ((listener >= 0) && FD_ISSET(listener, &readfds)) {
			int clientfd = accept(listener, NULL, NULL);
			if (clientfd >= 0) {
				for (index = 0; (index < MAX_CLIENTS) && (clients[index].fd >= 0); index++) ;
				if (index == MAX_CLIENTS) close(clientfd);
				else {
					set_nonblocking(clientfd);
					clients[index].fd = clientfd;
					clients[index].upgraded = clients[index].awaiting_pong = clients[index].close_after_write = 0;
					clients[index].replay_count = clients[index].replay_next = 0;
					clients[index].used = clients[index].output_used = clients[index].output_sent = 0;
					clients[index].last_response = now;
				}
			}
		}

		if ((eventpipe[0] >= 0) && FD_ISSET(eventpipe[0], &readfds)) {
			ssize_t count = read(eventpipe[0], eventbuffer + eventused, sizeof(eventbuffer) - eventused - 1);
			if (count <= 0) {
				close(eventpipe[0]);
				eventpipe[0] = -1;
				close(listener);
				listener = -1;
				channel_down = 1;
				for (index = 0; index < MAX_CLIENTS; index++) {
					if (clients[index].fd < 0) continue;
					discard_replay(&clients[index]);
					if (!clients[index].upgraded ||
					    (queue_text(&clients[index], "{\"type\":\"xymond\",\"state\":\"unavailable\"}") == -1)) {
						close_client(&clients[index]);
					}
					else clients[index].close_after_write = 1;
				}
			}
			else {
				char *line;
				char *newline;
				eventused += count;
				eventbuffer[eventused] = '\0';
				line = eventbuffer;
				while ((newline = strchr(line, '\n')) != NULL) {
					int ringpos;
					websocket_event_t *event;
					*newline = '\0';
					last_xymond_response = now;
					if (xymond_stale &&
					    (strcmp(line, "{\"type\":\"xymond\",\"state\":\"alive\"}") != 0)) {
						xymond_stale = 0;
						for (index = 0; index < MAX_CLIENTS; index++) {
							if (clients[index].upgraded &&
							    (queue_text(&clients[index], "{\"type\":\"xymond\",\"state\":\"alive\"}") == -1)) {
								close_client(&clients[index]);
							}
						}
					}
					if (strcmp(line, "{\"type\":\"xymond\",\"state\":\"alive\"}") == 0) {
						xymond_stale = 0;
						for (index = 0; index < MAX_CLIENTS; index++) {
							if (clients[index].upgraded &&
							    (queue_text(&clients[index], line) == -1)) close_client(&clients[index]);
						}
						line = newline + 1;
						continue;
					}
					event = new_event(line);
					if (!event) { running = 0; break; }
					for (index = 0; index < MAX_CLIENTS; index++) {
						if (!clients[index].upgraded) continue;
						if (clients[index].replay_count) {
							if (append_replay(&clients[index], event) == -1) close_client(&clients[index]);
						}
						else if (queue_text(&clients[index], event->text) == -1) close_client(&clients[index]);
					}
					ringpos = (eventstart + eventcount) % EVENT_RING_SIZE;
					if (eventcount == EVENT_RING_SIZE) { release_event(events[eventstart]); eventstart = (eventstart + 1) % EVENT_RING_SIZE; ringpos = (eventstart + eventcount - 1) % EVENT_RING_SIZE; }
					else eventcount++;
					events[ringpos] = event;
					line = newline + 1;
				}
				eventused = strlen(line);
				memmove(eventbuffer, line, eventused + 1);
			}
		}

		for (index = 0; index < MAX_CLIENTS; index++) {
			websocket_client_t *client = &clients[index];
			if ((client->fd < 0) || !FD_ISSET(client->fd, &readfds)) continue;
			if (!client->upgraded) {
				ssize_t count = read(client->fd, client->request + client->used, sizeof(client->request) - client->used - 1);
				if (count <= 0) { close_client(client); continue; }
				client->used += count;
				client->request[client->used] = '\0';
				if (strstr(client->request, "\r\n\r\n")) {
					websocket_event_t *ordered[EVENT_RING_SIZE];
					int item;
					for (item = 0; item < eventcount; item++) ordered[item] = events[(eventstart + item) % EVENT_RING_SIZE];
					if (upgrade_client(client, ordered, eventcount, generation, expected_origin) == -1) {
						static const char forbidden[] = "HTTP/1.1 403 Forbidden\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
						if (queue_bytes(client, forbidden, sizeof(forbidden) - 1) == -1) close_client(client);
						else client->close_after_write = 1;
					}
				}
				else if (client->used == sizeof(client->request) - 1) close_client(client);
			}
			else {
				ssize_t count = recv(client->fd, client->request + client->used, sizeof(client->request) - client->used, 0);
				if (count <= 0) close_client(client);
				else {
					client->used += count;
					if (process_frames(client) == -1) close_client(client);
				}
			}
		}
		if (channel_down) {
			for (index = 0; (index < MAX_CLIENTS) && (clients[index].fd < 0); index++) ;
			if (index == MAX_CLIENTS) running = 0;
		}
	}

	if (listener >= 0) close(listener);
	if (eventpipe[0] >= 0) close(eventpipe[0]);
	kill(readerpid, SIGTERM);
	waitpid(readerpid, NULL, 0);
	for (index = 0; index < MAX_CLIENTS; index++) close_client(&clients[index]);
	for (index = 0; index < EVENT_RING_SIZE; index++) release_event(events[index]);
	return 0;
}