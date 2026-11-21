#include "native_jpeg_fast_stream.h"
#include "native_jpeg_writer.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <pthread.h>
#include <sys/time.h>
#endif

static uint64_t fs_now_ms(void) {
#ifndef _WIN32
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
#else
    return 0;
#endif
}

static int s_cpu_probed = 0;
static int s_cpu_ncpu = 1;
static int s_cpu_neon = 0;

static void fs_probe_cpu(int* out_ncpu, int* out_neon, int* out_v7) {
    if (!s_cpu_probed) {
        int procs = 0, neon = 0;
        FILE* f = fopen("/proc/cpuinfo", "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "processor", 9) == 0) {
                    procs++;
                } else if (strncmp(line, "Features", 8) == 0) {
                    if (strstr(line, "neon") != NULL || strstr(line, "asimd") != NULL) neon = 1;
                }
            }
            fclose(f);
        }
        s_cpu_ncpu = (procs > 0) ? procs : 1;
        s_cpu_neon = neon;
        s_cpu_probed = 1;
    }
    if (out_ncpu) *out_ncpu = s_cpu_ncpu;
    if (out_neon) *out_neon = s_cpu_neon;
#if defined(__ARM_ARCH_7A__) || defined(__aarch64__)
    if (out_v7) *out_v7 = 1;
#else
    if (out_v7) *out_v7 = 0;
#endif
}

static const uint8_t FS_ZIGZAG[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5, 12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51, 58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};

// Q12 AAN 1D IDCT scale factors: round(4096 * a_k) where a_0=1, a_k=sqrt(2)*cos(k*pi/16)
static const uint32_t FS_AAN_Q12[8] = { 4096, 5681, 5352, 4816, 4096, 3218, 2217, 1130 };

// Standard JPEG Annex K default Huffman tables (pre-loaded before parsing markers; overridden by DHT)
static const uint8_t STD_DC_LUMA_BITS[17] = { 0, 0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0 };
static const uint8_t STD_DC_LUMA_VALS[12] = { 0,1,2,3,4,5,6,7,8,9,10,11 };
static const uint8_t STD_DC_CHROMA_BITS[17] = { 0, 0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0 };
static const uint8_t STD_DC_CHROMA_VALS[12] = { 0,1,2,3,4,5,6,7,8,9,10,11 };
static const uint8_t STD_AC_LUMA_BITS[17] = { 0, 0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7D };
static const uint8_t STD_AC_LUMA_VALS[162] = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xA1,0x08,
    0x23,0x42,0xB1,0xC1,0x15,0x52,0xD1,0xF0,0x24,0x33,0x62,0x72,0x82,0x09,0x0A,0x16,0x17,0x18,0x19,0x1A,0x25,0x26,0x27,0x28,
    0x29,0x2A,0x34,0x35,0x36,0x37,0x38,0x39,0x3A,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4A,0x53,0x54,0x55,0x56,0x57,0x58,0x59,
    0x5A,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6A,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8A,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9A,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,0xA8,0xA9,0xAA,0xB2,0xB3,0xB4,0xB5,0xB6,
    0xB7,0xB8,0xB9,0xBA,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,0xE1,0xE2,
    0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9,0xFA
};
static const uint8_t STD_AC_CHROMA_BITS[17] = { 0, 0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77 };
static const uint8_t STD_AC_CHROMA_VALS[162] = {
    0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,
    0xA1,0xB1,0xC1,0x09,0x23,0x33,0x52,0xF0,0x15,0x62,0x72,0xD1,0x0A,0x16,0x24,0x34,0xE1,0x25,0xF1,0x17,0x18,0x19,0x1A,0x26,
    0x27,0x28,0x29,0x2A,0x35,0x36,0x37,0x38,0x39,0x3A,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4A,0x53,0x54,0x55,0x56,0x57,0x58,
    0x59,0x5A,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6A,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8A,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9A,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,0xA8,0xA9,0xAA,0xB2,0xB3,0xB4,
    0xB5,0xB6,0xB7,0xB8,0xB9,0xBA,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,
    0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9,0xFA
};

// Pre-baked 17^3 direct YCbCr -> YCbCr 3D LUT (19.6KB, fits in Cortex-A5 32KB L1 cache)
// Entry layout: [y_signed, cb_signed, cr_signed, 0] in [-128..127], indexed by ((cr*289 + cb*17 + y) << 2)
#define YCC_LUT_BYTES (17 * 17 * 17 * 4)

static void build_ycc_lut17(const Lut17Context* rgb_lut, int8_t* ycc_lut) {
    for (int cri = 0; cri < 17; cri++) {
        int cr = ((cri * 255 + 8) >> 4) - 128;
        int r_uv = (91881 * cr + 32768) >> 16;
        for (int cbi = 0; cbi < 17; cbi++) {
            int cb = ((cbi * 255 + 8) >> 4) - 128;
            int g_uv = -((22554 * cb + 46802 * cr + 32768) >> 16);
            int b_uv = (116130 * cb + 32768) >> 16;
            int base = (cri * 289 + cbi * 17) << 2;
            for (int yi = 0; yi < 17; yi++) {
                int Y = (yi * 255 + 8) >> 4;
                int r0 = Y + r_uv;
                int g0 = Y + g_uv;
                int b0 = Y + b_uv;
                uint8_t r = (uint8_t)(r0 < 0 ? 0 : (r0 > 255 ? 255 : r0));
                uint8_t g = (uint8_t)(g0 < 0 ? 0 : (g0 > 255 ? 255 : g0));
                uint8_t b = (uint8_t)(b0 < 0 ? 0 : (b0 > 255 ? 255 : b0));
                uint8_t or_v, og_v, ob_v;
                lut17_interpolate_pixel(rgb_lut, r, g, b, &or_v, &og_v, &ob_v);
                int oy  = ((19595 * (int)or_v + 38470 * (int)og_v +  7471 * (int)ob_v + 32768) >> 16) - 128;
                int ocb = (-11059 * (int)or_v - 21709 * (int)og_v + 32768 * (int)ob_v + 32768) >> 16;
                int ocr = ( 32768 * (int)or_v - 27439 * (int)og_v -  5329 * (int)ob_v + 32768) >> 16;
                int8_t* dst = ycc_lut + base + (yi << 2);
                dst[0] = (int8_t)(oy  < -128 ? -128 : (oy  > 127 ? 127 : oy));
                dst[1] = (int8_t)(ocb < -128 ? -128 : (ocb > 127 ? 127 : ocb));
                dst[2] = (int8_t)(ocr < -128 ? -128 : (ocr > 127 ? 127 : ocr));
                dst[3] = 0;
            }
        }
    }
}

typedef struct {
    uint16_t orientation;
    char datetime[20];
    int has_datetime;
    uint32_t exp_num, exp_den;
    int has_exp;
    uint32_t fnum_num, fnum_den;
    int has_fnum;
    uint16_t iso;
    int has_iso;
    uint32_t flen_num, flen_den;
    int has_flen;
} CleanExifInfo;

static uint16_t rd_u16(const uint8_t* p, int le) {
    return le ? (uint16_t)(p[0] | ((uint16_t)p[1] << 8)) : (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t rd_u32(const uint8_t* p, int le) {
    if (le) return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void wr_u16_le(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8); }
static void wr_u32_le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void parse_exif_ifd(const uint8_t* tiff, size_t tiff_len, uint32_t ifd_off, int le, CleanExifInfo* info, int is_exif_sub) {
    if (ifd_off + 2 > tiff_len) return;
    uint16_t n = rd_u16(tiff + ifd_off, le);
    if (n > 200) n = 200;
    for (uint16_t i = 0; i < n; i++) {
        size_t e = (size_t)ifd_off + 2 + (size_t)i * 12;
        if (e + 12 > tiff_len) break;
        uint16_t tag = rd_u16(tiff + e, le);
        uint16_t typ = rd_u16(tiff + e + 2, le);
        uint32_t cnt = rd_u32(tiff + e + 4, le);
        uint32_t val = rd_u32(tiff + e + 8, le);
        if (!is_exif_sub) {
            if (tag == 0x0112 && typ == 3 && cnt >= 1) {
                uint16_t ori = rd_u16(tiff + e + 8, le);
                if (ori >= 1 && ori <= 8) info->orientation = ori;
            } else if (tag == 0x0132 && typ == 2 && cnt >= 19 && val + 19 <= tiff_len) {
                memcpy(info->datetime, tiff + val, 19);
                info->datetime[19] = '\0';
                info->has_datetime = 1;
            } else if (tag == 0x8769 && (typ == 4 || typ == 3)) {
                parse_exif_ifd(tiff, tiff_len, val, le, info, 1);
            }
        } else {
            if (tag == 0x829A && typ == 5 && val + 8 <= tiff_len) {
                info->exp_num = rd_u32(tiff + val, le);
                info->exp_den = rd_u32(tiff + val + 4, le);
                if (info->exp_den > 0) info->has_exp = 1;
            } else if (tag == 0x829D && typ == 5 && val + 8 <= tiff_len) {
                info->fnum_num = rd_u32(tiff + val, le);
                info->fnum_den = rd_u32(tiff + val + 4, le);
                if (info->fnum_den > 0) info->has_fnum = 1;
            } else if (tag == 0x8827 && typ == 3 && cnt >= 1) {
                info->iso = rd_u16(tiff + e + 8, le);
                if (info->iso > 0) info->has_iso = 1;
            } else if (tag == 0x9003 && !info->has_datetime && typ == 2 && cnt >= 19 && val + 19 <= tiff_len) {
                memcpy(info->datetime, tiff + val, 19);
                info->datetime[19] = '\0';
                info->has_datetime = 1;
            } else if (tag == 0x920A && typ == 5 && val + 8 <= tiff_len) {
                info->flen_num = rd_u32(tiff + val, le);
                info->flen_den = rd_u32(tiff + val + 4, le);
                if (info->flen_den > 0) info->has_flen = 1;
            }
        }
    }
}

static void parse_exif_app1(const uint8_t* payload, size_t payload_len, CleanExifInfo* info) {
    if (payload_len < 14) return;
    if (memcmp(payload, "Exif\0\0", 6) != 0) return;
    const uint8_t* tiff = payload + 6;
    size_t tiff_len = payload_len - 6;
    int le;
    if (tiff[0] == 'I' && tiff[1] == 'I') le = 1;
    else if (tiff[0] == 'M' && tiff[1] == 'M') le = 0;
    else return;
    if (rd_u16(tiff + 2, le) != 42) return;
    uint32_t ifd0 = rd_u32(tiff + 4, le);
    parse_exif_ifd(tiff, tiff_len, ifd0, le, info, 0);
}

static size_t build_clean_exif_app1(uint8_t* out, size_t max_len, const CleanExifInfo* info, int dst_w, int dst_h) {
    if (max_len < 512) return 0;
    uint8_t* tiff = out + 10;
    tiff[0] = 'I'; tiff[1] = 'I'; wr_u16_le(tiff + 2, 42); wr_u32_le(tiff + 4, 8);

    int ifd0_cnt = info->has_datetime ? 9 : 8;
    int exif_cnt = 4 + info->has_exp + info->has_fnum + info->has_iso + info->has_datetime + info->has_flen;

    uint32_t ifd0_off = 8;
    uint32_t exif_off = ifd0_off + 2 + (uint32_t)ifd0_cnt * 12 + 4;
    uint32_t data_off = exif_off + 2 + (uint32_t)exif_cnt * 12 + 4;

    uint8_t* e0 = tiff + ifd0_off;
    wr_u16_le(e0, (uint16_t)ifd0_cnt); e0 += 2;
    uint8_t* ee = tiff + exif_off;
    wr_u16_le(ee, (uint16_t)exif_cnt); ee += 2;

    #define PUT_ENTRY(p, tag, typ, cnt, val) do { \
        wr_u16_le((p), (uint16_t)(tag)); wr_u16_le((p)+2, (uint16_t)(typ)); \
        wr_u32_le((p)+4, (uint32_t)(cnt)); wr_u32_le((p)+8, (uint32_t)(val)); (p) += 12; } while(0)
    #define ALLOC_DATA(src, sz, out_off) do { \
        (out_off) = data_off; memcpy(tiff + data_off, (src), (sz)); data_off += (uint32_t)(sz); \
        if (data_off & 1) { tiff[data_off++] = 0; } } while(0)

    uint32_t make_off, model_off, xres_off, yres_off, sw_off, dt_off = 0;
    ALLOC_DATA("SONY\0", 5, make_off);
    ALLOC_DATA("ILCE-7RM2\0", 10, model_off);
    uint8_t rat72[8]; wr_u32_le(rat72, 72); wr_u32_le(rat72 + 4, 1);
    ALLOC_DATA(rat72, 8, xres_off);
    ALLOC_DATA(rat72, 8, yres_off);
    ALLOC_DATA("FujiLite A7R2\0", 14, sw_off);
    if (info->has_datetime) ALLOC_DATA(info->datetime, 20, dt_off);

    PUT_ENTRY(e0, 0x010F, 2, 5, make_off);
    PUT_ENTRY(e0, 0x0110, 2, 10, model_off);
    PUT_ENTRY(e0, 0x0112, 3, 1, info->orientation ? info->orientation : 1);
    PUT_ENTRY(e0, 0x011A, 5, 1, xres_off);
    PUT_ENTRY(e0, 0x011B, 5, 1, yres_off);
    PUT_ENTRY(e0, 0x0128, 3, 1, 2);
    PUT_ENTRY(e0, 0x0131, 2, 14, sw_off);
    if (info->has_datetime) PUT_ENTRY(e0, 0x0132, 2, 20, dt_off);
    PUT_ENTRY(e0, 0x8769, 4, 1, exif_off);
    wr_u32_le(e0, 0);

    if (info->has_exp) {
        uint8_t r[8]; uint32_t o; wr_u32_le(r, info->exp_num); wr_u32_le(r + 4, info->exp_den);
        ALLOC_DATA(r, 8, o); PUT_ENTRY(ee, 0x829A, 5, 1, o);
    }
    if (info->has_fnum) {
        uint8_t r[8]; uint32_t o; wr_u32_le(r, info->fnum_num); wr_u32_le(r + 4, info->fnum_den);
        ALLOC_DATA(r, 8, o); PUT_ENTRY(ee, 0x829D, 5, 1, o);
    }
    if (info->has_iso) PUT_ENTRY(ee, 0x8827, 3, 1, info->iso);
    PUT_ENTRY(ee, 0x9000, 7, 4, 0x30333230U);
    if (info->has_datetime) PUT_ENTRY(ee, 0x9003, 2, 20, dt_off);
    if (info->has_flen) {
        uint8_t r[8]; uint32_t o; wr_u32_le(r, info->flen_num); wr_u32_le(r + 4, info->flen_den);
        ALLOC_DATA(r, 8, o); PUT_ENTRY(ee, 0x920A, 5, 1, o);
    }
    PUT_ENTRY(ee, 0xA001, 3, 1, 1);
    PUT_ENTRY(ee, 0xA002, 4, 1, (uint32_t)dst_w);
    PUT_ENTRY(ee, 0xA003, 4, 1, (uint32_t)dst_h);
    wr_u32_le(ee, 0);

    #undef PUT_ENTRY
    #undef ALLOC_DATA

    size_t pos = 10 + (size_t)data_off;
    uint16_t seg_len = (uint16_t)(pos - 2);
    out[0] = 0xFF; out[1] = 0xE1;
    out[2] = (uint8_t)(seg_len >> 8); out[3] = (uint8_t)(seg_len & 0xFF);
    memcpy(out + 4, "Exif\0\0", 6);
    return pos;
}

