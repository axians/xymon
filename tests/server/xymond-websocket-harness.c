#define _POSIX_C_SOURCE 200809L

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RESPONSE_SIZE 4096
#define FRAME_SIZE 8192
#define REPLAY_LIMIT 500

static int write_all(int fd, const void *data, size_t length)
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

static int read_all(int fd, void *data, size_t length)
{
	char *walk = data;

	while (length > 0) {
		ssize_t count = read(fd, walk, length);
		if ((count < 0) && (errno == EINTR)) continue;
		if (count <= 0) return -1;
		walk += count;
		length -= count;
	}
	return 0;
}

static void short_pause(void)
{
	struct timespec delay;
	delay.tv_sec = 0;
	delay.tv_nsec = 50000000;
	nanosleep(&delay, NULL);
}

static in_port_t network_port(unsigned int port)
{
	unsigned char bytes[2];
	in_port_t result;

	bytes[0] = (port >> 8) & 0xff;
	bytes[1] = port & 0xff;
	memcpy(&result, bytes, sizeof(bytes));
	return result;
}

static unsigned int host_port(in_port_t port)
{
	unsigned char bytes[2];

	memcpy(bytes, &port, sizeof(bytes));
	return ((unsigned int)bytes[0] << 8) | bytes[1];
}

static int free_port(void)
{
	struct sockaddr_in address;
	socklen_t address_length = sizeof(address);
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	int port;

	if (listener == -1) return -1;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) { close(listener); return -1; }
	address.sin_port = 0;
	if ((bind(listener, (struct sockaddr *)&address, sizeof(address)) == -1) ||
	    (getsockname(listener, (struct sockaddr *)&address, &address_length) == -1)) {
		close(listener);
		return -1;
	}
	port = host_port(address.sin_port);
	close(listener);
	return port;
}

static int connect_port(int port)
{
	struct sockaddr_in address;
	struct timeval timeout;
	int fd = socket(AF_INET, SOCK_STREAM, 0);

	if (fd == -1) return -1;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) { close(fd); return -1; }
	address.sin_port = network_port((unsigned int)port);
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == -1) {
		close(fd);
		return -1;
	}
	timeout.tv_sec = 3;
	timeout.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	return fd;
}

static int wait_listener(int port, pid_t child)
{
	int tries;

	for (tries = 0; tries < 100; tries++) {
		int status;
		int fd;
		if (waitpid(child, &status, WNOHANG) == child) return -1;
		fd = connect_port(port);
		if (fd >= 0) { close(fd); return 0; }
		short_pause();
	}
	return -1;
}

static int read_headers(int fd, char response[RESPONSE_SIZE])
{
	size_t used = 0;

	while (used < RESPONSE_SIZE - 1) {
		if (read_all(fd, response + used, 1) == -1) return -1;
		used++;
		response[used] = '\0';
		if ((used >= 4) && (strcmp(response + used - 4, "\r\n\r\n") == 0)) return 0;
	}
	return -1;
}

