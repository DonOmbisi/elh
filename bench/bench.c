#include "elh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: bench <file>\n"); return 1; }

    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    int n = (int)ftell(f);
    fseek(f, 0, SEEK_SET);
    char* src    = malloc(n);
    int   bound  = elh_compress_bound(n);
    char* comp   = malloc(bound);
    char* decomp = malloc(n);
    fread(src, 1, n, f);
    fclose(f);

    printf("%-12s %8d bytes\n", argv[1], n);
    printf("%-20s %8s %8s %10s %10s\n", "scheme", "ratio%", "bytes", "comp MB/s", "decomp MB/s");
    printf("%-20s %8s %8s %10s %10s\n", "------", "------", "-----", "---------", "-----------");

    int ks[]   = {1, 2, 4};
    int ovfs[] = {0, 1};
    int nk = 3, no = 2;
    int ki, oi, r;

    for (ki = 0; ki < nk; ki++) {
        for (oi = 0; oi < no; oi++) {
            int k = ks[ki], ovf = ovfs[oi];
            if (k == 1 && ovf) continue;
            elh_params_t p = {k, ovf, 1};
            char label[32];
            snprintf(label, sizeof(label), "ELH k=%d ovf=%d", k, ovf);

            /* Warm up */
            int cLen = elh_compress(src, n, comp, bound, p);
            elh_decompress(comp, cLen, decomp, n);

            /* Compress timing — 3 runs */
            double ct = 1e9;
            for (r = 0; r < 3; r++) {
                double t0 = now_sec();
                cLen = elh_compress(src, n, comp, bound, p);
                double t1 = now_sec();
                if (t1-t0 < ct) ct = t1-t0;
            }

            /* Decompress timing — 3 runs */
            double dt = 1e9;
            for (r = 0; r < 3; r++) {
                double t0 = now_sec();
                elh_decompress(comp, cLen, decomp, n);
                double t1 = now_sec();
                if (t1-t0 < dt) dt = t1-t0;
            }

            double ratio    = 100.0 * cLen / n;
            double comp_mbs = (n / 1e6) / ct;
            double dec_mbs  = (n / 1e6) / dt;
            int ok = memcmp(src, decomp, n) == 0;

            printf("%-20s %7.2f%% %8d %10.1f %10.1f %s\n",
                   label, ratio, cLen, comp_mbs, dec_mbs,
                   ok ? "" : "WRONG");
        }
    }

    free(src); free(comp); free(decomp);
    return 0;
}