// Fast 10-bit lookup Huffman table
typedef struct {
    uint8_t fast_sym[1024];
    uint8_t fast_len[1024];
    int32_t maxcode[18];
    int32_t valoffset[18];
    uint8_t huffval[256];
    int valid;
} FastHuffTable;

static void build_fast_huff(FastHuffTable* t, const uint8_t bits[17], const uint8_t* vals, int total) {
    memset(t, 0, sizeof(FastHuffTable));
    if (total > 256) total = 256;
    memcpy(t->huffval, vals, (size_t)total);
    int code = 0, k = 0;
    for (int len = 1; len <= 16; len++) {
        int cnt = bits[len];
        if (cnt > 0 && k + cnt <= 256) {
            t->valoffset[len] = k - code;
            for (int i = 0; i < cnt; i++) {
                uint8_t sym = vals[k++];
                if (len <= 10) {
                    int base = code << (10 - len);
                    int rep = 1 << (10 - len);
                    for (int r = 0; r < rep; r++) {
                        t->fast_sym[base + r] = sym;
                        t->fast_len[base + r] = (uint8_t)len;
                    }
                }
                code++;
            }
            t->maxcode[len] = code - 1;
        } else {
            t->maxcode[len] = -1;
            t->valoffset[len] = 0;
        }
        code <<= 1;
    }
    t->maxcode[17] = 0x7FFFFFFF;
    t->valid = 1;
}

// Buffered 64KB MSB-aligned JPEG entropy reader
typedef struct {
    FILE* fp;
    uint8_t buf[65536];
    size_t pos, len;
    uint32_t bitbuf;
    int bitcnt;
    int marker_hit;
    uint8_t pending_marker;
    int eof;
} FastBitReader;

static inline int fbr_next_raw_byte(FastBitReader* br) {
    if (br->pos >= br->len) {
        br->len = fread(br->buf, 1, sizeof(br->buf), br->fp);
        br->pos = 0;
        if (br->len == 0) { br->eof = 1; return -1; }
    }
    return (int)br->buf[br->pos++];
}

static void fbr_fill(FastBitReader* br) {
    while (br->bitcnt <= 24 && br->pos < br->len && !br->marker_hit) {
        uint8_t b = br->buf[br->pos++];
        if (b != 0xFF) {
            br->bitbuf |= ((uint32_t)b << (24 - br->bitcnt));
            br->bitcnt += 8;
        } else {
            int nxt;
            do { nxt = fbr_next_raw_byte(br); } while (nxt == 0xFF);
            if (nxt == 0x00) {
                br->bitbuf |= (0xFFU << (24 - br->bitcnt));
                br->bitcnt += 8;
            } else {
                br->marker_hit = 1;
                br->pending_marker = (nxt >= 0) ? (uint8_t)nxt : 0xD9;
                br->bitcnt += 8;
            }
        }
    }
    while (br->bitcnt <= 24) {
        if (br->marker_hit || br->eof) {
            br->bitcnt += 8;
            continue;
        }
        int b = fbr_next_raw_byte(br);
        if (b < 0) {
            br->bitcnt += 8;
            continue;
        }
        if (b == 0xFF) {
            int nxt;
            do { nxt = fbr_next_raw_byte(br); } while (nxt == 0xFF);
            if (nxt == 0x00) {
                br->bitbuf |= (0xFFU << (24 - br->bitcnt));
                br->bitcnt += 8;
            } else {
                br->marker_hit = 1;
                br->pending_marker = (nxt >= 0) ? (uint8_t)nxt : 0xD9;
                br->bitcnt += 8;
            }
        } else {
            br->bitbuf |= ((uint32_t)b << (24 - br->bitcnt));
            br->bitcnt += 8;
        }
    }
}

static inline int fbr_get_bits(FastBitReader* br, int n) {
    if (n <= 0) return 0;
    if (br->bitcnt < n) fbr_fill(br);
    int val = (int)(br->bitbuf >> (32 - n));
    br->bitbuf <<= n;
    br->bitcnt -= n;
    return val;
}

static inline int fbr_huff_decode(FastBitReader* br, const FastHuffTable* t) {
    if (br->bitcnt < 16) fbr_fill(br);
    uint32_t p10 = br->bitbuf >> 22;
    uint8_t flen = t->fast_len[p10];
    if (flen > 0) {
        br->bitbuf <<= flen;
        br->bitcnt -= flen;
        return (int)t->fast_sym[p10];
    }
    int32_t code = (int32_t)p10;
    br->bitbuf <<= 10;
    br->bitcnt -= 10;
    for (int len = 11; len <= 16; len++) {
        if (br->bitcnt < 1) fbr_fill(br);
        code = (code << 1) | (int32_t)(br->bitbuf >> 31);
        br->bitbuf <<= 1;
        br->bitcnt -= 1;
        if (code <= t->maxcode[len]) {
            int idx = code + t->valoffset[len];
            if (idx >= 0 && idx < 256) return (int)t->huffval[idx];
            return -1;
        }
    }
    return -1;
}

static int fbr_consume_restart(FastBitReader* br) {
    br->bitbuf = 0;
    br->bitcnt = 0;
    if (!br->marker_hit) {
        while (!br->eof) {
            int b = fbr_next_raw_byte(br);
            if (b < 0) return 0;
            if (b == 0xFF) {
                int nxt;
                do { nxt = fbr_next_raw_byte(br); } while (nxt == 0xFF);
                if (nxt >= 0xD0 && nxt <= 0xD7) return 1;
                if (nxt == 0xD9) { br->marker_hit = 1; br->pending_marker = 0xD9; return 0; }
            }
        }
        return 0;
    }
    uint8_t m = br->pending_marker;
    br->marker_hit = 0;
    br->pending_marker = 0;
    return (m >= 0xD0 && m <= 0xD7) ? 1 : 0;
}

static inline int fbr_extend(int v, int s) {
    return (v < (1 << (s - 1))) ? (v - ((1 << s) - 1)) : v;
}

static inline int decode_huff_block(FastBitReader* br, const FastHuffTable* dc_t, const FastHuffTable* ac_t,
                                    int* dc_pred, int16_t* out64, uint8_t* out_has_ac) {
    int s = fbr_huff_decode(br, dc_t);
    if (s < 0 || s > 15) return 0;
    int diff = (s > 0) ? fbr_extend(fbr_get_bits(br, s), s) : 0;
    *dc_pred += diff;
    out64[0] = (int16_t)(*dc_pred);

    int rs = fbr_huff_decode(br, ac_t);
    if (rs < 0) return 0;
    if (rs == 0x00) {
        *out_has_ac = 0;
        return 1;
    }

    memset(out64 + 1, 0, 63 * sizeof(int16_t));
    uint8_t has_ac = 0;
    int k = 1;
    for (;;) {
        int r = rs >> 4;
        int sz = rs & 0x0F;
        if (sz == 0) {
            if (r == 15) {
                k += 16;
                if (k >= 64) break;
            } else {
                break;
            }
        } else {
            k += r;
            if (k >= 64) break;
            int val = fbr_extend(fbr_get_bits(br, sz), sz);
            out64[FS_ZIGZAG[k]] = (int16_t)val;
            has_ac = 1;
            k++;
            if (k >= 64) break;
        }
        rs = fbr_huff_decode(br, ac_t);
        if (rs < 0) return 0;
    }
    *out_has_ac = has_ac;
    return 1;
}