static int websocket(int port, const char *origin, int accepted)
{
	static const char key[] = "dGhlIHNhbXBsZSBub25jZQ==";
	char request[1024];
	char response[RESPONSE_SIZE];
	char host[64];
	int fd = connect_port(port);
	int length;

	if (fd == -1) return -1;
	snprintf(host, sizeof(host), "127.0.0.1:%d", port);
	length = snprintf(request, sizeof(request),
		"GET /xymonlive HTTP/1.1\r\n"
		"Host: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
		"Origin: %s\r\nSec-WebSocket-Key: %s\r\n"
		"Sec-WebSocket-Version: 13\r\n\r\n",
		host, origin ? origin : host, key);
	if ((length < 0) || ((size_t)length >= sizeof(request)) ||
	    (write_all(fd, request, (size_t)length) == -1) ||
	    (read_headers(fd, response) == -1)) {
		close(fd);
		return -1;
	}
	if (accepted) {
		if (!strstr(response, "HTTP/1.1 101 ") ||
		    !strstr(response, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")) {
			close(fd);
			return -1;
		}
	}
	else {
		if (!strstr(response, "HTTP/1.1 403 ")) { close(fd); return -1; }
		close(fd);
		return 0;
	}
	return fd;
}

static int rejected_handshake(int port, const char *request_line, const char *connection)
{
	char request[1024];
	char response[RESPONSE_SIZE];
	int fd = connect_port(port);
	int length;

	if (fd == -1) return -1;
	length = snprintf(request, sizeof(request),
		"%sHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\n%s"
		"Origin: http://127.0.0.1:%d\r\n"
		"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
		"Sec-WebSocket-Version: 13\r\n\r\n", request_line, port, connection, port);
	if ((length < 0) || ((size_t)length >= sizeof(request)) ||
	    (write_all(fd, request, (size_t)length) == -1) ||
	    (read_headers(fd, response) == -1) || !strstr(response, "HTTP/1.1 403 ")) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

static int read_opcode_frame(int fd, unsigned char *opcode, char payload[FRAME_SIZE])
{
	unsigned char header[4];
	size_t length;

	if (read_all(fd, header, 2) == -1) return -1;
	*opcode = header[0] & 0x0f;
	length = header[1] & 0x7f;
	if (length == 126) {
		if (read_all(fd, header + 2, 2) == -1) return -1;
		length = ((size_t)header[2] << 8) | header[3];
	}
	if (length >= FRAME_SIZE) return -1;
	if (read_all(fd, payload, length) == -1) return -1;
	payload[length] = '\0';
	return 0;
}

static int send_masked_data(int fd, unsigned char first, const unsigned char *payload, size_t length);

static int read_frame(int fd, char payload[FRAME_SIZE])
{
	unsigned char opcode;

	while (read_opcode_frame(fd, &opcode, payload) == 0) {
		if (opcode == 0x1) return 0;
		if ((opcode == 0x9) &&
		    (send_masked_data(fd, 0x8A, (unsigned char *)payload, strlen(payload)) == 0)) continue;
		if (opcode == 0xA) continue;
		return -1;
	}
	return -1;
}

static int send_masked_data(int fd, unsigned char first, const unsigned char *payload, size_t length)
{
	static const unsigned char mask[4] = { 0x12, 0x34, 0x56, 0x78 };
	unsigned char frame[2 + 4 + 125];
	size_t index;

	if (length > 125) return -1;
	frame[0] = first;
	frame[1] = 0x80 | length;
	memcpy(frame + 2, mask, sizeof(mask));
	for (index = 0; index < length; index++) frame[6 + index] = payload[index] ^ mask[index % 4];
	return write_all(fd, frame, 6 + length);
}

static int send_masked_first(int fd, unsigned char first, const char *payload)
{
	return send_masked_data(fd, first, (const unsigned char *)payload, strlen(payload));
}

static int send_masked_frame(int fd, unsigned char opcode, const char *payload)
{
	return send_masked_first(fd, 0x80 | opcode, payload);
}

static int expect_closed(int fd)
{
	char byte;
	ssize_t count;
	do { count = read(fd, &byte, 1); } while ((count < 0) && (errno == EINTR));
	return (count == 0) ? 0 : -1;
}

static int incomplete_handshake_expires(int port)
{
	static const char incomplete[] = "GET /xymonlive HTTP/1.1\r\nHost: localhost\r\n";
	int fd = connect_port(port);

	if ((fd == -1) || (write_all(fd, incomplete, sizeof(incomplete) - 1) == -1)) {
		if (fd >= 0) close(fd);
		return -1;
	}
	sleep(12);
	if (expect_closed(fd) == -1) { close(fd); return -1; }
	close(fd);
	return 0;
}

static int contains(const char *text, const char *expected)
{
	if (strstr(text, expected)) return 1;
	fprintf(stderr, "frame missing %s: %s\n", expected, text);
	return 0;
}

static void stop_child(pid_t child)
{
	int tries;
	int status;

	if (child <= 0) return;
	kill(child, SIGTERM);
	for (tries = 0; tries < 50; tries++) {
		if (waitpid(child, &status, WNOHANG) == child) return;
		short_pause();
	}
	kill(child, SIGKILL);
	waitpid(child, &status, 0);
}

int main(int argc, char **argv)
{
	char portarg[32];
	char origin[64];
	char originarg[80];
	char message[1024];
	char hello[FRAME_SIZE];
	char change[FRAME_SIZE];
	char replay[FRAME_SIZE];
	char longbody[512];
	int inputpipe[2];
	int port;
	int first = -1;
	int second = -1;
	int third = -1;
	int invalid = -1;
	pid_t child = -1;
	time_t now;
	int result = 1;
	const char *stage = "startup";

	if (argc != 2) { fprintf(stderr, "usage: %s GATEWAY\n", argv[0]); return 2; }
	port = free_port();
	if ((port < 1) || (pipe(inputpipe) == -1)) return 2;
	snprintf(portarg, sizeof(portarg), "--port=%d", port);
	snprintf(origin, sizeof(origin), "http://127.0.0.1:%d", port);
	snprintf(originarg, sizeof(originarg), "--origin=%s", origin);
	child = fork();
	if (child == -1) return 2;
	if (child == 0) {
		dup2(inputpipe[0], STDIN_FILENO);
		close(inputpipe[0]);
		close(inputpipe[1]);
		execl(argv[1], argv[1], "--listen=127.0.0.1", portarg, originarg, (char *)NULL);
		_exit(127);
	}
	close(inputpipe[0]);
	stage = "listener";
	if (wait_listener(port, child) == -1) goto done;
	first = websocket(port, origin, 1);
	stage = "first handshake";
	if ((first == -1) || (read_frame(first, hello) == -1) ||
	    !contains(hello, "\"type\":\"hello\"") ||
	    !contains(hello, "\"generation\":")) goto done;
	stage = "origin scheme rejection";
	snprintf(origin, sizeof(origin), "https://127.0.0.1:%d", port);
	if (websocket(port, origin, 0) == -1) goto done;
	snprintf(origin, sizeof(origin), "http://127.0.0.1:%d", port);
	{
		unsigned char opcode;
		stage = "ping pong";
		if ((send_masked_frame(first, 0x9, "alive") == -1) ||
		    (read_opcode_frame(first, &opcode, replay) == -1) ||
		    (opcode != 0xA) || (strcmp(replay, "alive") != 0)) goto done;
	}
	stage = "invalid close";
	invalid = websocket(port, origin, 1);
	if ((invalid == -1) || (read_frame(invalid, hello) == -1)) goto done;
	{
		static const unsigned char forbidden_close[] = { 0x03, 0xED };
		if ((send_masked_data(invalid, 0x88, forbidden_close, sizeof(forbidden_close)) == -1) ||
		    (expect_closed(invalid) == -1)) goto done;
	}
	close(invalid);
	invalid = -1;

	now = time(NULL);
	snprintf(message, sizeof(message),
		"@@stachg#7/testhost.example.com|%ld.000000|127.0.0.1|origin|"
		"testhost.example.com|cpu|%ld|red|green|%ld|0||0|0|\n"
		"status testhost.example.com.cpu red\nstatus body\n@@\n", (long)now, (long)(now + 300), (long)now);
	if ((write_all(inputpipe[1], message, strlen(message)) == -1) ||
	    (read_frame(first, change) == -1) ||
	    !contains(change, "\"type\":\"change\"") ||
	    !contains(change, "\"sequence\":7") ||
	    !contains(change, "\"host\":\"testhost.example.com\"") ||
	    !contains(change, "\"test\":\"cpu\"") ||
	    !contains(change, "\"previousColor\":\"green\"") ||
	    !contains(change, "\"color\":\"red\"") ||
	    !contains(change, "\"kind\":\"status\"") ||
	    !contains(change, "\"message\":\"status body\"")) goto done;
	stage = "ack event";
	snprintf(message, sizeof(message),
		"@@ack#8/testhost.example.com|%ld.000000|127.0.0.1|origin|"
		"testhost.example.com|cpu|%ld|red|red|%ld|0||0|0|\noperator ack\n@@\n",
		(long)now, (long)(now + 300), (long)now);
	if ((write_all(inputpipe[1], message, strlen(message)) == -1) ||
	    (read_frame(first, replay) == -1) ||
	    !contains(replay, "\"kind\":\"ack\"") ||
	    !contains(replay, "\"message\":\"operator ack\"")) goto done;
	stage = "disable event";
	snprintf(message, sizeof(message),
		"@@disable#9/testhost.example.com|%ld.000000|127.0.0.1|origin|"
		"testhost.example.com|cpu|%ld|blue|red|%ld|%ld|maintenance|0|0|\nmaintenance\n@@\n",
		(long)now, (long)(now + 300), (long)now, (long)(now + 600));
	if ((write_all(inputpipe[1], message, strlen(message)) == -1) ||
	    (read_frame(first, replay) == -1) ||
	    !contains(replay, "\"kind\":\"disable\"") ||
	    !contains(replay, "\"message\":\"maintenance\"")) goto done;

	stage = "single replay";
	second = websocket(port, origin, 1);
	if ((second == -1) || (read_frame(second, hello) == -1) ||
	    (read_frame(second, replay) == -1) || (strcmp(replay, change) != 0)) goto done;
	close(second);
	second = -1;
	{
		int sequence;
		stage = "full replay";
		memset(longbody, 'x', sizeof(longbody) - 1);
		longbody[sizeof(longbody) - 1] = '\0';
		for (sequence = 10; sequence < 520; sequence++) {
			snprintf(message, sizeof(message),
				"@@stachg#%d/testhost.example.com|%ld.000000|127.0.0.1|origin|"
				"testhost.example.com|cpu|%ld|%s|%s|%ld|0||0|0|\n"
				"status testhost.example.com.cpu red\n%s\n@@\n",
				sequence, (long)now, (long)(now + 300),
				(sequence & 1) ? "yellow" : "red", (sequence & 1) ? "red" : "yellow", (long)now,
				longbody);
			if (write_all(inputpipe[1], message, strlen(message)) == -1) goto done;
			if (read_frame(first, replay) == -1) goto done;
		}
		third = websocket(port, origin, 1);
		if (third == -1) goto done;
		snprintf(message, sizeof(message),
			"@@stachg#520/testhost.example.com|%ld.000000|127.0.0.1|origin|"
			"testhost.example.com|cpu|%ld|green|red|%ld|0||0|0|\n"
			"status testhost.example.com.cpu green\nlive after replay\n@@\n",
			(long)now, (long)(now + 300), (long)now);
		if ((write_all(inputpipe[1], message, strlen(message)) == -1) ||
		    (read_frame(first, change) == -1) ||
		    !contains(change, "\"sequence\":520")) goto done;
		if (read_frame(third, hello) == -1) goto done;
		for (sequence = 0; sequence < REPLAY_LIMIT; sequence++) {
			if (read_frame(third, replay) == -1) goto done;
		}
		if (!contains(replay, "\"sequence\":519")) goto done;
		if ((read_frame(third, replay) == -1) || !contains(replay, "\"sequence\":520")) goto done;
	}
	stage = "origin rejection";
	if (websocket(port, "https://attacker.example", 0) == -1) goto done;
	stage = "malformed handshake";
	if (rejected_handshake(port, "GET /xymonlive HTTP/1.1\r\n", "") == -1) goto done;
	if (rejected_handshake(port, "GET /xymonlive INVALID\r\n", "Connection: Upgrade\r\n") == -1) goto done;
	stage = "RSV rejection";
	invalid = websocket(port, origin, 1);
	if ((invalid == -1) || (read_frame(invalid, hello) == -1)) goto done;
	{
		int replay_index;
		for (replay_index = 0; replay_index < REPLAY_LIMIT; replay_index++) {
			if (read_frame(invalid, replay) == -1) goto done;
		}
	}
	if ((send_masked_first(invalid, 0xC9, "reserved") == -1) ||
	    (expect_closed(invalid) == -1)) goto done;
	close(invalid);
	invalid = -1;
	stage = "handshake timeout";
	if (incomplete_handshake_expires(port) == -1) goto done;
	result = 0;

done:
	if (first >= 0) close(first);
	if (second >= 0) close(second);
	if (third >= 0) close(third);
	if (invalid >= 0) close(invalid);
	close(inputpipe[1]);
	stop_child(child);
	if (result != 0) fprintf(stderr, "failed stage: %s\n", stage);
	return result;
}
