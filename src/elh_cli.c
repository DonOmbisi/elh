#include "elh_frame.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    MODE_NONE = 0,
    MODE_COMPRESS,
    MODE_DECOMPRESS
} cli_mode_t;

static void usage(FILE* out) {
    fprintf(out,
        "usage: elh_cli -c|-d [options] <input> <output>\n"
        "\n"
        "modes:\n"
        "  -c, --compress       compress input to an ELH frame\n"
        "  -d, --decompress     decompress an ELH frame\n"
        "\n"
        "compression options:\n"
        "  -k N                 bucket depth: 1, 2, 4, or 0 adaptive (default 4)\n"
        "  --overflow N         enable A2 overflow table: 0 or 1 (default 1)\n"
        "  --accel N            acceleration 1-9 (default 1)\n"
        "  --chunk N            frame chunk size, max 65536 (default 65536)\n"
        "  --no-raw             disable raw fallback for expanding chunks\n"
        "\n"
        "other:\n"
        "  -h, --help           show this help\n");
}

static int parse_int(const char* s, int* out) {
    char* end = NULL;
    long v;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || !end || *end != '\0' || v < -2147483647L || v > 2147483647L)
        return -1;
    *out = (int)v;
    return 0;
}

static int read_file(const char* path, char** data, int* size) {
    FILE* f = fopen(path, "rb");
    long n;
    char* buf;

    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    n = ftell(f);
    if (n < 0 || n > 0x7fffffff) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }

    buf = (char*)malloc((size_t)n ? (size_t)n : 1);
    if (!buf) { fclose(f); return -1; }
    if (n && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    *data = buf;
    *size = (int)n;
    return 0;
}

static int write_file(const char* path, const char* data, int size) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    if (size && fwrite(data, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0) return -1;
    return 0;
}

static int compress_file(const char* inPath, const char* outPath,
                         elh_frame_params_t params) {
    char* in = NULL;
    char* out = NULL;
    int inSize = 0;
    int bound, outSize;
    int rc = 1;

    if (read_file(inPath, &in, &inSize) != 0) {
        perror(inPath);
        goto done;
    }

    bound = elh_frame_compress_bound(inSize, params.chunk_size);
    if (bound < 0) {
        fprintf(stderr, "invalid compression bound\n");
        goto done;
    }
    out = (char*)malloc((size_t)bound);
    if (!out) {
        fprintf(stderr, "allocation failed\n");
        goto done;
    }

    outSize = elh_frame_compress(in, inSize, out, bound, params);
    if (outSize < 0) {
        fprintf(stderr, "compression failed\n");
        goto done;
    }
    if (write_file(outPath, out, outSize) != 0) {
        perror(outPath);
        goto done;
    }

    fprintf(stderr, "%s: %d -> %d bytes (%.2f%%)\n",
            inPath, inSize, outSize, inSize ? 100.0 * outSize / inSize : 0.0);
    rc = 0;

done:
    free(in);
    free(out);
    return rc;
}

static int decompress_file(const char* inPath, const char* outPath) {
    char* in = NULL;
    char* out = NULL;
    int inSize = 0;
    int outCap, outSize;
    int rc = 1;

    if (read_file(inPath, &in, &inSize) != 0) {
        perror(inPath);
        goto done;
    }

    outCap = elh_frame_get_original_size(in, inSize);
    if (outCap < 0) {
        fprintf(stderr, "invalid ELH frame: %s\n", inPath);
        goto done;
    }
    out = (char*)malloc((size_t)outCap ? (size_t)outCap : 1);
    if (!out) {
        fprintf(stderr, "allocation failed\n");
        goto done;
    }

    outSize = elh_frame_decompress(in, inSize, out, outCap);
    if (outSize < 0) {
        fprintf(stderr, "decompression failed\n");
        goto done;
    }
    if (write_file(outPath, out, outSize) != 0) {
        perror(outPath);
        goto done;
    }

    fprintf(stderr, "%s: %d -> %d bytes\n", inPath, inSize, outSize);
    rc = 0;

done:
    free(in);
    free(out);
    return rc;
}

int main(int argc, char** argv) {
    cli_mode_t mode = MODE_NONE;
    elh_frame_params_t params = ELH_FRAME_PARAMS_DEFAULT;
    const char* input = NULL;
    const char* output = NULL;

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(arg, "-c") == 0 || strcmp(arg, "--compress") == 0) {
            mode = MODE_COMPRESS;
        } else if (strcmp(arg, "-d") == 0 || strcmp(arg, "--decompress") == 0) {
            mode = MODE_DECOMPRESS;
        } else if (strcmp(arg, "-k") == 0) {
            if (++i >= argc || parse_int(argv[i], &params.params.bucket_k) != 0) {
                fprintf(stderr, "-k requires an integer\n");
                return 1;
            }
        } else if (strcmp(arg, "--overflow") == 0) {
            if (++i >= argc || parse_int(argv[i], &params.params.use_overflow) != 0) {
                fprintf(stderr, "--overflow requires 0 or 1\n");
                return 1;
            }
        } else if (strcmp(arg, "--accel") == 0) {
            if (++i >= argc || parse_int(argv[i], &params.params.acceleration) != 0) {
                fprintf(stderr, "--accel requires an integer\n");
                return 1;
            }
        } else if (strcmp(arg, "--chunk") == 0) {
            if (++i >= argc || parse_int(argv[i], &params.chunk_size) != 0) {
                fprintf(stderr, "--chunk requires an integer\n");
                return 1;
            }
        } else if (strcmp(arg, "--no-raw") == 0) {
            params.store_uncompressed = 0;
        } else if (!input) {
            input = arg;
        } else if (!output) {
            output = arg;
        } else {
            fprintf(stderr, "unexpected argument: %s\n", arg);
            usage(stderr);
            return 1;
        }
    }

    if (mode == MODE_NONE || !input || !output) {
        usage(stderr);
        return 1;
    }

    if (mode == MODE_COMPRESS)
        return compress_file(input, output, params);
    return decompress_file(input, output);
}