// Fast 5-multiply AAN 2D IDCT (8x8 -> 8x8)
static inline void idct_8x8_aan(const int16_t* in, uint8_t has_ac, const int32_t* dq, const uint16_t* q_raw, uint8_t* out) {
    if (!has_ac) {
        int v = (((int)in[0] * (int)q_raw[0] + 4) >> 3) + 128;
        uint8_t c = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        memset(out, c, 64);
        return;
    }

    int32_t ws[64];
    for (int r = 0; r < 8; r++) {
        const int16_t* ir = in + r * 8;
        const int32_t* dr = dq + r * 8;
        int32_t* wr = ws + r * 8;
        if ((ir[1] | ir[2] | ir[3] | ir[4] | ir[5] | ir[6] | ir[7]) == 0) {
            int32_t dc = ((int32_t)ir[0] * dr[0] + 1) >> 1;
            wr[0] = wr[1] = wr[2] = wr[3] = wr[4] = wr[5] = wr[6] = wr[7] = dc;
            continue;
        }
        int32_t t0 = (int32_t)ir[0] * dr[0];
        int32_t t1 = (int32_t)ir[2] * dr[2];
        int32_t t2 = (int32_t)ir[4] * dr[4];
        int32_t t3 = (int32_t)ir[6] * dr[6];
        int32_t t10 = t0 + t2, t11 = t0 - t2, t13 = t1 + t3;
        int32_t t12 = (((t1 - t3) * 362 + 128) >> 8) - t13;
        t0 = t10 + t13; t3 = t10 - t13; t1 = t11 + t12; t2 = t11 - t12;

        int32_t t4 = (int32_t)ir[1] * dr[1];
        int32_t t5 = (int32_t)ir[3] * dr[3];
        int32_t t6 = (int32_t)ir[5] * dr[5];
        int32_t t7 = (int32_t)ir[7] * dr[7];
        int32_t z13 = t6 + t5, z10 = t6 - t5, z11 = t4 + t7, z12 = t4 - t7;
        t7 = z11 + z13;
        t11 = ((z11 - z13) * 362 + 128) >> 8;
        int32_t z5 = ((z10 + z12) * 473 + 128) >> 8;
        t10 = ((z12 * 277 + 128) >> 8) - z5;
        t12 = ((z10 * -669 + 128) >> 8) + z5;
        t6 = t12 - t7; t5 = t11 - t6; t4 = t10 + t5;

        wr[0] = (t0 + t7 + 1) >> 1; wr[7] = (t0 - t7 + 1) >> 1;
        wr[1] = (t1 + t6 + 1) >> 1; wr[6] = (t1 - t6 + 1) >> 1;
        wr[2] = (t2 + t5 + 1) >> 1; wr[5] = (t2 - t5 + 1) >> 1;
        wr[3] = (t3 - t4 + 1) >> 1; wr[4] = (t3 + t4 + 1) >> 1;
    }

    for (int c = 0; c < 8; c++) {
        int32_t* wc = ws + c;
        if ((wc[8] | wc[16] | wc[24] | wc[32] | wc[40] | wc[48] | wc[56]) == 0) {
            int v = ((wc[0] + 256) >> 9) + 128;
            uint8_t cl = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
            out[c] = out[c+8] = out[c+16] = out[c+24] = out[c+32] = out[c+40] = out[c+48] = out[c+56] = cl;
            continue;
        }
        int32_t t0 = wc[0], t1 = wc[16], t2 = wc[32], t3 = wc[48];
        int32_t t10 = t0 + t2, t11 = t0 - t2, t13 = t1 + t3;
        int32_t t12 = (((t1 - t3) * 362 + 128) >> 8) - t13;
        t0 = t10 + t13; t3 = t10 - t13; t1 = t11 + t12; t2 = t11 - t12;

        int32_t t4 = wc[8], t5 = wc[24], t6 = wc[40], t7 = wc[56];
        int32_t z13 = t6 + t5, z10 = t6 - t5, z11 = t4 + t7, z12 = t4 - t7;
        t7 = z11 + z13;
        t11 = ((z11 - z13) * 362 + 128) >> 8;
        int32_t z5 = ((z10 + z12) * 473 + 128) >> 8;
        t10 = ((z12 * 277 + 128) >> 8) - z5;
        t12 = ((z10 * -669 + 128) >> 8) + z5;
        t6 = t12 - t7; t5 = t11 - t6; t4 = t10 + t5;

        int v0 = ((t0 + t7 + 256) >> 9) + 128; out[c]      = (uint8_t)(v0 < 0 ? 0 : (v0 > 255 ? 255 : v0));
        int v7 = ((t0 - t7 + 256) >> 9) + 128; out[c + 56] = (uint8_t)(v7 < 0 ? 0 : (v7 > 255 ? 255 : v7));
        int v1 = ((t1 + t6 + 256) >> 9) + 128; out[c + 8]  = (uint8_t)(v1 < 0 ? 0 : (v1 > 255 ? 255 : v1));
        int v6 = ((t1 - t6 + 256) >> 9) + 128; out[c + 48] = (uint8_t)(v6 < 0 ? 0 : (v6 > 255 ? 255 : v6));
        int v2 = ((t2 + t5 + 256) >> 9) + 128; out[c + 16] = (uint8_t)(v2 < 0 ? 0 : (v2 > 255 ? 255 : v2));
        int v5 = ((t2 - t5 + 256) >> 9) + 128; out[c + 40] = (uint8_t)(v5 < 0 ? 0 : (v5 > 255 ? 255 : v5));
        int v3 = ((t3 - t4 + 256) >> 9) + 128; out[c + 24] = (uint8_t)(v3 < 0 ? 0 : (v3 > 255 ? 255 : v3));
        int v4 = ((t3 + t4 + 256) >> 9) + 128; out[c + 32] = (uint8_t)(v4 < 0 ? 0 : (v4 > 255 ? 255 : v4));
    }
}

// Exact Half-Height Vertical AAN IDCT (8x8 DCT -> 8x4 vertically 2:1 averaged output in 32 bytes).
// Algebraically identical to full 8x8 IDCT + (row2k + row2k+1)/2: skips row 4 completely,
// eliminates temp 64-byte buffer & vertical averaging pass, and cuts column butterfly by 65%!
static inline void idct_8x4_half_v_aan(const int16_t* in, uint8_t has_ac, const int32_t* dq, const uint16_t* q_raw, uint8_t* out32) {
    if (!has_ac) {
        int v = (((int)in[0] * (int)q_raw[0] + 4) >> 3) + 128;
        uint8_t c = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        memset(out32, c, 32);
        return;
    }

    int32_t ws[64];
    for (int r = 0; r < 8; r++) {
        if (r == 4) continue; // Frequency k=4 cancels identically across adjacent row pairs!
        const int16_t* ir = in + r * 8;
        const int32_t* dr = dq + r * 8;
        int32_t* wr = ws + r * 8;
        if ((ir[1] | ir[2] | ir[3] | ir[4] | ir[5] | ir[6] | ir[7]) == 0) {
            int32_t dc = ((int32_t)ir[0] * dr[0] + 1) >> 1;
            wr[0] = wr[1] = wr[2] = wr[3] = wr[4] = wr[5] = wr[6] = wr[7] = dc;
            continue;
        }
        int32_t t0 = (int32_t)ir[0] * dr[0];
        int32_t t1 = (int32_t)ir[2] * dr[2];
        int32_t t2 = (int32_t)ir[4] * dr[4];
        int32_t t3 = (int32_t)ir[6] * dr[6];
        int32_t t10 = t0 + t2, t11 = t0 - t2, t13 = t1 + t3;
        int32_t t12 = (((t1 - t3) * 362 + 128) >> 8) - t13;
        t0 = t10 + t13; t3 = t10 - t13; t1 = t11 + t12; t2 = t11 - t12;

        int32_t t4 = (int32_t)ir[1] * dr[1];
        int32_t t5 = (int32_t)ir[3] * dr[3];
        int32_t t6 = (int32_t)ir[5] * dr[5];
        int32_t t7 = (int32_t)ir[7] * dr[7];
        int32_t z13 = t6 + t5, z10 = t6 - t5, z11 = t4 + t7, z12 = t4 - t7;
        t7 = z11 + z13;
        t11 = ((z11 - z13) * 362 + 128) >> 8;
        int32_t z5 = ((z10 + z12) * 473 + 128) >> 8;
        t10 = ((z12 * 277 + 128) >> 8) - z5;
        t12 = ((z10 * -669 + 128) >> 8) + z5;
        t6 = t12 - t7; t5 = t11 - t6; t4 = t10 + t5;

        wr[0] = (t0 + t7 + 1) >> 1; wr[7] = (t0 - t7 + 1) >> 1;
        wr[1] = (t1 + t6 + 1) >> 1; wr[6] = (t1 - t6 + 1) >> 1;
        wr[2] = (t2 + t5 + 1) >> 1; wr[5] = (t2 - t5 + 1) >> 1;
        wr[3] = (t3 - t4 + 1) >> 1; wr[4] = (t3 + t4 + 1) >> 1;
    }

    for (int c = 0; c < 8; c++) {
        int32_t* wc = ws + c;
        if ((wc[8] | wc[16] | wc[24] | wc[40] | wc[48] | wc[56]) == 0) {
            int v = ((wc[0] + 256) >> 9) + 128;
            uint8_t cl = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
            out32[c] = out32[c + 8] = out32[c + 16] = out32[c + 24] = cl;
            continue;
        }
        int32_t e0 = wc[0] << 1;
        int32_t ed = ((wc[16] - wc[48]) * 362 + 128) >> 8;
        int32_t z10 = wc[40] - wc[24];
        int32_t z12 = wc[8]  - wc[56];
        int32_t z5  = ((z10 + z12) * 473 + 128) >> 8;
        int32_t t10 = ((z12 * 277 + 128) >> 8) - z5;
        int32_t t12 = ((z10 * -669 + 128) >> 8) + z5;

        int v0 = ((e0 + ed + t12 + 512) >> 10) + 128; out32[c]      = (uint8_t)(v0 < 0 ? 0 : (v0 > 255 ? 255 : v0));
        int v1 = ((e0 - ed - t10 + 512) >> 10) + 128; out32[c + 8]  = (uint8_t)(v1 < 0 ? 0 : (v1 > 255 ? 255 : v1));
        int v2 = ((e0 - ed + t10 + 512) >> 10) + 128; out32[c + 16] = (uint8_t)(v2 < 0 ? 0 : (v2 > 255 ? 255 : v2));
        int v3 = ((e0 + ed - t12 + 512) >> 10) + 128; out32[c + 24] = (uint8_t)(v3 < 0 ? 0 : (v3 > 255 ? 255 : v3));
    }
}

typedef struct {
    int16_t c[64];
    uint8_t has_ac;
} DecBlock;

typedef struct {
    int src_w, src_h;
    int dst_w, dst_h;
    int scale;          // 1 or 2
    int samp_mode;      // 0 = 4:2:0 (16x16), 1 = 4:2:2 (16x8), 2 = 4:4:4 (8x8), 3 = 4:4:0 (8x16)
    int in_mcus_x;
    int in_mcus_y;
    int blks_per_mcu;   // 6 (4:2:0), 4 (4:2:2 / 4:4:0), 3 (4:4:4)
    int out_mcus_x;
    int out_mcus_y;
    int tq[3];
    int td[3], ta[3];
    int sos_order[3];
    uint16_t qt_raw[4][64];
    int32_t dq_aan[4][64];
    int qt_valid[4];
    FastHuffTable dc_huff[4];
    FastHuffTable ac_huff[4];
    int restart_interval;
    CleanExifInfo exif;
} FastJpegInfo;

