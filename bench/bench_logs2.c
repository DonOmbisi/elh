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

#define MAX_LINE  256
#define CBUF_SIZE 4096

int main() {
    printf("ELH Log Compression Benchmark\n");
    printf("Fair comparison: per-line block vs per-line streaming\n\n");

    char sample[MAX_LINE];
    int slen = gen_log_line(sample, 0);
    printf("Sample line (%d bytes):\n%s\n", slen, sample);

    printf("%-8s %-8s  %-12s %-8s  %-12s %-8s  %-8s\n",
           "Lines","Raw KB",
           "PerLine MB/s","PerLine%",
           "Stream MB/s","Stream%",
           "Gain");
    printf("%.75s\n",
           "--------------------------------------------------------------------------");

    /* Single cbuf allocation, never freed inside loop */
    char* cbuf = malloc(CBUF_SIZE);

    int counts[] = {100, 1000, 10000, 100000};
    for (int ci = 0; ci < 4; ci++) {
        int n = counts[ci];

        /* Build all log lines */
        char* data = malloc(n * MAX_LINE + 64);
        int*  offs = malloc((n+1) * sizeof(int));
        int   total = 0;
        for (int i = 0; i < n; i++) {
            offs[i] = total;
            total  += gen_log_line(data + total, i);
        }
        offs[n] = total;

        elh_params_t p = {4, 1, 1, 0};

        /* Block: compress each line independently, no history */
        int blk_total = 0;
        double t0 = now_sec();
        for (int i = 0; i < n; i++) {
            int llen = offs[i+1] - offs[i];
            int clen = elh_compress(data + offs[i], llen, cbuf, CBUF_SIZE, p);
            if (clen < 0) { printf("block error line %d\n", i); goto next; }
            blk_total += clen;
        }
        double blk_time = now_sec() - t0;

        /* Stream: compress each line with persistent cross-line history */
        {
            elh_stream_t* cs = elh_stream_new(p);
            int str_total = 0;
            t0 = now_sec();
            for (int i = 0; i < n; i++) {
                int llen = offs[i+1] - offs[i];
                int clen = elh_stream_compress(cs, data+offs[i], llen,
                                               cbuf, CBUF_SIZE);
                if (clen < 0) { printf("stream error line %d\n", i); break; }
                str_total += clen;
            }
            double str_time = now_sec() - t0;
            elh_stream_free(cs);

            double bmbs = (total/1e6) / blk_time;
            double smbs = (total/1e6) / str_time;
            double br   = 100.0 * blk_total / total;
            double sr   = 100.0 * str_total  / total;

            printf("%-8d %-8.1f  %-12.1f %-8.2f  %-12.1f %-8.2f  %+.2f%%\n",
                   n, total/1024.0,
                   bmbs, br, smbs, sr, br - sr);
        }

next:
        free(data);
        free(offs);
    }

    free(cbuf);
    printf("\nPerLine = block compress each line independently (LZ4 style)\n");
    printf("Stream  = ELH streaming with cross-line history\n");
    printf("Gain    = stream improvement (positive = stream wins)\n");
    return 0;
}
