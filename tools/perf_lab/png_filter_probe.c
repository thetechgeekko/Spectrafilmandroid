/*
 * #175: the filter decision, measured on a REAL Spektrafilm 16-bit export rather
 * than a synthetic pattern.
 *
 * Reads a PNG16 the app produced, inflates its IDAT (which the writer emitted with
 * filter 0 on every row), reconstructs the raw scanlines, then re-encodes them with
 * each PNG filter and both compressors. Everything is measured on the same pixels.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#include "libdeflate.h"

#define BPP 6

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c) {
    const int p = (int)a + (int)b - (int)c;
    const int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

static void filter_row(int type, const uint8_t* cur, const uint8_t* prev,
                       size_t row_bytes, uint8_t* dst) {
    for (size_t i = 0; i < row_bytes; ++i) {
        const uint8_t a = i >= BPP ? cur[i - BPP] : 0;
        const uint8_t b = prev ? prev[i] : 0;
        const uint8_t c = (prev && i >= BPP) ? prev[i - BPP] : 0;
        switch (type) {
            case 0: dst[i] = cur[i]; break;
            case 1: dst[i] = (uint8_t)(cur[i] - a); break;
            case 2: dst[i] = (uint8_t)(cur[i] - b); break;
            case 3: dst[i] = (uint8_t)(cur[i] - (uint8_t)(((int)a + (int)b) / 2)); break;
            default: dst[i] = (uint8_t)(cur[i] - paeth(a, b, c)); break;
        }
    }
}

static unsigned long row_score(const uint8_t* row, size_t n) {
    unsigned long sum = 0;
    for (size_t i = 0; i < n; ++i) {
        const int8_t v = (int8_t)row[i];
        sum += (unsigned long)(v < 0 ? -v : v);
    }
    return sum;
}

static void report(const char* label, double filter_ms, const uint8_t* src,
                   size_t len, size_t raw_len) {
    uLong bound = compressBound((uLong)len);
    uint8_t* dst = malloc(bound);
    z_stream s;
    memset(&s, 0, sizeof(s));
    deflateInit(&s, 6);
    s.next_in = (Bytef*)src; s.avail_in = (uInt)len;
    s.next_out = dst; s.avail_out = (uInt)bound;
    double t0 = now_ms();
    int rc; do { rc = deflate(&s, Z_FINISH); } while (rc == Z_OK);
    const double zms = now_ms() - t0;
    const unsigned long zout = s.total_out;
    deflateEnd(&s);
    free(dst);

    struct libdeflate_compressor* c = libdeflate_alloc_compressor(6);
    size_t lbound = libdeflate_zlib_compress_bound(c, len);
    uint8_t* ldst = malloc(lbound);
    t0 = now_ms();
    const size_t lout = libdeflate_zlib_compress(c, src, len, ldst, lbound);
    const double lms = now_ms() - t0;
    libdeflate_free_compressor(c);
    free(ldst);

    printf("  %-24s filter %5.0f ms | zlib %6.0f ms -> %6.1f MB (%.1f%%) | "
           "libdeflate %5.0f ms -> %6.1f MB (%.1f%%)\n",
           label, filter_ms,
           zms, zout / (1048576.0), 100.0 * zout / raw_len,
           lms, lout / (1048576.0), 100.0 * lout / raw_len);
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: real_filter_bench <file.png>\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    const long file_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* file_buf = malloc((size_t)file_len);
    if (fread(file_buf, 1, (size_t)file_len, f) != (size_t)file_len) return 2;
    fclose(f);

    /* Walk chunks: read IHDR, concatenate IDAT payloads. */
    int width = 0, height = 0, depth = 0, color = 0;
    uint8_t* idat = NULL;
    size_t idat_len = 0;
    size_t pos = 8;
    while (pos + 12 <= (size_t)file_len) {
        const uint32_t len = be32(file_buf + pos);
        const char* type = (const char*)(file_buf + pos + 4);
        if (memcmp(type, "IHDR", 4) == 0) {
            width = (int)be32(file_buf + pos + 8);
            height = (int)be32(file_buf + pos + 12);
            depth = file_buf[pos + 16];
            color = file_buf[pos + 17];
        } else if (memcmp(type, "IDAT", 4) == 0) {
            idat = realloc(idat, idat_len + len);
            memcpy(idat + idat_len, file_buf + pos + 8, len);
            idat_len += len;
        }
        pos += 12 + len;
    }
    printf("source: %s  %dx%d depth=%d color=%d  file %.1f MB  IDAT %.1f MB\n",
           argv[1], width, height, depth, color, file_len / 1048576.0,
           idat_len / 1048576.0);
    if (depth != 16 || color != 2) { printf("not 16-bit RGB\n"); return 2; }

    const size_t row_bytes = (size_t)width * 3 * 2;
    const size_t filtered_len = (row_bytes + 1) * (size_t)height;
    uint8_t* filtered = malloc(filtered_len);
    uLongf out_len = (uLongf)filtered_len;
    if (uncompress(filtered, &out_len, idat, (uLong)idat_len) != Z_OK ||
        out_len != filtered_len) {
        printf("inflate failed (%lu vs %zu)\n", (unsigned long)out_len, filtered_len);
        return 2;
    }

    /* Undo the writer's filters to get raw rows. The shipping writer uses filter 0
       on every row, so this also verifies that assumption. */
    uint8_t* raw = malloc(row_bytes * (size_t)height);
    long seen[5] = {0, 0, 0, 0, 0};
    for (int y = 0; y < height; ++y) {
        const uint8_t* src = filtered + (size_t)y * (row_bytes + 1);
        const int type = src[0];
        if (type >= 0 && type < 5) seen[type]++;
        uint8_t* cur = raw + (size_t)y * row_bytes;
        const uint8_t* prev = y > 0 ? raw + (size_t)(y - 1) * row_bytes : NULL;
        for (size_t i = 0; i < row_bytes; ++i) {
            const uint8_t a = i >= BPP ? cur[i - BPP] : 0;
            const uint8_t b = prev ? prev[i] : 0;
            const uint8_t c = (prev && i >= BPP) ? prev[i - BPP] : 0;
            const uint8_t x = src[1 + i];
            switch (type) {
                case 0: cur[i] = x; break;
                case 1: cur[i] = (uint8_t)(x + a); break;
                case 2: cur[i] = (uint8_t)(x + b); break;
                case 3: cur[i] = (uint8_t)(x + (uint8_t)(((int)a + (int)b) / 2)); break;
                default: cur[i] = (uint8_t)(x + paeth(a, b, c)); break;
            }
        }
    }
    printf("filters used by the shipping writer: none=%ld sub=%ld up=%ld avg=%ld paeth=%ld\n",
           seen[0], seen[1], seen[2], seen[3], seen[4]);

    const size_t raw_len = row_bytes * (size_t)height;
    uint8_t* out = malloc(filtered_len);
    uint8_t* scratch = malloc(row_bytes * 5);

    const int modes[] = {0, 1, 2, 4, -1};
    const char* names[] = {"filter 0 (SHIPS TODAY)", "filter 1 (Sub)", "filter 2 (Up)",
                           "filter 4 (Paeth)", "adaptive (min-sum)"};
    for (int m = 0; m < 5; ++m) {
        const int mode = modes[m];
        long counts[5] = {0, 0, 0, 0, 0};
        uint8_t* p = out;
        const double t0 = now_ms();
        for (int y = 0; y < height; ++y) {
            const uint8_t* cur = raw + (size_t)y * row_bytes;
            const uint8_t* prev = y > 0 ? raw + (size_t)(y - 1) * row_bytes : NULL;
            if (mode < 0) {
                unsigned long best = ~0UL;
                int chosen = 0;
                for (int t = 0; t < 5; ++t) {
                    uint8_t* cand = scratch + (size_t)t * row_bytes;
                    filter_row(t, cur, prev, row_bytes, cand);
                    const unsigned long score = row_score(cand, row_bytes);
                    if (score < best) { best = score; chosen = t; }
                }
                *p = (uint8_t)chosen;
                memcpy(p + 1, scratch + (size_t)chosen * row_bytes, row_bytes);
                counts[chosen]++;
            } else {
                *p = (uint8_t)mode;
                filter_row(mode, cur, prev, row_bytes, p + 1);
                counts[mode]++;
            }
            p += 1 + row_bytes;
        }
        const double fms = now_ms() - t0;
        report(names[m], fms, out, filtered_len, raw_len);
        if (mode < 0)
            printf("      adaptive picked: none=%ld sub=%ld up=%ld avg=%ld paeth=%ld\n",
                   counts[0], counts[1], counts[2], counts[3], counts[4]);
    }
    return 0;
}