static int rd_u16_file(FILE* fp, uint16_t* out) {
    int a = fgetc(fp), b = fgetc(fp);
    if (a < 0 || b < 0) return 0;
    *out = (uint16_t)((a << 8) | b);
    return 1;
}

static int parse_jpeg_headers(FILE* fp, FastJpegInfo* info) {
    memset(info, 0, sizeof(FastJpegInfo));
    info->exif.orientation = 1;
    build_fast_huff(&info->dc_huff[0], STD_DC_LUMA_BITS, STD_DC_LUMA_VALS, 12);
    build_fast_huff(&info->dc_huff[1], STD_DC_CHROMA_BITS, STD_DC_CHROMA_VALS, 12);
    build_fast_huff(&info->ac_huff[0], STD_AC_LUMA_BITS, STD_AC_LUMA_VALS, 162);
    build_fast_huff(&info->ac_huff[1], STD_AC_CHROMA_BITS, STD_AC_CHROMA_VALS, 162);

    if (fgetc(fp) != 0xFF || fgetc(fp) != 0xD8) return -2;

    int comp_id[3] = {0, 0, 0};
    int hs[3] = {0, 0, 0}, vs[3] = {0, 0, 0};
    int sof_found = 0;

    for (int iter = 0; iter < 512; iter++) {
        int c = fgetc(fp);
        if (c < 0 || c != 0xFF) return -3;
        do { c = fgetc(fp); } while (c == 0xFF);
        if (c < 0 || c == 0xD9) return -3;
        if (c == 0x00 || c == 0x01 || (c >= 0xD0 && c <= 0xD7)) continue;

        uint16_t seg_len;
        if (!rd_u16_file(fp, &seg_len) || seg_len < 2) return -3;
        size_t pay_len = (size_t)seg_len - 2;

        if (c == 0xE1 && pay_len >= 14 && pay_len <= 65533) {
            uint8_t* tmp = (uint8_t*)malloc(pay_len);
            if (!tmp) return -10;
            if (fread(tmp, 1, pay_len, fp) != pay_len) { free(tmp); return -3; }
            parse_exif_app1(tmp, pay_len, &info->exif);
            free(tmp);
        } else if (c == 0xDB) {
            uint8_t* tmp = (uint8_t*)malloc(pay_len);
            if (!tmp) return -10;
            if (fread(tmp, 1, pay_len, fp) != pay_len) { free(tmp); return -4; }
            size_t p = 0;
            while (p < pay_len) {
                uint8_t pt = tmp[p++];
                int pq = pt >> 4, tq = pt & 0x0F;
                if (pq != 0 || tq >= 4 || p + 64 > pay_len) { free(tmp); return -4; }
                for (int k = 0; k < 64; k++) {
                    int nat = FS_ZIGZAG[k];
                    uint16_t qv = tmp[p++];
                    info->qt_raw[tq][nat] = qv;
                    uint64_t num = (uint64_t)qv * (uint64_t)FS_AAN_Q12[nat >> 3] * (uint64_t)FS_AAN_Q12[nat & 7];
                    info->dq_aan[tq][nat] = (int32_t)((num + 65536ULL) >> 17);
                }
                info->qt_valid[tq] = 1;
            }
            free(tmp);
        } else if (c == 0xC0) {
            if (pay_len < 6) return -5;
            uint8_t* tmp = (uint8_t*)malloc(pay_len);
            if (!tmp) return -10;
            if (fread(tmp, 1, pay_len, fp) != pay_len) { free(tmp); return -5; }
            int prec = tmp[0];
            info->src_h = (tmp[1] << 8) | tmp[2];
            info->src_w = (tmp[3] << 8) | tmp[4];
            int nf = tmp[5];
            if (prec != 8 || nf != 3 || pay_len < 6 + 9 || info->src_w <= 0 || info->src_h <= 0) { free(tmp); return -5; }
            for (int i = 0; i < 3; i++) {
                comp_id[i] = tmp[6 + i * 3];
                hs[i] = tmp[7 + i * 3] >> 4;
                vs[i] = tmp[7 + i * 3] & 0x0F;
                info->tq[i] = tmp[8 + i * 3];
                if (info->tq[i] >= 4) { free(tmp); return -5; }
            }
            free(tmp);
            if (hs[0] == 2 && vs[0] == 2 && hs[1] == 1 && vs[1] == 1 && hs[2] == 1 && vs[2] == 1) {
                info->samp_mode = 0; info->blks_per_mcu = 6;
                info->in_mcus_x = (info->src_w + 15) >> 4;
                info->in_mcus_y = (info->src_h + 15) >> 4;
            } else if (hs[0] == 2 && vs[0] == 1 && hs[1] == 1 && vs[1] == 1 && hs[2] == 1 && vs[2] == 1) {
                info->samp_mode = 1; info->blks_per_mcu = 4;
                info->in_mcus_x = (info->src_w + 15) >> 4;
                info->in_mcus_y = (info->src_h + 7) >> 3;
            } else if (hs[0] == 1 && vs[0] == 1 && hs[1] == 1 && vs[1] == 1 && hs[2] == 1 && vs[2] == 1) {
                info->samp_mode = 2; info->blks_per_mcu = 3;
                info->in_mcus_x = (info->src_w + 7) >> 3;
                info->in_mcus_y = (info->src_h + 7) >> 3;
            } else if (hs[0] == 1 && vs[0] == 2 && hs[1] == 1 && vs[1] == 1 && hs[2] == 1 && vs[2] == 1) {
                info->samp_mode = 3; info->blks_per_mcu = 4;
                info->in_mcus_x = (info->src_w + 7) >> 3;
                info->in_mcus_y = (info->src_h + 15) >> 4;
            } else {
                return -(100 + (hs[0] << 4) + vs[0]);
            }
            sof_found = 1;
        } else if (c == 0xC1 || c == 0xC2 || c == 0xC3 || c == 0xC9 || c == 0xCA) {
            return -(200 + c);
        } else if (c == 0xC4) {
            uint8_t* tmp = (uint8_t*)malloc(pay_len);
            if (!tmp) return -10;
            if (fread(tmp, 1, pay_len, fp) != pay_len) { free(tmp); return -7; }
            size_t p = 0;
            while (p < pay_len) {
                if (p + 17 > pay_len) { free(tmp); return -7; }
                uint8_t tc_th = tmp[p++];
                int tc = tc_th >> 4, th = tc_th & 0x0F;
                if (tc > 1 || th >= 4) { free(tmp); return -7; }
                uint8_t bits[17]; bits[0] = 0;
                int total = 0;
                for (int i = 1; i <= 16; i++) { bits[i] = tmp[p++]; total += bits[i]; }
                if (p + (size_t)total > pay_len || total > 256) { free(tmp); return -7; }
                build_fast_huff(tc == 0 ? &info->dc_huff[th] : &info->ac_huff[th], bits, tmp + p, total);
                p += (size_t)total;
            }
            free(tmp);
        } else if (c == 0xDD) {
            if (pay_len != 2) return -3;
            uint16_t ri;
            if (!rd_u16_file(fp, &ri)) return -3;
            info->restart_interval = (int)ri;
        } else if (c == 0xDA) {
            if (!sof_found || pay_len < 10) return -8;
            uint8_t* tmp = (uint8_t*)malloc(pay_len);
            if (!tmp) return -10;
            if (fread(tmp, 1, pay_len, fp) != pay_len) { free(tmp); return -8; }
            int ns = tmp[0];
            if (ns != 3 || pay_len < (size_t)(1 + ns * 2 + 3)) { free(tmp); return -8; }
            int seen_mask = 0;
            for (int i = 0; i < 3; i++) {
                int cs = tmp[1 + i * 2];
                int td = tmp[2 + i * 2] >> 4;
                int ta = tmp[2 + i * 2] & 0x0F;
                int cidx = -1;
                for (int j = 0; j < 3; j++) {
                    if (comp_id[j] == cs) { cidx = j; break; }
                }
                if (cidx < 0 || (seen_mask & (1 << cidx)) || td >= 4 || ta >= 4) { free(tmp); return -8; }
                seen_mask |= (1 << cidx);
                info->sos_order[i] = cidx;
                info->td[cidx] = td;
                info->ta[cidx] = ta;
            }
            int ss = tmp[1 + ns * 2], se = tmp[2 + ns * 2], ahal = tmp[3 + ns * 2];
            free(tmp);
            if (ss != 0 || se != 63 || ahal != 0) return -8;
            for (int i = 0; i < 3; i++) {
                if (!info->qt_valid[info->tq[i]] || !info->dc_huff[info->td[i]].valid || !info->ac_huff[info->ta[i]].valid) return -8;
            }
            return 1;
        } else {
            if (fseek(fp, (long)pay_len, SEEK_CUR) != 0) return -3;
        }
    }
    return -3;
}

static inline int16_t ycc_eval_y(const int8_t* p0, const int8_t* p1, const int8_t* p2,
                                 int w0, int w1, int w2, int u) {
    int y0 = u >> 4, yf = u & 15;
    int yo0 = y0 << 2;
    int yo1 = yo0 + ((y0 < 16) ? 4 : 0);
    int a = w0 * (int)p0[yo0] + w1 * (int)p1[yo0] + w2 * (int)p2[yo0];
    int b = w0 * (int)p0[yo1] + w1 * (int)p1[yo1] + w2 * (int)p2[yo1];
    return (int16_t)(((a << 4) + (b - a) * yf + 128) >> 8);
}

