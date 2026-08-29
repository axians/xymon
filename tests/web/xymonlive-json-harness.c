#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xymonlive-json.inc"

int main(void)
{
	char *result = NULL;
	size_t length = 0;
	FILE *output = open_memstream(&result, &length);
	const char input[] = "host\"\\\n\t\001";
	const char expected[] = "\"host\\\"\\\\\\n\\t\\u0001\"";

	if (!output) return 2;
	json_string(output, input);
	if (fclose(output) != 0) return 2;
	if (strcmp(result, expected) != 0) {
		fprintf(stderr, "JSON mismatch: got [%s], expected [%s]\n", result, expected);
		free(result);
		return 1;
	}
	free(result);
	return 0;
}