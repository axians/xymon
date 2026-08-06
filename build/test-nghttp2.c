#include <stdio.h>

#include <nghttp2/nghttp2.h>

int main(int argc, char *argv[])
{
	nghttp2_session_callbacks *callbacks = NULL;

	if (nghttp2_session_callbacks_new(&callbacks) != 0) {
		printf("nghttp2_session_callbacks_new() failed\n");
		return 1;
	}
	nghttp2_session_callbacks_del(callbacks);

	printf("nghttp2 version: %s\n", nghttp2_version(0)->version_str);
	return 0;
}