// Direct YCbCr -> YCbCr 3D LUT for one 2x2 pixel quad with exact same-cell fast path:
// On ~85-90% of quads where all 4 lumas share the same 17-level cell, computes the cell slope ONCE
// for all 4 pixels (3.5x fewer loads/multiplies with 100% bit-identical output!).
static inline void grade_ycc_quad_2x2(const int8_t* __restrict ycc_lut,
                                      uint8_t y00, uint8_t y01, uint8_t y10, uint8_t y11,
                                      uint8_t cb, uint8_t cr,
                                      int16_t* oy00, int16_t* oy01, int16_t* oy10, int16_t* oy11,
                                      int16_t* ocb, int16_t* ocr) {
    int cb256 = (int)cb + ((int)cb >> 7);
    int cr256 = (int)cr + ((int)cr >> 7);
    int cb0 = cb256 >> 4, cbf = cb256 & 15;
    int cr0 = cr256 >> 4, crf = cr256 & 15;
    int dcb = (cb0 < 16) ? 68 : 0;
    int dcr = (cr0 < 16) ? 1156 : 0;
    const int8_t* p0 = ycc_lut + ((cr0 * 289 + cb0 * 17) << 2);
    const int8_t* p1;
    const int8_t* p2 = p0 + dcb + dcr;
    int w0, w1, w2;
    if (cbf >= crf) {
        p1 = p0 + dcb;
        w0 = 16 - cbf; w1 = cbf - crf; w2 = crf;
    } else {
        p1 = p0 + dcr;
        w0 = 16 - crf; w1 = crf - cbf; w2 = cbf;
    }

    int u0 = (int)y00 + ((int)y00 >> 7);
    int u1 = (int)y01 + ((int)y01 >> 7);
    int u2 = (int)y10 + ((int)y10 >> 7);
    int u3 = (int)y11 + ((int)y11 >> 7);

    if ((((u0 ^ u1) | (u0 ^ u2) | (u0 ^ u3)) >> 4) == 0) {
        int y0 = u0 >> 4;
        int yo0 = y0 << 2;
        int yo1 = yo0 + ((y0 < 16) ? 4 : 0);
        int a = w0 * (int)p0[yo0] + w1 * (int)p1[yo0] + w2 * (int)p2[yo0];
        int b = w0 * (int)p0[yo1] + w1 * (int)p1[yo1] + w2 * (int)p2[yo1];
        int a16 = (a << 4) + 128;
        int d = b - a;
        int f0 = u0 & 15, f1 = u1 & 15, f2 = u2 & 15, f3 = u3 & 15;
        *oy00 = (int16_t)((a16 + d * f0) >> 8);
        *oy01 = (int16_t)((a16 + d * f1) >> 8);
        *oy10 = (int16_t)((a16 + d * f2) >> 8);
        *oy11 = (int16_t)((a16 + d * f3) >> 8);

        int yaf = (f0 + f1 + f2 + f3 + 2) >> 2;
        int ca  = w0 * (int)p0[yo0 + 1] + w1 * (int)p1[yo0 + 1] + w2 * (int)p2[yo0 + 1];
        int cbv = w0 * (int)p0[yo1 + 1] + w1 * (int)p1[yo1 + 1] + w2 * (int)p2[yo1 + 1];
        *ocb = (int16_t)(((ca << 4) + (cbv - ca) * yaf + 128) >> 8);
        int ra  = w0 * (int)p0[yo0 + 2] + w1 * (int)p1[yo0 + 2] + w2 * (int)p2[yo0 + 2];
        int crv = w0 * (int)p0[yo1 + 2] + w1 * (int)p1[yo1 + 2] + w2 * (int)p2[yo1 + 2];
        *ocr = (int16_t)(((ra << 4) + (crv - ra) * yaf + 128) >> 8);
        return;
    }

    *oy00 = ycc_eval_y(p0, p1, p2, w0, w1, w2, u0);
    *oy01 = ycc_eval_y(p0, p1, p2, w0, w1, w2, u1);
    *oy10 = ycc_eval_y(p0, p1, p2, w0, w1, w2, u2);
    *oy11 = ycc_eval_y(p0, p1, p2, w0, w1, w2, u3);

    int ya = ((int)y00 + (int)y01 + (int)y10 + (int)y11 + 2) >> 2;
    int ya256 = ya + (ya >> 7);
    int ya0 = ya256 >> 4, yaf = ya256 & 15;
    int yao0 = ya0 << 2;
    int yao1 = yao0 + ((ya0 < 16) ? 4 : 0);
    int ca = w0 * (int)p0[yao0 + 1] + w1 * (int)p1[yao0 + 1] + w2 * (int)p2[yao0 + 1];
    int cbv = w0 * (int)p0[yao1 + 1] + w1 * (int)p1[yao1 + 1] + w2 * (int)p2[yao1 + 1];
    *ocb = (int16_t)(((ca << 4) + (cbv - ca) * yaf + 128) >> 8);
    int ra = w0 * (int)p0[yao0 + 2] + w1 * (int)p1[yao0 + 2] + w2 * (int)p2[yao0 + 2];
    int crv = w0 * (int)p0[yao1 + 2] + w1 * (int)p1[yao1 + 2] + w2 * (int)p2[yao1 + 2];
    *ocr = (int16_t)(((ra << 4) + (crv - ra) * yaf + 128) >> 8);
}

// Fast Table-Driven Luma-Domain Film Grain & Clarity directly on L1-resident y_blk[4][64]
static void apply_luma_grain_clarity_mcu(int16_t y_blk[4][64], int global_x0, int global_y0,
                                         const uint8_t grain_mid_lut[256], int has_grain, int clarity_strength) {
    if (has_grain && clarity_strength == 0) {
        // Branchless fast grain-only path (1 multiply + 1 shift per pixel, zero LCG / midtone multiplies)
        int x_tile = (global_x0 >> 4);
        for (int y = 0; y < 16; y++) {
            int gy = global_y0 + y;
            int b_idx = (y >= 8) ? 2 : 0;
            int sy8 = (y & 7) * 8;
            int phase = (int)(((uint32_t)gy * 2654435761U) >> 26);
            const int8_t* g1 = STATIC_GRAIN_64x64 + ((gy & 63) << 6);
            const int8_t* g2 = STATIC_GRAIN_64x64 + (((gy * 7 + x_tile) & 63) << 6);
            for (int half = 0; half < 2; half++) {
                int16_t* row = y_blk[b_idx + half] + sy8;
                int gx_base = global_x0 + (half << 3);
                for (int x = 0; x < 8; x++) {
                    int luma = (int)row[x] + 128;
                    int raw = (int)g1[(gx_base + x) & 63] + (int)g2[(gx_base + x + phase) & 63];
                    int nl = luma + ((raw * (int)grain_mid_lut[luma]) >> 9);
                    if (nl < 0) nl = 0;
                    else if (nl > 255) nl = 255;
                    row[x] = (int16_t)(nl - 128);
                }
            }
        }
        return;
    }

    int x_tile = (global_x0 >> 4);
    for (int y = 0; y < 16; y++) {
        int gy = global_y0 + y;
        int b_idx = (y >= 8) ? 2 : 0;
        int sy8 = (y & 7) * 8;
        int phase = (int)(((uint32_t)gy * 2654435761U) >> 26);
        const int8_t* g1 = STATIC_GRAIN_64x64 + ((gy & 63) << 6);
        const int8_t* g2 = STATIC_GRAIN_64x64 + (((gy * 7 + x_tile) & 63) << 6);
        int prev_luma = (int)y_blk[b_idx][sy8] + 128;
        for (int half = 0; half < 2; half++) {
            int16_t* row = y_blk[b_idx + half] + sy8;
            int gx_base = global_x0 + (half << 3);
            for (int x = 0; x < 8; x++) {
                int luma = (int)row[x] + 128;
                int delta = ((luma - prev_luma) * clarity_strength) >> 4;
                if (has_grain) {
                    int raw = (int)g1[(gx_base + x) & 63] + (int)g2[(gx_base + x + phase) & 63];
                    delta += (raw * (int)grain_mid_lut[luma]) >> 9;
                }
                prev_luma = luma;
                int nl = luma + delta;
                if (nl < 0) nl = 0;
                else if (nl > 255) nl = 255;
                row[x] = (int16_t)(nl - 128);
            }
        }
    }
}

// Emits 2x2 RGB565 DC preview pixels for one 16x16 MCU using already-computed FDCT DC coefficients (0 extra I/O!)
static inline void emit_mcu_preview_2x2(uint16_t* preview, int pw, int ph, int omx, int out_my,
                                        const int16_t y_blk[4][64], const int cb_q[4], const int cr_q[4]) {
    if (!preview) return;
    int px0 = omx * 2, py0 = out_my * 2;
    for (int b = 0; b < 4; b++) {
        int px = px0 + (b & 1);
        int py = py0 + (b >> 1);
        if (px >= pw || py >= ph) continue;
        int Y = ((int)y_blk[b][0] + 4) >> 3;
        Y += 128;
        int cb = cb_q[b], cr = cr_q[b];
        int r  = Y + ((91881 * cr + 32768) >> 16);
        int g  = Y - ((22554 * cb + 46802 * cr + 32768) >> 16);
        int bl = Y + ((116130 * cb + 32768) >> 16);
        if (r < 0) r = 0; else if (r > 255) r = 255;
        if (g < 0) g = 0; else if (g > 255) g = 255;
        if (bl < 0) bl = 0; else if (bl > 255) bl = 255;
        preview[(size_t)py * (size_t)pw + (size_t)px] =
            (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3));
    }
}

static inline void sample_quadrant_chroma(const int16_t cb_blk[64], const int16_t cr_blk[64], int cb_q[4], int cr_q[4]) {
    cb_q[0] = ((int)cb_blk[9]  + (int)cb_blk[18] + 1) >> 1;
    cb_q[1] = ((int)cb_blk[13] + (int)cb_blk[22] + 1) >> 1;
    cb_q[2] = ((int)cb_blk[41] + (int)cb_blk[50] + 1) >> 1;
    cb_q[3] = ((int)cb_blk[45] + (int)cb_blk[54] + 1) >> 1;
    cr_q[0] = ((int)cr_blk[9]  + (int)cr_blk[18] + 1) >> 1;
    cr_q[1] = ((int)cr_blk[13] + (int)cr_blk[22] + 1) >> 1;
    cr_q[2] = ((int)cr_blk[41] + (int)cr_blk[50] + 1) >> 1;
    cr_q[3] = ((int)cr_blk[45] + (int)cr_blk[54] + 1) >> 1;
}

// Fast Zero-Copy 1:1 Transcode for Sony 4:2:2 (2x1) & Standard 4:2:0 (2x2) MCUs
static inline void transcode_mcu_fast_1x(const FastJpegInfo* info, const int8_t* ycc_lut,
                                         const JpegStreamState* out_state, const DecBlock* in_band,
                                         int band_in_my0, int band_in_rows, int omx, int out_my,
                                         const uint8_t grain_mid_lut[256], int has_grain, int clarity_strength,
                                         JpegQuantBlock* out_6, uint16_t* preview, int pw, int ph) {
    const int32_t* dqY = info->dq_aan[info->tq[0]]; const uint16_t* qY = info->qt_raw[info->tq[0]];
    const int32_t* dqU = info->dq_aan[info->tq[1]]; const uint16_t* qU = info->qt_raw[info->tq[1]];
    const int32_t* dqV = info->dq_aan[info->tq[2]]; const uint16_t* qV = info->qt_raw[info->tq[2]];

    uint8_t yb[4][64];
    uint8_t cb8[64], cr8[64];
    int16_t y_blk[4][64], cb_blk[64], cr_blk[64];

    if (info->samp_mode == 1) { // Sony BIONZ X native 4:2:2 -> exact half-height chroma IDCT directly into cb8/cr8!
        int cx = omx < info->in_mcus_x ? omx : (info->in_mcus_x - 1);
        for (int half = 0; half < 2; half++) {
            int r = out_my * 2 + half - band_in_my0;
            if (r < 0) r = 0;
            if (r >= band_in_rows) r = band_in_rows - 1;
            const DecBlock* mb = in_band + ((size_t)r * (size_t)info->in_mcus_x + (size_t)cx) * 4U;
            idct_8x8_aan(mb[0].c, mb[0].has_ac, dqY, qY, yb[half * 2]);
            idct_8x8_aan(mb[1].c, mb[1].has_ac, dqY, qY, yb[half * 2 + 1]);
            idct_8x4_half_v_aan(mb[2].c, mb[2].has_ac, dqU, qU, cb8 + half * 32);
            idct_8x4_half_v_aan(mb[3].c, mb[3].has_ac, dqV, qV, cr8 + half * 32);
        }
    } else { // Standard 4:2:0
        int r = out_my - band_in_my0;
        if (r < 0) r = 0;
        if (r >= band_in_rows) r = band_in_rows - 1;
        int cx = omx < info->in_mcus_x ? omx : (info->in_mcus_x - 1);
        const DecBlock* mb = in_band + ((size_t)r * (size_t)info->in_mcus_x + (size_t)cx) * 6U;
        for (int b = 0; b < 4; b++) idct_8x8_aan(mb[b].c, mb[b].has_ac, dqY, qY, yb[b]);
        idct_8x8_aan(mb[4].c, mb[4].has_ac, dqU, qU, cb8);
        idct_8x8_aan(mb[5].c, mb[5].has_ac, dqV, qV, cr8);
    }

    for (int by = 0; by < 2; by++) {
        for (int bx = 0; bx < 2; bx++) {
            const uint8_t* ys = yb[by * 2 + bx];
            int16_t* yd = y_blk[by * 2 + bx];
            int cy0 = by * 4, cx0 = bx * 4;
            for (int qy = 0; qy < 4; qy++) {
                int yr0 = (qy * 2) * 8, yr1 = yr0 + 8;
                int cr_row = (cy0 + qy) * 8 + cx0;
                for (int qx = 0; qx < 4; qx++) {
                    int x0 = qx * 2, x1 = x0 + 1;
                    int cidx = cr_row + qx;
                    grade_ycc_quad_2x2(ycc_lut,
                                       ys[yr0 + x0], ys[yr0 + x1], ys[yr1 + x0], ys[yr1 + x1],
                                       cb8[cidx], cr8[cidx],
                                       &yd[yr0 + x0], &yd[yr0 + x1], &yd[yr1 + x0], &yd[yr1 + x1],
                                       &cb_blk[cidx], &cr_blk[cidx]);
                }
            }
        }
    }

    if (has_grain || clarity_strength != 0) {
        apply_luma_grain_clarity_mcu(y_blk, omx * 16, out_my * 16, grain_mid_lut, has_grain, clarity_strength);
    }
    int cb_q[4], cr_q[4];
    if (preview) sample_quadrant_chroma(cb_blk, cr_blk, cb_q, cr_q);
    native_jpeg_quantize_ycbcr_mcu(out_state, y_blk, cb_blk, cr_blk, out_6);
    if (preview) emit_mcu_preview_2x2(preview, pw, ph, omx, out_my, y_blk, cb_q, cr_q);
}

