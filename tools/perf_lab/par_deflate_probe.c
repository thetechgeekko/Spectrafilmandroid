/*
 * #175: can the PNG16 encode use the eight cores the phone already has?
 *
 * pigz's trick: split the filtered scanlines into bands, compress each band with
 * its own zlib stream ending in Z_SYNC_FLUSH (so no band emits a final block), and
 * concatenate. The result is ONE valid deflate stream, so it stays a single IDAT --
 * only the block boundaries differ from a serial encode, which costs a little
 * ratio and no correctness.
 *
 * Measured against the serial encode on the same real payload, and the output is
 * inflated back and compared with the input so a faster wrong answer cannot pass.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

typedef struct {
    const uint8_t* src;
    size_t len;
    uint8_t* dst;
    size_t dst_cap;
    size_t out;
    int last;       /* the final band finishes the stream */
    int level;
} Band;

static void* run_band(void* arg) {
    Band* b = (Band*)arg;
    z_stream s;
    memset(&s, 0, sizeof(s));
    /* Raw deflate for every band; the zlib header/trailer is added by the caller so
       the concatenation is a single valid zlib stream. */
    deflateInit2(&s, b->level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    s.next_in = (Bytef*)b->src;
    s.avail_in = (uInt)b->len;
    s.next_out = b->dst;
    s.avail_out = (uInt)b->dst_cap;
    if (b->last) {
        int rc;
        do { rc = deflate(&s, Z_FINISH); } while (rc == Z_OK);
    } else {
        deflate(&s, Z_SYNC_FLUSH);
    }
    b->out = b->dst_cap - s.avail_out;
    deflateEnd(&s);
    return NULL;
}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : NULL;
    const int workers = argc > 2 ? atoi(argv[2]) : 8;
    if (!path) { printf("usage: par_deflate <payload.bin> [workers]\n"); return 2; }

    FILE* f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); return 2; }
    fseek(f, 0, SEEK_END);
    const size_t len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* src = malloc(len);
    if (fread(src, 1, len, f) != len) return 2;
    fclose(f);
    printf("payload %.1f MB, %d workers\n", len / 1048576.0, workers);

    /* Serial reference, exactly what ships. */
    uLong bound = compressBound((uLong)len);
    uint8_t* ref = malloc(bound);
    z_stream s;
    memset(&s, 0, sizeof(s));
    deflateInit(&s, 6);
    s.next_in = src; s.avail_in = (uInt)len;
    s.next_out = ref; s.avail_out = (uInt)bound;
    double t0 = now_ms();
    int rc; do { rc = deflate(&s, Z_FINISH); } while (rc == Z_OK);
    const double serial_ms = now_ms() - t0;
    const size_t serial_out = s.total_out;
    deflateEnd(&s);
    printf("  serial zlib          %7.0f ms -> %6.1f MB\n",
           serial_ms, serial_out / 1048576.0);

    for (int w = 2; w <= workers; w *= 2) {
        Band* bands = calloc((size_t)w, sizeof(Band));
        pthread_t* tids = calloc((size_t)w, sizeof(pthread_t));
        const size_t chunk = (len + (size_t)w - 1) / (size_t)w;
        for (int i = 0; i < w; ++i) {
            const size_t off = (size_t)i * chunk;
            bands[i].src = src + off;
            bands[i].len = off + chunk > len ? len - off : chunk;
            bands[i].dst_cap = (size_t)compressBound((uLong)bands[i].len) + 64;
            bands[i].dst = malloc(bands[i].dst_cap);
            bands[i].last = (i == w - 1);
            bands[i].level = 6;
        }
        t0 = now_ms();
        for (int i = 0; i < w; ++i) pthread_create(&tids[i], NULL, run_band, &bands[i]);
        for (int i = 0; i < w; ++i) pthread_join(tids[i], NULL);
        size_t total = 2;  /* zlib header */
        for (int i = 0; i < w; ++i) total += bands[i].out;
        total += 4;        /* adler32 trailer */
        const double par_ms = now_ms() - t0;

        /* Assemble a real zlib stream and verify it inflates to the input. */
        uint8_t* out = malloc(total + 16);
        size_t p = 0;
        out[p++] = 0x78; out[p++] = 0x9C;   /* CMF/FLG for deflate, default window */
        for (int i = 0; i < w; ++i) {
            memcpy(out + p, bands[i].dst, bands[i].out);
            p += bands[i].out;
        }
        uLong ad = adler32(0L, Z_NULL, 0);
        ad = adler32(ad, src, (uInt)len);
        out[p++] = (uint8_t)((ad >> 24) & 0xFF);
        out[p++] = (uint8_t)((ad >> 16) & 0xFF);
        out[p++] = (uint8_t)((ad >> 8) & 0xFF);
        out[p++] = (uint8_t)(ad & 0xFF);

        uint8_t* back = malloc(len);
        uLongf back_len = (uLongf)len;
        const int urc = uncompress(back, &back_len, out, (uLong)p);
        const int ok = urc == Z_OK && back_len == len && memcmp(back, src, len) == 0;
        printf("  parallel zlib x%-2d    %7.0f ms -> %6.1f MB   %5.2fx   round-trip %s\n",
               w, par_ms, p / 1048576.0, serial_ms / par_ms,
               ok ? "OK" : "FAILED");
        free(back); free(out);
        for (int i = 0; i < w; ++i) free(bands[i].dst);
        free(bands); free(tids);
    }
    free(ref); free(src);
    return 0;
}
