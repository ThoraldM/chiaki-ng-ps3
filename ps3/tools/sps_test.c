// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// Test for src/h264_sps.c: sps_test <rewrite|strip-bsr|strip-vui> in.h264 out.h264
// Build: gcc -DH264_SPS_TEST -O2 -o sps_test tools/sps_test.c src/h264_sps.c
#include "../src/h264_sps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
size_t h264_sps_test_strip(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_cap, int mode);
int main(int argc, char **argv)
{
	if(argc != 4) return 2;
	FILE *f = fopen(argv[2], "rb"); if(!f) return 1;
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t *in = malloc(n), *out = malloc(n + 4096);
	if(fread(in, 1, n, f) != (size_t)n) return 1;
	fclose(f);
	size_t on; int count = 0;
	if(!strcmp(argv[1], "rewrite")) on = h264_low_latency_rewrite(in, n, out, n + 4096, &count);
	else on = h264_sps_test_strip(in, n, out, n + 4096, !strcmp(argv[1], "strip-bsr") ? 1 : 2);
	f = fopen(argv[3], "wb"); fwrite(out, 1, on, f); fclose(f);
	printf("%s: %ld -> %zu bytes, %d SPS rewritten\n", argv[1], n, on, count);
	return on ? 0 : 1;
}