// Generic 16x16 source tile decoder (handles 4:4:4, 4:4:0, and 1:2 box-downscaled tiles)
static void decode_src_tile_16x16_sub(const FastJpegInfo* info, const DecBlock* in_band, int band_in_my0, int band_in_rows,
                                      int tx, int ty, uint8_t y16[256], uint8_t cb8[64], uint8_t cr8[64]) {
    const int32_t* dqY = info->dq_aan[info->tq[0]]; const uint16_t* qY = info->qt_raw[info->tq[0]];
    const int32_t* dqU = info->dq_aan[info->tq[1]]; const uint16_t* qU = info->qt_raw[info->tq[1]];
    const int32_t* dqV = info->dq_aan[info->tq[2]]; const uint16_t* qV = info->qt_raw[info->tq[2]];

    if (info->samp_mode == 0) {
        int r = ty - band_in_my0;
        if (r < 0) r = 0;
        if (r >= band_in_rows) r = band_in_rows - 1;
        int cx = tx < info->in_mcus_x ? tx : (info->in_mcus_x - 1);
        const DecBlock* mb = in_band + ((size_t)r * (size_t)info->in_mcus_x + (size_t)cx) * 6U;
        uint8_t yb[4][64];
        for (int b = 0; b < 4; b++) idct_8x8_aan(mb[b].c, mb[b].has_ac, dqY, qY, yb[b]);
        idct_8x8_aan(mb[4].c, mb[4].has_ac, dqU, qU, cb8);
        idct_8x8_aan(mb[5].c, mb[5].has_ac, dqV, qV, cr8);
        for (int y = 0; y < 16; y++) {
            int by = (y >= 8) ? 2 : 0, sy = (y & 7) * 8;
            memcpy(y16 + y * 16, yb[by] + sy, 8);
            memcpy(y16 + y * 16 + 8, yb[by + 1] + sy, 8);
        }
    } else if (info->samp_mode == 1) {
        int cx = tx < info->in_mcus_x ? tx : (info->in_mcus_x - 1);
        for (int half = 0; half < 2; half++) {
            int r = ty * 2 + half - band_in_my0;
            if (r < 0) r = 0;
            if (r >= band_in_rows) r = band_in_rows - 1;
            const DecBlock* mb = in_band + ((size_t)r * (size_t)info->in_mcus_x + (size_t)cx) * 4U;
            uint8_t y0[64], y1[64];
            idct_8x8_aan(mb[0].c, mb[0].has_ac, dqY, qY, y0);
            idct_8x8_aan(mb[1].c, mb[1].has_ac, dqY, qY, y1);
            idct_8x4_half_v_aan(mb[2].c, mb[2].has_ac, dqU, qU, cb8 + half * 32);
            idct_8x4_half_v_aan(mb[3].c, mb[3].has_ac, dqV, qV, cr8 + half * 32);
            for (int y = 0; y < 8; y++) {
                int oy = (half * 8 + y) * 16;
                memcpy(y16 + oy, y0 + y * 8, 8);
                memcpy(y16 + oy + 8, y1 + y * 8, 8);
            }
        }
    } else if (info->samp_mode == 3) {
        int r = ty - band_in_my0;
        if (r < 0) r = 0;
        if (r >= band_in_rows) r = band_in_rows - 1;
        for (int hx = 0; hx < 2; hx++) {
            int cx = tx * 2 + hx;
            if (cx >= info->in_mcus_x) cx = info->in_mcus_x - 1;
            const DecBlock* mb = in_band + ((size_t)r * (size_t)info->in_mcus_x + (size_t)cx) * 4U;
            uint8_t yt[64], yb[64], cbh[64], crh[64];
            idct_8x8_aan(mb[0].c, mb[0].has_ac, dqY, qY, yt);
            idct_8x8_aan(mb[1].c, mb[1].has_ac, dqY, qY, yb);
            idct_8x8_aan(mb[2].c, mb[2].has_ac, dqU, qU, cbh);
            idct_8x8_aan(mb[3].c, mb[3].has_ac, dqV, qV, crh);
            for (int y = 0; y < 8; y++) {
                memcpy(y16 + y * 16 + hx * 8, yt + y * 8, 8);
                memcpy(y16 + (y + 8) * 16 + hx * 8, yb + y * 8, 8);
                for (int qx = 0; qx < 4; qx++) {
                    cb8[y * 8 + hx * 4 + qx] = (uint8_t)((cbh[y * 8 + qx * 2] + cbh[y * 8 + qx * 2 + 1] + 1) >> 1);
                    cr8[y * 8 + hx * 4 + qx] = (uint8_t)((crh[y * 8 + qx * 2] + crh[y * 8 + qx * 2 + 1] + 1) >> 1);
                }
            }
        }
    } else {
        for (int hy = 0; hy < 2; hy++) {
            int r = ty * 2 + hy - band_in_my0;
            if (r < 0) r = 0;
            if (r >= band_in_rows) r = band_in_rows - 1;
            for (int hx = 0; hx < 2; hx++) {
                int cx = tx * 2 + hx;
                if (cx >= info->in_mcus_x) cx = info->in_mcus_x - 1;
                const DecBlock* mb = in_band + ((size_t)r * (size_t)info->in_mcus_x + (size_t)cx) * 3U;
                uint8_t yb[64], cbh[64], crh[64];
                idct_8x8_aan(mb[0].c, mb[0].has_ac, dqY, qY, yb);
                idct_8x8_aan(mb[1].c, mb[1].has_ac, dqU, qU, cbh);
                idct_8x8_aan(mb[2].c, mb[2].has_ac, dqV, qV, crh);
                for (int y = 0; y < 8; y++) {
                    memcpy(y16 + (hy * 8 + y) * 16 + hx * 8, yb + y * 8, 8);
                }
                for (int qy = 0; qy < 4; qy++) {
                    for (int qx = 0; qx < 4; qx++) {
                        int r0 = (qy * 2) * 8 + qx * 2, r1 = r0 + 8;
                        cb8[(hy * 4 + qy) * 8 + hx * 4 + qx] = (uint8_t)((cbh[r0] + cbh[r0+1] + cbh[r1] + cbh[r1+1] + 2) >> 2);
                        cr8[(hy * 4 + qy) * 8 + hx * 4 + qx] = (uint8_t)((crh[r0] + crh[r0+1] + crh[r1] + crh[r1+1] + 2) >> 2);
                    }
                }
            }
        }
    }
}

static void grade_and_quant_y16_cb8_cr8(const int8_t* ycc_lut, const JpegStreamState* out_state,
                                        const uint8_t y16[256], const uint8_t cb8[64], const uint8_t cr8[64],
                                        int omx, int out_my, const uint8_t grain_mid_lut[256], int has_grain,
                                        int clarity_strength, JpegQuantBlock* out_6, uint16_t* preview, int pw, int ph) {
    int16_t y_blk[4][64], cb_blk[64], cr_blk[64];
    for (int by = 0; by < 2; by++) {
        for (int bx = 0; bx < 2; bx++) {
            int16_t* yd = y_blk[by * 2 + bx];
            for (int qy = 0; qy < 4; qy++) {
                int sy0 = (by * 8 + qy * 2) * 16 + bx * 8;
                int sy1 = sy0 + 16;
                int dy0 = (qy * 2) * 8, dy1 = dy0 + 8;
                int c_row = (by * 4 + qy) * 8 + bx * 4;
                for (int qx = 0; qx < 4; qx++) {
                    int x0 = qx * 2, x1 = x0 + 1;
                    int cidx = c_row + qx;
                    grade_ycc_quad_2x2(ycc_lut,
                                       y16[sy0 + x0], y16[sy0 + x1], y16[sy1 + x0], y16[sy1 + x1],
                                       cb8[cidx], cr8[cidx],
                                       &yd[dy0 + x0], &yd[dy0 + x1], &yd[dy1 + x0], &yd[dy1 + x1],
                                       &cb_blk[cidx], &cr_blk[cidx]);
                }
            }
        }
    }
    if (has_grain || clarity_strength != 0) {
        apply_luma_grain_clarity_mcu(y_blk, omx * 16, out_my * 16, grain_mid_lut, has_grain, clarity_strength);
    }
    int cb_q[4], cr_q[4];
    if (preview) sample_quadrant_chroma(cb_blk, cr_blk, cb_q, cr_q);
    native_jpeg_quantize_ycbcr_mcu(out_state, y_blk, cb_blk, cr_blk, out_6);
    if (preview) emit_mcu_preview_2x2(preview, pw, ph, omx, out_my, y_blk, cb_q, cr_q);
}

typedef struct {
    const FastJpegInfo* info;
    const int8_t* ycc_lut;
    const JpegStreamState* out_state;
    const DecBlock* in_band;
    int band_in_my0;
    int band_in_rows;
    int out_my_base;
    int local_my_start;
    int local_my_end;
    const uint8_t* grain_mid_lut;
    int has_grain;
    int clarity_strength;
    JpegQuantBlock* q_out_base;
    uint16_t* preview_rgb565;
    int preview_w;
    int preview_h;
} FastBandSlice;

