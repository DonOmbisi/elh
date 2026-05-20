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

static int gen_log_line(char* buf, int i) {
    const char* models[] = {"llama-3-8b","llama-3-70b","mistral-7b","mixtral-8x7b"};
    const char* tokens[] = {"the","a","is","to","and","of","in","that",
                             "it","was","for","on","are","as","with","his",
                             "they","at","be","this","from","or","had","by",
                             "hot","but","some","what","there","we","can","out"};
    const char* statuses[] = {"OK","OK","OK","OK","TIMEOUT","ERROR"};
    return sprintf(buf,
        "{\"ts\":%d.%03d,\"req_id\":\"req-%04d\",\"model\":\"%s\","
        "\"token\":\"%s\",\"latency_ms\":%d,\"status\":\"%s\","
        "\"gpu\":0,\"worker\":\"w%d\"}\n",
        1700000000 + i/100, (i%100)*10,
        1000 + i/20,
        models[(i/500)%4], tokens[i%32],
        12 + i%8, statuses[i%6],
        i%4);
}

#define MAX_LINE 256

int main() {
    printf("ELH Log Compression Benchmark\n");
    printf("Simulating LLM inference logs (vLLM/TGI style)\n\n");

    char sample[MAX_LINE];
    int slen = gen_log_line(sample, 0);
    printf("Sample line (%d bytes):\n%s\n", slen, sample);

    printf("%-8s %-8s  %-14s %-10s  %-14s %-10s  %-8s\n",
           "Lines","Raw KB","Block MB/s","Block%","Stream MB/s","Stream%","Gain");
    printf("%.80s\n", "----------------------------------------"
                      "----------------------------------------");

    int counts[] = {100, 1000, 10000, 100000};
    for (int ci = 0; ci < 4; ci++) {
        int n = counts[ci];

        /* Build log data */
        char* data = malloc(n * MAX_LINE);
        int*  offs = malloc((n+1) * sizeof(int));
        int   total = 0;
        for (int i = 0; i < n; i++) {
            offs[i] = total;
            total  += gen_log_line(data + total, i);
        }
        offs[n] = total;

        elh_params_t p = {4, 1, 1, 0};

        /* Block: compress whole file at once */
        int bbound = elh_compress_bound(total) + 64;
        char* bcomp = malloc(bbound);
        double t0 = now_sec();
        int blen = elh_compress(data, total, bcomp, bbound, p);
        double btime = now_sec() - t0;

        /* Stream: compress line by line, accumulate output */
        /* Allocate generously: each line expands at most ~line+32 bytes */
        char* scomp = malloc(total + n * 32 + 1024);
        elh_stream_t* cs = elh_stream_new(p);
        int slen2 = 0;
        /* Use a fixed output buffer per line, sized safely */
        int linebuf_size = MAX_LINE * 2 + 64;
        char* linebuf = malloc(linebuf_size);
        t0 = now_sec();
        for (int i = 0; i < n; i++) {
            int llen = offs[i+1] - offs[i];
            int clen = elh_stream_compress(cs,
                                           data + offs[i], llen,
                                           linebuf, linebuf_size);
            if (clen < 0) { printf("stream error at line %d\n", i); break; }
            memcpy(scomp + slen2, linebuf, clen);
            slen2 += clen;
        }
        double stime = now_sec() - t0;
        elh_stream_free(cs);
        free(linebuf);

        /* Verify block round-trip */
        char* decomp = malloc(total + 1);
        int dlen = elh_decompress(bcomp, blen, decomp, total);
        int ok = (dlen == total && memcmp(data, decomp, total) == 0);

        double bmbs  = (total/1e6) / btime;
        double smbs  = (total/1e6) / stime;
        double br    = 100.0 * blen  / total;
        double sr    = 100.0 * slen2 / total;
        double gain  = br - sr;

        printf("%-8d %-8.1f  %-14.1f %-10.2f  %-14.1f %-10.2f  %+.2f%% %s\n",
               n, total/1024.0,
               bmbs, br,
               smbs, sr,
               gain, ok?"":"FAIL");

        free(data); free(offs); free(bcomp); free(scomp); free(decomp);
    }

    printf("\nBlock  = whole-file compression\n");
    printf("Stream = line-by-line with cross-line history\n");
    printf("Gain   = stream improvement over block (positive = stream wins)\n");
    return 0;
}