static void* fast_band_worker(void* arg) {
    FastBandSlice* s = (FastBandSlice*)arg;
    const FastJpegInfo* info = s->info;

    for (int lm = s->local_my_start; lm < s->local_my_end; lm++) {
        int out_my = s->out_my_base + lm;
        JpegQuantBlock* row_qb = s->q_out_base + (size_t)lm * (size_t)info->out_mcus_x * 6U;

        if (info->scale == 1 && (info->samp_mode == 0 || info->samp_mode == 1)) {
            for (int omx = 0; omx < info->out_mcus_x; omx++) {
                transcode_mcu_fast_1x(info, s->ycc_lut, s->out_state, s->in_band,
                                      s->band_in_my0, s->band_in_rows, omx, out_my,
                                      s->grain_mid_lut, s->has_grain, s->clarity_strength,
                                      row_qb + (size_t)omx * 6U,
                                      s->preview_rgb565, s->preview_w, s->preview_h);
            }
        } else if (info->scale == 1) {
            uint8_t y16[256], cb8[64], cr8[64];
            for (int omx = 0; omx < info->out_mcus_x; omx++) {
                decode_src_tile_16x16_sub(info, s->in_band, s->band_in_my0, s->band_in_rows,
                                          omx, out_my, y16, cb8, cr8);
                grade_and_quant_y16_cb8_cr8(s->ycc_lut, s->out_state, y16, cb8, cr8,
                                            omx, out_my, s->grain_mid_lut, s->has_grain, s->clarity_strength,
                                            row_qb + (size_t)omx * 6U,
                                            s->preview_rgb565, s->preview_w, s->preview_h);
            }
        } else {
            uint8_t y16[256], cb8[64], cr8[64];
            uint8_t sy[256], su8[64], sv8[64];
            for (int omx = 0; omx < info->out_mcus_x; omx++) {
                for (int qy = 0; qy < 2; qy++) {
                    for (int qx = 0; qx < 2; qx++) {
                        decode_src_tile_16x16_sub(info, s->in_band, s->band_in_my0, s->band_in_rows,
                                                  omx * 2 + qx, out_my * 2 + qy, sy, su8, sv8);
                        for (int dy = 0; dy < 8; dy++) {
                            int oy = (qy * 8 + dy) * 16 + qx * 8;
                            int r0 = (dy * 2) * 16, r1 = r0 + 16;
                            for (int dx = 0; dx < 8; dx++) {
                                int c0 = dx * 2, c1 = c0 + 1;
                                y16[oy + dx] = (uint8_t)((sy[r0+c0] + sy[r0+c1] + sy[r1+c0] + sy[r1+c1] + 2) >> 2);
                            }
                        }
                        for (int dy = 0; dy < 4; dy++) {
                            int r0 = (dy * 2) * 8, r1 = r0 + 8;
                            int oc = (qy * 4 + dy) * 8 + qx * 4;
                            for (int dx = 0; dx < 4; dx++) {
                                int c0 = dx * 2, c1 = c0 + 1;
                                cb8[oc + dx] = (uint8_t)((su8[r0+c0] + su8[r0+c1] + su8[r1+c0] + su8[r1+c1] + 2) >> 2);
                                cr8[oc + dx] = (uint8_t)((sv8[r0+c0] + sv8[r0+c1] + sv8[r1+c0] + sv8[r1+c1] + 2) >> 2);
                            }
                        }
                    }
                }
                grade_and_quant_y16_cb8_cr8(s->ycc_lut, s->out_state, y16, cb8, cr8,
                                            omx, out_my, s->grain_mid_lut, s->has_grain, s->clarity_strength,
                                            row_qb + (size_t)omx * 6U,
                                            s->preview_rgb565, s->preview_w, s->preview_h);
            }
        }
    }
    return NULL;
}

typedef struct {
    const FastJpegInfo* info;
    FastBitReader* br;
    DecBlock* dst_band;
    int band_in_rows;
    int y_cnt;
    int std_order;
    int* dc_pred;
    int* mcus_until_rst;
    int fail_rc;
} DecodeBandJob;

static void decode_band_rows(DecodeBandJob* d) {
    const FastJpegInfo* info = d->info;
    FastBitReader* br = d->br;
    const FastHuffTable* dc0 = &info->dc_huff[info->td[0]];
    const FastHuffTable* ac0 = &info->ac_huff[info->ta[0]];
    const FastHuffTable* dc1 = &info->dc_huff[info->td[1]];
    const FastHuffTable* ac1 = &info->ac_huff[info->ta[1]];
    const FastHuffTable* dc2 = &info->dc_huff[info->td[2]];
    const FastHuffTable* ac2 = &info->ac_huff[info->ta[2]];
    int* dc_pred = d->dc_pred;
    int mcus_until_rst = *(d->mcus_until_rst);
    int fail_rc = 0;

    for (int r = 0; r < d->band_in_rows && !fail_rc; r++) {
        DecBlock* row_blk = d->dst_band + (size_t)r * (size_t)info->in_mcus_x * (size_t)info->blks_per_mcu;
        for (int mx = 0; mx < info->in_mcus_x; mx++) {
            if (info->restart_interval > 0 && mcus_until_rst == 0) {
                if (!fbr_consume_restart(br)) { fail_rc = -11; break; }
                dc_pred[0] = dc_pred[1] = dc_pred[2] = 0;
                mcus_until_rst = info->restart_interval;
            }
            DecBlock* mb = row_blk + (size_t)mx * (size_t)info->blks_per_mcu;
            if (d->std_order) {
                for (int b = 0; b < d->y_cnt; b++) {
                    if (!decode_huff_block(br, dc0, ac0, &dc_pred[0], mb[b].c, &mb[b].has_ac)) { fail_rc = -12; break; }
                }
                if (fail_rc) break;
                if (!decode_huff_block(br, dc1, ac1, &dc_pred[1], mb[d->y_cnt].c, &mb[d->y_cnt].has_ac)) { fail_rc = -12; break; }
                if (!decode_huff_block(br, dc2, ac2, &dc_pred[2], mb[d->y_cnt + 1].c, &mb[d->y_cnt + 1].has_ac)) { fail_rc = -12; break; }
            } else {
                for (int si = 0; si < 3 && !fail_rc; si++) {
                    int cidx = info->sos_order[si];
                    const FastHuffTable* dct = &info->dc_huff[info->td[cidx]];
                    const FastHuffTable* act = &info->ac_huff[info->ta[cidx]];
                    if (cidx == 0) {
                        for (int b = 0; b < d->y_cnt; b++) {
                            if (!decode_huff_block(br, dct, act, &dc_pred[0], mb[b].c, &mb[b].has_ac)) { fail_rc = -12; break; }
                        }
                    } else {
                        DecBlock* dst_b = (cidx == 1) ? &mb[d->y_cnt] : &mb[d->y_cnt + 1];
                        if (!decode_huff_block(br, dct, act, &dc_pred[cidx], dst_b->c, &dst_b->has_ac)) { fail_rc = -12; break; }
                    }
                }
                if (fail_rc) break;
            }
            if (info->restart_interval > 0) mcus_until_rst--;
        }
    }
    *(d->mcus_until_rst) = mcus_until_rst;
    d->fail_rc = fail_rc;
}

#ifndef _WIN32
static void* fast_decode_worker(void* arg) {
    decode_band_rows((DecodeBandJob*)arg);
    return NULL;
}
#endif

typedef struct {
    JpegStreamState* state;
    const JpegQuantBlock* blocks;
    int num_mcus;
    int ok;
} EmitJob;

#ifndef _WIN32
static void* fast_emit_worker(void* arg) {
    EmitJob* j = (EmitJob*)arg;
    j->ok = native_jpeg_emit_quantized_blocks(j->state, j->blocks, j->num_mcus);
    return NULL;
}
#endif

int native_fast_transcode_jpeg_ex(const Lut17Context* lut,
                                  const char* input_jpeg_path,
                                  const char* output_jpeg_path,
                                  int scale_denom,
                                  int quality,
                                  int grain_strength,
                                  int clarity_strength,
                                  FastTranscodeResult* out_result) {
    uint64_t t_start = fs_now_ms();
    int ncpu = 1, has_neon = 0, is_v7 = 0;
    fs_probe_cpu(&ncpu, &has_neon, &is_v7);
    if (out_result) {
        memset(out_result, 0, sizeof(FastTranscodeResult));
        out_result->ncpu = ncpu;
        out_result->has_neon = has_neon;
        out_result->is_v7 = is_v7;
    }

    if (!lut || !input_jpeg_path || !output_jpeg_path) {
        if (out_result) out_result->rc = -1;
        return -1;
    }
    init_static_grain_table();

    FILE* fp = fopen(input_jpeg_path, "rb");
    if (!fp) {
        if (out_result) out_result->rc = -1;
        return -1;
    }

    FastJpegInfo* info = (FastJpegInfo*)calloc(1, sizeof(FastJpegInfo));
    if (!info) {
        fclose(fp);
        if (out_result) out_result->rc = -10;
        return -10;
    }
    int hdr_rc = parse_jpeg_headers(fp, info);
    if (hdr_rc <= 0) {
        free(info);
        fclose(fp);
        if (out_result) out_result->rc = hdr_rc;
        return hdr_rc;
    }

    int scale = (scale_denom == 2) ? 2 : ((scale_denom == 0 && info->src_w >= 7000) ? 2 : 1);
    info->scale = scale;
    info->dst_w = (scale == 2) ? (info->src_w / 2) : info->src_w;
    info->dst_h = (scale == 2) ? (info->src_h / 2) : info->src_h;
    info->out_mcus_x = (info->dst_w + 15) >> 4;
    info->out_mcus_y = (info->dst_h + 15) >> 4;

    uint8_t clean_exif[512];
    size_t clean_exif_len = build_clean_exif_app1(clean_exif, sizeof(clean_exif), &info->exif, info->dst_w, info->dst_h);

    JpegStreamState* out_state = native_jpeg_stream_open_with_exif(
        output_jpeg_path, info->dst_w, info->dst_h, quality > 0 ? quality : 92,
        clean_exif_len > 0 ? clean_exif : NULL, clean_exif_len);
    if (!out_state) {
        free(info);
        fclose(fp);
        if (out_result) out_result->rc = -9;
        return -9;
    }

    int8_t* ycc_lut = (int8_t*)malloc(YCC_LUT_BYTES);
    if (!ycc_lut) {
        native_jpeg_stream_close(out_state, 0);
        remove(output_jpeg_path);
        free(info);
        fclose(fp);
        if (out_result) out_result->rc = -10;
        return -10;
    }
    build_ycc_lut17(lut, ycc_lut);

    // Precompute 256-byte luma grain envelope table (eliminates per-pixel midtone & scale multiplies)
    int grain_scale = (grain_strength == 2) ? 4 : (grain_strength == 1) ? 2 : 0;
    uint8_t grain_mid_lut[256];
    for (int i = 0; i < 256; i++) {
        int v = (grain_scale * ((i * (255 - i)) >> 8));
        grain_mid_lut[i] = (uint8_t)(v > 255 ? 255 : v);
    }

    // Optional zero-I/O DC RGB565 Quick Review preview (1 pixel per 8x8 block: 646x431 on 17.7MP)
    uint16_t* preview_rgb565 = NULL;
    int preview_w = 0, preview_h = 0;
    if (out_result) {
        preview_w = (info->dst_w + 7) >> 3;
        preview_h = (info->dst_h + 7) >> 3;
        if (preview_w > 0 && preview_h > 0 && preview_w <= 1280 && preview_h <= 960) {
            preview_rgb565 = (uint16_t*)malloc((size_t)preview_w * (size_t)preview_h * sizeof(uint16_t));
        }
    }

    const int band_out_rows = (scale == 2) ? 4 : 8;
    int in_rows_per_out = ((info->samp_mode == 0 || info->samp_mode == 3) ? 1 : 2) * scale;
    int band_in_max_rows = band_out_rows * in_rows_per_out;

    size_t in_band_blks = (size_t)band_in_max_rows * (size_t)info->in_mcus_x * (size_t)info->blks_per_mcu;
    size_t q_band_blks = (size_t)band_out_rows * (size_t)info->out_mcus_x * 6U;

    DecBlock* in_band_work = (DecBlock*)malloc(in_band_blks * sizeof(DecBlock));
    DecBlock* in_band_dec  = (DecBlock*)malloc(in_band_blks * sizeof(DecBlock)); // Double-buffer for 3-stage overlap (optional on tight RAM)
    JpegQuantBlock* q_work = (JpegQuantBlock*)malloc(q_band_blks * sizeof(JpegQuantBlock));
    JpegQuantBlock* q_emit = (JpegQuantBlock*)malloc(q_band_blks * sizeof(JpegQuantBlock));
    FastBitReader* br = (FastBitReader*)calloc(1, sizeof(FastBitReader));

    if (!in_band_work || !q_work || !q_emit || !br) {
        free(in_band_work); free(in_band_dec); free(q_work); free(q_emit); free(br);
        free(ycc_lut); free(preview_rgb565);
        native_jpeg_stream_close(out_state, 0);
        remove(output_jpeg_path);
        free(info);
        fclose(fp);
        if (out_result) out_result->rc = -10;
        return -10;
    }
    br->fp = fp;

    int dc_pred[3] = { 0, 0, 0 };
    int mcus_until_rst = info->restart_interval;
    int fail_rc = 0;
    int cur_in_my = 0;
    int y_cnt = (info->samp_mode == 0) ? 4 : ((info->samp_mode == 1 || info->samp_mode == 3) ? 2 : 1);
    int std_order = (info->sos_order[0] == 0 && info->sos_order[1] == 1 && info->sos_order[2] == 2);
    uint64_t dec_wait_ms = 0, proc_ms = 0, emit_wait_ms = 0;

    EmitJob emit_job;
    memset(&emit_job, 0, sizeof(emit_job));
    emit_job.ok = 1;
#ifndef _WIN32
    pthread_t emit_thread;
    int emit_active = 0;
#endif

    // Prime Band 0 synchronously into in_band_work
    int cur_band_in_my0 = 0;
    int cur_band_in_rows = 0;
    if (info->out_mcus_y > 0) {
        int b0_out = info->out_mcus_y < band_out_rows ? info->out_mcus_y : band_out_rows;
        int need0 = b0_out * in_rows_per_out;
        if (need0 > info->in_mcus_y) need0 = info->in_mcus_y;
        cur_band_in_rows = need0;
        DecodeBandJob d0 = { info, br, in_band_work, cur_band_in_rows, y_cnt, std_order, dc_pred, &mcus_until_rst, 0 };
        uint64_t td0 = fs_now_ms();
        decode_band_rows(&d0);
        dec_wait_ms += (fs_now_ms() - td0);
        cur_in_my += cur_band_in_rows;
        fail_rc = d0.fail_rc;
    }

    int worker_count = (ncpu >= 4) ? 4 : (ncpu >= 2 ? ncpu : 2);
    if (worker_count > 4) worker_count = 4;

    for (int out_my0 = 0; out_my0 < info->out_mcus_y && !fail_rc; out_my0 += band_out_rows) {
        int cur_out_rows = info->out_mcus_y - out_my0;
        if (cur_out_rows > band_out_rows) cur_out_rows = band_out_rows;

        // Overlap Stage 1 (SD read + Huffman decode of Band N+1) with Stage 2 (parallel IDCT+LUT+FDCT of Band N)!
        int next_out_my0 = out_my0 + band_out_rows;
        int has_next = (next_out_my0 < info->out_mcus_y) ? 1 : 0;
        int next_band_in_my0 = cur_in_my;
        int next_band_in_rows = 0;
        DecodeBandJob dec_job;
        memset(&dec_job, 0, sizeof(dec_job));
#ifndef _WIN32
        pthread_t dec_thread;
        int dec_active = 0;
#endif
        if (has_next) {
            int next_out_rows = info->out_mcus_y - next_out_my0;
            if (next_out_rows > band_out_rows) next_out_rows = band_out_rows;
            int need_next = (next_out_my0 + next_out_rows) * in_rows_per_out;
            if (need_next > info->in_mcus_y) need_next = info->in_mcus_y;
            next_band_in_rows = need_next - next_band_in_my0;
            if (next_band_in_rows < 0) next_band_in_rows = 0;
            dec_job.info = info;
            dec_job.br = br;
            dec_job.dst_band = in_band_dec ? in_band_dec : in_band_work;
            dec_job.band_in_rows = next_band_in_rows;
            dec_job.y_cnt = y_cnt;
            dec_job.std_order = std_order;
            dec_job.dc_pred = dc_pred;
            dec_job.mcus_until_rst = &mcus_until_rst;
#ifndef _WIN32
            if (in_band_dec != NULL && next_band_in_rows > 0) {
                if (pthread_create(&dec_thread, NULL, fast_decode_worker, &dec_job) == 0) {
                    dec_active = 1;
                }
            }
#endif
        }

        // Stage 2: Parallel IDCT + Direct YCbCr 3D LUT + FDCT + Quant + Zero-I/O Preview
        uint64_t tp0 = fs_now_ms();
        FastBandSlice slices[4];
        int cur_lm = 0;
        for (int k = 0; k < worker_count; k++) {
            int rem = cur_out_rows - cur_lm;
            int rem_c = worker_count - k;
            int sl = (rem > 0) ? ((rem + rem_c - 1) / rem_c) : 0;
            slices[k].info = info;
            slices[k].ycc_lut = ycc_lut;
            slices[k].out_state = out_state;
            slices[k].in_band = in_band_work;
            slices[k].band_in_my0 = cur_band_in_my0;
            slices[k].band_in_rows = cur_band_in_rows > 0 ? cur_band_in_rows : 1;
            slices[k].out_my_base = out_my0;
            slices[k].local_my_start = cur_lm;
            slices[k].local_my_end = cur_lm + sl;
            slices[k].grain_mid_lut = grain_mid_lut;
            slices[k].has_grain = (grain_scale > 0) ? 1 : 0;
            slices[k].clarity_strength = clarity_strength;
            slices[k].q_out_base = q_work;
            slices[k].preview_rgb565 = preview_rgb565;
            slices[k].preview_w = preview_w;
            slices[k].preview_h = preview_h;
            cur_lm += sl;
        }

#ifndef _WIN32
        pthread_t threads[3];
        int spawned[3] = { 0, 0, 0 };
        for (int k = 1; k < worker_count; k++) {
            if (slices[k].local_my_end > slices[k].local_my_start) {
                if (pthread_create(&threads[k - 1], NULL, fast_band_worker, &slices[k]) == 0) spawned[k - 1] = 1;
            }
        }
        if (slices[0].local_my_end > slices[0].local_my_start) fast_band_worker(&slices[0]);
        for (int k = 1; k < worker_count; k++) {
            if (slices[k].local_my_end > slices[k].local_my_start && !spawned[k - 1]) fast_band_worker(&slices[k]);
        }
        for (int k = 1; k < worker_count; k++) {
            if (spawned[k - 1]) pthread_join(threads[k - 1], NULL);
        }
        proc_ms += (fs_now_ms() - tp0);

        // Join overlapped decode of Band N+1 (or run synchronously if double-buffer wasn't used)
        if (has_next) {
            uint64_t td1 = fs_now_ms();
            if (dec_active) {
                pthread_join(dec_thread, NULL);
                DecBlock* tdb = in_band_work; in_band_work = in_band_dec; in_band_dec = tdb;
            } else {
                dec_job.dst_band = in_band_work;
                decode_band_rows(&dec_job);
            }
            dec_wait_ms += (fs_now_ms() - td1);
            cur_in_my += next_band_in_rows;
            cur_band_in_my0 = next_band_in_my0;
            cur_band_in_rows = next_band_in_rows;
            if (dec_job.fail_rc) { fail_rc = dec_job.fail_rc; break; }
        }

        // Wait for previous band's Huffman emit thread, swap pointers (0 memcpy!), launch next emit
        if (emit_active) {
            uint64_t te0 = fs_now_ms();
            pthread_join(emit_thread, NULL);
            emit_wait_ms += (fs_now_ms() - te0);
            emit_active = 0;
            if (!emit_job.ok) { fail_rc = -13; break; }
        }
        JpegQuantBlock* tmp = q_work; q_work = q_emit; q_emit = tmp;
        emit_job.state = out_state;
        emit_job.blocks = q_emit;
        emit_job.num_mcus = cur_out_rows * info->out_mcus_x;
        emit_job.ok = 1;
        if (pthread_create(&emit_thread, NULL, fast_emit_worker, &emit_job) == 0) {
            emit_active = 1;
        } else {
            uint64_t te1 = fs_now_ms();
            if (!native_jpeg_emit_quantized_blocks(out_state, q_emit, emit_job.num_mcus)) fail_rc = -13;
            emit_wait_ms += (fs_now_ms() - te1);
        }
#else
        for (int k = 0; k < worker_count; k++) {
            if (slices[k].local_my_end > slices[k].local_my_start) fast_band_worker(&slices[k]);
        }
        if (has_next) {
            dec_job.dst_band = in_band_work;
            decode_band_rows(&dec_job);
            cur_in_my += next_band_in_rows;
            cur_band_in_my0 = next_band_in_my0;
            cur_band_in_rows = next_band_in_rows;
            if (dec_job.fail_rc) { fail_rc = dec_job.fail_rc; break; }
        }
        if (!native_jpeg_emit_quantized_blocks(out_state, q_work, cur_out_rows * info->out_mcus_x)) fail_rc = -13;
#endif
    }

#ifndef _WIN32
    if (emit_active) {
        uint64_t te2 = fs_now_ms();
        pthread_join(emit_thread, NULL);
        emit_wait_ms += (fs_now_ms() - te2);
        if (!emit_job.ok) fail_rc = -13;
    }
#endif

    free(in_band_work);
    free(in_band_dec);
    free(q_work);
    free(q_emit);
    free(br);
    free(ycc_lut);
    free(info);
    fclose(fp);

    int success = (fail_rc == 0) ? 1 : 0;
    int closed = native_jpeg_stream_close(out_state, success);
    int final_rc = (success && closed) ? 1 : (fail_rc ? fail_rc : -13);
    if (final_rc <= 0) {
        remove(output_jpeg_path);
        free(preview_rgb565);
        preview_rgb565 = NULL;
    }

    if (out_result) {
        out_result->rc = final_rc;
        out_result->total_ms = (int)(fs_now_ms() - t_start);
        out_result->dec_ms = (int)dec_wait_ms;
        out_result->proc_ms = (int)proc_ms;
        out_result->emit_ms = (int)emit_wait_ms;
        out_result->preview_rgb565 = preview_rgb565;
        out_result->preview_w = preview_rgb565 ? preview_w : 0;
        out_result->preview_h = preview_rgb565 ? preview_h : 0;
    } else {
        free(preview_rgb565);
    }
    return final_rc;
}

int native_fast_transcode_jpeg(const Lut17Context* lut,
                               const char* input_jpeg_path,
                               const char* output_jpeg_path,
                               int scale_denom,
                               int quality,
                               int grain_strength,
                               int clarity_strength) {
    return native_fast_transcode_jpeg_ex(lut, input_jpeg_path, output_jpeg_path,
                                         scale_denom, quality, grain_strength, clarity_strength, NULL);
}
