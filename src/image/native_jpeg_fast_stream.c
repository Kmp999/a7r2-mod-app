#include "native_jpeg_fast_stream.h"
#include "native_jpeg_writer.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <pthread.h>
#endif

static const uint8_t FS_ZIGZAG[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5, 12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51, 58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};

// Q12 AAN 1D IDCT scale factors: round(4096 * a_k) where a_0=1, a_k=sqrt(2)*cos(k*pi/16)
static const uint32_t FS_AAN_Q12[8] = { 4096, 5681, 5352, 4816, 4096, 3218, 2217, 1130 };

// L1-cache RGB->YCbCr tables (9KB)
static int32_t fs_yr[256], fs_yg[256], fs_yb[256];
static int32_t fs_cbr[256], fs_cbg[256], fs_cbb[256];
static int32_t fs_crr[256], fs_crg[256], fs_crb[256];
static int fs_tables_ready = 0;

static void fs_init_tables(void) {
    if (fs_tables_ready) return;
    for (int i = 0; i < 256; i++) {
        fs_yr[i]  =  19595 * i; fs_yg[i]  =  38470 * i; fs_yb[i]  =   7471 * i + 32768;
        fs_cbr[i] = -11059 * i; fs_cbg[i] = -21709 * i; fs_cbb[i] =  32768 * i + 32768;
        fs_crr[i] =  32768 * i; fs_crg[i] = -27439 * i; fs_crb[i] =  -5329 * i + 32768;
    }
    fs_tables_ready = 1;
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

// Builds a clean, 100% spec-compliant TIFF EXIF APP1 segment (zero stale IFD1 thumbnail, zero broken MakerNotes)
static size_t build_clean_exif_app1(uint8_t* out, size_t max_len, const CleanExifInfo* info, int dst_w, int dst_h) {
    if (max_len < 512) return 0;
    uint8_t* tiff = out + 10;
    size_t pos = 0;
    // TIFF header: "II", 42, IFD0 at offset 8
    tiff[0] = 'I'; tiff[1] = 'I'; wr_u16_le(tiff + 2, 42); wr_u32_le(tiff + 4, 8);

    // Count IFD0 entries: Make, Model, Orientation, XRes, YRes, ResUnit, Software, [DateTime], ExifIFD
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

    // IFD0 entries (strictly sorted by tag ID)
    PUT_ENTRY(e0, 0x010F, 2, 5, make_off);
    PUT_ENTRY(e0, 0x0110, 2, 10, model_off);
    PUT_ENTRY(e0, 0x0112, 3, 1, info->orientation ? info->orientation : 1);
    PUT_ENTRY(e0, 0x011A, 5, 1, xres_off);
    PUT_ENTRY(e0, 0x011B, 5, 1, yres_off);
    PUT_ENTRY(e0, 0x0128, 3, 1, 2);
    PUT_ENTRY(e0, 0x0131, 2, 14, sw_off);
    if (info->has_datetime) PUT_ENTRY(e0, 0x0132, 2, 20, dt_off);
    PUT_ENTRY(e0, 0x8769, 4, 1, exif_off);
    wr_u32_le(e0, 0); // NextIFDOffset = 0 (NO stale thumbnail!)

    // ExifIFD entries (strictly sorted by tag ID)
    if (info->has_exp) {
        uint8_t r[8]; uint32_t o; wr_u32_le(r, info->exp_num); wr_u32_le(r + 4, info->exp_den);
        ALLOC_DATA(r, 8, o); PUT_ENTRY(ee, 0x829A, 5, 1, o);
    }
    if (info->has_fnum) {
        uint8_t r[8]; uint32_t o; wr_u32_le(r, info->fnum_num); wr_u32_le(r + 4, info->fnum_den);
        ALLOC_DATA(r, 8, o); PUT_ENTRY(ee, 0x829D, 5, 1, o);
    }
    if (info->has_iso) PUT_ENTRY(ee, 0x8827, 3, 1, info->iso);
    PUT_ENTRY(ee, 0x9000, 7, 4, 0x30333230U); // "0230" little-endian
    if (info->has_datetime) PUT_ENTRY(ee, 0x9003, 2, 20, dt_off);
    if (info->has_flen) {
        uint8_t r[8]; uint32_t o; wr_u32_le(r, info->flen_num); wr_u32_le(r + 4, info->flen_den);
        ALLOC_DATA(r, 8, o); PUT_ENTRY(ee, 0x920A, 5, 1, o);
    }
    PUT_ENTRY(ee, 0xA001, 3, 1, 1); // sRGB
    PUT_ENTRY(ee, 0xA002, 4, 1, (uint32_t)dst_w);
    PUT_ENTRY(ee, 0xA003, 4, 1, (uint32_t)dst_h);
    wr_u32_le(ee, 0);

    #undef PUT_ENTRY
    #undef ALLOC_DATA

    pos = 10 + (size_t)data_off;
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
    while (br->bitcnt <= 24) {
        if (br->marker_hit || br->eof) {
            br->bitcnt += 8; // Zero-pad trailing bits
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

// Decodes one 8x8 Huffman block into natural-order quantized coefficients. Returns 1 on success.
static inline int decode_huff_block(FastBitReader* br, const FastHuffTable* dc_t, const FastHuffTable* ac_t,
                                    int* dc_pred, int16_t* out64, uint8_t* out_has_ac) {
    memset(out64, 0, 64 * sizeof(int16_t));
    int s = fbr_huff_decode(br, dc_t);
    if (s < 0 || s > 15) return 0;
    int diff = (s > 0) ? fbr_extend(fbr_get_bits(br, s), s) : 0;
    *dc_pred += diff;
    out64[0] = (int16_t)(*dc_pred);

    uint8_t has_ac = 0;
    int k = 1;
    while (k < 64) {
        int rs = fbr_huff_decode(br, ac_t);
        if (rs < 0) return 0;
        int r = rs >> 4;
        int sz = rs & 0x0F;
        if (sz == 0) {
            if (r == 15) { k += 16; continue; }
            break; // EOB
        }
        k += r;
        if (k >= 64) break;
        int val = fbr_extend(fbr_get_bits(br, sz), sz);
        out64[FS_ZIGZAG[k]] = (int16_t)val;
        has_ac = 1;
        k++;
    }
    *out_has_ac = has_ac;
    return 1;
}

// Fast 5-multiply AAN 2D IDCT (Q7 dequant -> Q8 butterfly -> Q6 intermediate -> exact 8-bit 0..255 output)
// 7.8x overflow headroom on 32-bit int; 0-multiply DC-only shortcut for flat blocks.
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

typedef struct {
    int16_t c[64];
    uint8_t has_ac;
} DecBlock;

typedef struct {
    int src_w, src_h;
    int dst_w, dst_h;
    int scale;          // 1 or 2
    int samp_mode;      // 0 = 4:2:0 (16x16), 1 = 4:2:2 (16x8), 2 = 4:4:4 (8x8)
    int in_mcus_x;      // input MCUs per row
    int in_mcus_y;      // total input MCU rows
    int blks_per_mcu;   // 6 (4:2:0), 4 (4:2:2), 3 (4:4:4)
    int out_mcus_x;     // output 4:2:0 MCUs per row
    int out_mcus_y;     // total output 4:2:0 MCU rows
    int tq[3];          // quant table idx per component
    int td[3], ta[3];   // DC/AC huff table idx per component
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
    if (fgetc(fp) != 0xFF || fgetc(fp) != 0xD8) return 0;

    int comp_id[3] = {0, 0, 0};
    int hs[3] = {0, 0, 0}, vs[3] = {0, 0, 0};
    int sof_found = 0;

    for (int iter = 0; iter < 128; iter++) {
        int c = fgetc(fp);
        if (c < 0) return 0;
        if (c != 0xFF) return 0;
        do { c = fgetc(fp); } while (c == 0xFF);
        if (c < 0) return 0;
        if (c == 0xD9) return 0;
        if (c == 0x01 || (c >= 0xD0 && c <= 0xD7)) continue;

        uint16_t seg_len;
        if (!rd_u16_file(fp, &seg_len) || seg_len < 2) return 0;
        size_t pay_len = (size_t)seg_len - 2;

        if (c == 0xE1 && pay_len >= 14 && pay_len <= 65533) {
            uint8_t* tmp = (uint8_t*)malloc(pay_len);
            if (!tmp) return 0;
            if (fread(tmp, 1, pay_len, fp) != pay_len) { free(tmp); return 0; }
            parse_exif_app1(tmp, pay_len, &info->exif);
            free(tmp);
        } else if (c == 0xDB) { // DQT
            uint8_t* tmp = (uint8_t*)malloc(pay_len);
            if (!tmp) return 0;
            if (fread(tmp, 1, pay_len, fp) != pay_len) { free(tmp); return 0; }
            size_t p = 0;
            while (p < pay_len) {
                uint8_t pt = tmp[p++];
                int pq = pt >> 4, tq = pt & 0x0F;
                if (pq != 0 || tq >= 4 || p + 64 > pay_len) { free(tmp); return 0; }
                for (int k = 0; k < 64; k++) {
                    int nat = FS_ZIGZAG[k];
                    uint16_t qv = tmp[p++];
                    info->qt_raw[tq][nat] = qv;
                    uint64_t num = (uint64_t)qv * (uint64_t)FS_AAN_Q12[nat >> 3] * (uint64_t)FS_AAN_Q12[nat & 7];
                    info->dq_aan[tq][nat] = (int32_t)((num + 65536ULL) >> 17); // Q7
                }
                info->qt_valid[tq] = 1;
            }
            free(tmp);
        } else if (c == 0xC0) { // SOF0 Baseline DCT
            if (pay_len < 6) return 0;
            uint8_t* tmp = (uint8_t*)malloc(pay_len);
            if (!tmp) return 0;
            if (fread(tmp, 1, pay_len, fp) != pay_len) { free(tmp); return 0; }
            int prec = tmp[0];
            info->src_h = (tmp[1] << 8) | tmp[2];
            info->src_w = (tmp[3] << 8) | tmp[4];
            int nf = tmp[5];
            if (prec != 8 || nf != 3 || pay_len < 6 + 9 || info->src_w <= 0 || info->src_h <= 0) { free(tmp); return 0; }
            for (int i = 0; i < 3; i++) {
                comp_id[i] = tmp[6 + i * 3];
                hs[i] = tmp[7 + i * 3] >> 4;
                vs[i] = tmp[7 + i * 3] & 0x0F;
                info->tq[i] = tmp[8 + i * 3];
                if (info->tq[i] >= 4) { free(tmp); return 0; }
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
            } else {
                return 0;
            }
            sof_found = 1;
        } else if (c == 0xC1 || c == 0xC2 || c == 0xC3 || c == 0xC9 || c == 0xCA) {
            return 0; // Progressive / Arithmetic / Lossless -> fallback cleanly
        } else if (c == 0xC4) { // DHT
            uint8_t* tmp = (uint8_t*)malloc(pay_len);
            if (!tmp) return 0;
            if (fread(tmp, 1, pay_len, fp) != pay_len) { free(tmp); return 0; }
            size_t p = 0;
            while (p < pay_len) {
                if (p + 17 > pay_len) { free(tmp); return 0; }
                uint8_t tc_th = tmp[p++];
                int tc = tc_th >> 4, th = tc_th & 0x0F;
                if (tc > 1 || th >= 4) { free(tmp); return 0; }
                uint8_t bits[17]; bits[0] = 0;
                int total = 0;
                for (int i = 1; i <= 16; i++) { bits[i] = tmp[p++]; total += bits[i]; }
                if (p + (size_t)total > pay_len || total > 256) { free(tmp); return 0; }
                build_fast_huff(tc == 0 ? &info->dc_huff[th] : &info->ac_huff[th], bits, tmp + p, total);
                p += (size_t)total;
            }
            free(tmp);
        } else if (c == 0xDD) { // DRI
            if (pay_len != 2) return 0;
            uint16_t ri;
            if (!rd_u16_file(fp, &ri)) return 0;
            info->restart_interval = (int)ri;
        } else if (c == 0xDA) { // SOS
            if (!sof_found || pay_len < 10) return 0;
            uint8_t* tmp = (uint8_t*)malloc(pay_len);
            if (!tmp) return 0;
            if (fread(tmp, 1, pay_len, fp) != pay_len) { free(tmp); return 0; }
            int ns = tmp[0];
            if (ns != 3 || pay_len < (size_t)(1 + ns * 2 + 3)) { free(tmp); return 0; }
            for (int i = 0; i < 3; i++) {
                int cs = tmp[1 + i * 2];
                int td = tmp[2 + i * 2] >> 4;
                int ta = tmp[2 + i * 2] & 0x0F;
                if (cs != comp_id[i] || td >= 4 || ta >= 4) { free(tmp); return 0; }
                info->td[i] = td;
                info->ta[i] = ta;
            }
            int ss = tmp[1 + ns * 2], se = tmp[2 + ns * 2], ahal = tmp[3 + ns * 2];
            free(tmp);
            if (ss != 0 || se != 63 || ahal != 0) return 0;
            for (int i = 0; i < 3; i++) {
                if (!info->qt_valid[info->tq[i]] || !info->dc_huff[info->td[i]].valid || !info->ac_huff[info->ta[i]].valid) return 0;
            }
            return 1; // Positioned right at entropy scan data
        } else {
            if (fseek(fp, (long)pay_len, SEEK_CUR) != 0) return 0;
        }
    }
    return 0;
}

// Decode a single 16x16 source tile at tile coordinates (tx, ty) from in_band starting at input MCU row band_in_my0
static void decode_src_tile_16x16(const FastJpegInfo* info, const DecBlock* in_band, int band_in_my0, int band_in_rows,
                                  int tx, int ty, uint8_t y16[256], uint8_t cb16[256], uint8_t cr16[256]) {
    const int32_t* dqY = info->dq_aan[info->tq[0]]; const uint16_t* qY = info->qt_raw[info->tq[0]];
    const int32_t* dqU = info->dq_aan[info->tq[1]]; const uint16_t* qU = info->qt_raw[info->tq[1]];
    const int32_t* dqV = info->dq_aan[info->tq[2]]; const uint16_t* qV = info->qt_raw[info->tq[2]];

    if (info->samp_mode == 0) { // 4:2:0: 1 input MCU per 16x16 tile
        int r = ty - band_in_my0;
        if (r < 0) r = 0;
        if (r >= band_in_rows) r = band_in_rows - 1;
        int cx = tx < info->in_mcus_x ? tx : (info->in_mcus_x - 1);
        const DecBlock* mb = in_band + ((size_t)r * (size_t)info->in_mcus_x + (size_t)cx) * 6U;
        uint8_t yb[4][64], cbb[64], crb[64];
        for (int b = 0; b < 4; b++) idct_8x8_aan(mb[b].c, mb[b].has_ac, dqY, qY, yb[b]);
        idct_8x8_aan(mb[4].c, mb[4].has_ac, dqU, qU, cbb);
        idct_8x8_aan(mb[5].c, mb[5].has_ac, dqV, qV, crb);
        for (int y = 0; y < 16; y++) {
            int by = (y >= 8) ? 2 : 0, sy = (y & 7) * 8, cy = (y >> 1) * 8;
            memcpy(y16 + y * 16, yb[by] + sy, 8);
            memcpy(y16 + y * 16 + 8, yb[by + 1] + sy, 8);
            for (int x = 0; x < 8; x++) {
                uint8_t u = cbb[cy + x], v = crb[cy + x];
                cb16[y * 16 + x * 2] = u; cb16[y * 16 + x * 2 + 1] = u;
                cr16[y * 16 + x * 2] = v; cr16[y * 16 + x * 2 + 1] = v;
            }
        }
    } else if (info->samp_mode == 1) { // 4:2:2 (Sony BIONZ X): 2 stacked 16x8 input MCUs per 16x16 tile
        int cx = tx < info->in_mcus_x ? tx : (info->in_mcus_x - 1);
        for (int half = 0; half < 2; half++) {
            int in_my = ty * 2 + half;
            int r = in_my - band_in_my0;
            if (r < 0) r = 0;
            if (r >= band_in_rows) r = band_in_rows - 1;
            const DecBlock* mb = in_band + ((size_t)r * (size_t)info->in_mcus_x + (size_t)cx) * 4U;
            uint8_t y0[64], y1[64], cbb[64], crb[64];
            idct_8x8_aan(mb[0].c, mb[0].has_ac, dqY, qY, y0);
            idct_8x8_aan(mb[1].c, mb[1].has_ac, dqY, qY, y1);
            idct_8x8_aan(mb[2].c, mb[2].has_ac, dqU, qU, cbb);
            idct_8x8_aan(mb[3].c, mb[3].has_ac, dqV, qV, crb);
            for (int y = 0; y < 8; y++) {
                int oy = (half * 8 + y) * 16;
                memcpy(y16 + oy, y0 + y * 8, 8);
                memcpy(y16 + oy + 8, y1 + y * 8, 8);
                for (int x = 0; x < 8; x++) {
                    uint8_t u = cbb[y * 8 + x], v = crb[y * 8 + x];
                    cb16[oy + x * 2] = u; cb16[oy + x * 2 + 1] = u;
                    cr16[oy + x * 2] = v; cr16[oy + x * 2 + 1] = v;
                }
            }
        }
    } else { // 4:4:4: 2x2 8x8 input MCUs per 16x16 tile
        for (int hy = 0; hy < 2; hy++) {
            int in_my = ty * 2 + hy;
            int r = in_my - band_in_my0;
            if (r < 0) r = 0;
            if (r >= band_in_rows) r = band_in_rows - 1;
            for (int hx = 0; hx < 2; hx++) {
                int cx = tx * 2 + hx;
                if (cx >= info->in_mcus_x) cx = info->in_mcus_x - 1;
                const DecBlock* mb = in_band + ((size_t)r * (size_t)info->in_mcus_x + (size_t)cx) * 3U;
                uint8_t yb[64], cbb[64], crb[64];
                idct_8x8_aan(mb[0].c, mb[0].has_ac, dqY, qY, yb);
                idct_8x8_aan(mb[1].c, mb[1].has_ac, dqU, qU, cbb);
                idct_8x8_aan(mb[2].c, mb[2].has_ac, dqV, qV, crb);
                for (int y = 0; y < 8; y++) {
                    int oy = (hy * 8 + y) * 16 + hx * 8;
                    memcpy(y16 + oy, yb + y * 8, 8);
                    memcpy(cb16 + oy, cbb + y * 8, 8);
                    memcpy(cr16 + oy, crb + y * 8, 8);
                }
            }
        }
    }
}

static inline void grade_and_quant_tile_16x16(const Lut17Context* lut, const JpegStreamState* out_state,
                                              const uint8_t y16[256], const uint8_t cb16[256], const uint8_t cr16[256],
                                              int global_x0, int global_y0, int grain_scale, int clarity_strength,
                                              JpegQuantBlock* out_6) {
    int16_t y_blk[4][64], cb_blk[64], cr_blk[64];
    int32_t cb_acc[64], cr_acc[64];
    memset(cb_acc, 0, sizeof(cb_acc));
    memset(cr_acc, 0, sizeof(cr_acc));

    for (int y = 0; y < 16; y++) {
        int gy = global_y0 + y;
        int b_idx = (y >= 8) ? 2 : 0;
        int sy8 = (y & 7) * 8;
        int cy8 = (y >> 1) * 8;
        int grain_y_off = (gy & 63) << 6;
        uint32_t rng_state = (grain_scale > 0) ? (uint32_t)((gy * 1103515245U + 12345U) ^ ((uint32_t)global_x0 * 2654435761U)) : 0;
        int prev_luma = 128;

        for (int x = 0; x < 16; x++) {
            int p = y * 16 + x;
            int Y = (int)y16[p];
            int cb = (int)cb16[p] - 128;
            int cr = (int)cr16[p] - 128;

            int r0 = Y + ((91881 * cr + 32768) >> 16);
            int g0 = Y - ((22554 * cb + 46802 * cr + 32768) >> 16);
            int b0 = Y + ((116130 * cb + 32768) >> 16);
            uint8_t r = (uint8_t)(r0 < 0 ? 0 : (r0 > 255 ? 255 : r0));
            uint8_t g = (uint8_t)(g0 < 0 ? 0 : (g0 > 255 ? 255 : g0));
            uint8_t b = (uint8_t)(b0 < 0 ? 0 : (b0 > 255 ? 255 : b0));

            uint8_t or_v, og_v, ob_v;
            lut17_interpolate_pixel(lut, r, g, b, &or_v, &og_v, &ob_v);

            if (grain_scale > 0 || clarity_strength != 0) {
                int luma = (or_v * 77 + og_v * 150 + ob_v * 29) >> 8;
                int delta = 0;
                if (grain_scale > 0) {
                    rng_state = rng_state * 1103515245U + 12345U;
                    int lcg = (int)((rng_state >> 16) & 0x3F) - 32;
                    int tbl = STATIC_GRAIN_64x64[grain_y_off | ((global_x0 + x) & 63)];
                    int mid = (luma * (255 - luma)) >> 8;
                    delta += (((lcg + tbl) >> 1) * grain_scale * mid) >> 8;
                }
                if (clarity_strength != 0) {
                    delta += ((luma - prev_luma) * clarity_strength) >> 4;
                }
                prev_luma = luma;
                or_v = clamp_u8_fast((int)or_v + delta);
                og_v = clamp_u8_fast((int)og_v + delta);
                ob_v = clamp_u8_fast((int)ob_v + delta);
            }

            int blk = b_idx + ((x >= 8) ? 1 : 0);
            y_blk[blk][sy8 + (x & 7)] = (int16_t)(((fs_yr[or_v] + fs_yg[og_v] + fs_yb[ob_v]) >> 16) - 128);
            int cidx = cy8 + (x >> 1);
            cb_acc[cidx] += (fs_cbr[or_v] + fs_cbg[og_v] + fs_cbb[ob_v]) >> 16;
            cr_acc[cidx] += (fs_crr[or_v] + fs_crg[og_v] + fs_crb[ob_v]) >> 16;
        }
    }

    for (int i = 0; i < 64; i++) {
        cb_blk[i] = (int16_t)((cb_acc[i] + 2) >> 2);
        cr_blk[i] = (int16_t)((cr_acc[i] + 2) >> 2);
    }
    native_jpeg_quantize_ycbcr_mcu(out_state, y_blk, cb_blk, cr_blk, out_6);
}

typedef struct {
    const FastJpegInfo* info;
    const Lut17Context* lut;
    const JpegStreamState* out_state;
    const DecBlock* in_band;
    int band_in_my0;
    int band_in_rows;
    int out_my_base;
    int local_my_start;
    int local_my_end;
    int grain_scale;
    int clarity_strength;
    JpegQuantBlock* q_out_base;
} FastBandSlice;

static void* fast_band_worker(void* arg) {
    FastBandSlice* s = (FastBandSlice*)arg;
    const FastJpegInfo* info = s->info;
    uint8_t y16[256], cb16[256], cr16[256];

    for (int lm = s->local_my_start; lm < s->local_my_end; lm++) {
        int out_my = s->out_my_base + lm;
        JpegQuantBlock* row_qb = s->q_out_base + (size_t)lm * (size_t)info->out_mcus_x * 6U;

        if (info->scale == 1) {
            for (int omx = 0; omx < info->out_mcus_x; omx++) {
                decode_src_tile_16x16(info, s->in_band, s->band_in_my0, s->band_in_rows,
                                      omx, out_my, y16, cb16, cr16);
                grade_and_quant_tile_16x16(s->lut, s->out_state, y16, cb16, cr16,
                                           omx * 16, out_my * 16, s->grain_scale, s->clarity_strength,
                                           row_qb + (size_t)omx * 6U);
            }
        } else { // scale == 2: 2x2 source 16x16 tiles -> 2x2 box average -> 1 output 16x16 tile
            uint8_t sy[256], su[256], sv[256];
            for (int omx = 0; omx < info->out_mcus_x; omx++) {
                for (int qy = 0; qy < 2; qy++) {
                    for (int qx = 0; qx < 2; qx++) {
                        decode_src_tile_16x16(info, s->in_band, s->band_in_my0, s->band_in_rows,
                                              omx * 2 + qx, out_my * 2 + qy, sy, su, sv);
                        for (int dy = 0; dy < 8; dy++) {
                            int oy = (qy * 8 + dy) * 16 + qx * 8;
                            int r0 = (dy * 2) * 16, r1 = r0 + 16;
                            for (int dx = 0; dx < 8; dx++) {
                                int c0 = dx * 2, c1 = c0 + 1;
                                y16[oy + dx]  = (uint8_t)((sy[r0+c0] + sy[r0+c1] + sy[r1+c0] + sy[r1+c1] + 2) >> 2);
                                cb16[oy + dx] = (uint8_t)((su[r0+c0] + su[r0+c1] + su[r1+c0] + su[r1+c1] + 2) >> 2);
                                cr16[oy + dx] = (uint8_t)((sv[r0+c0] + sv[r0+c1] + sv[r1+c0] + sv[r1+c1] + 2) >> 2);
                            }
                        }
                    }
                }
                grade_and_quant_tile_16x16(s->lut, s->out_state, y16, cb16, cr16,
                                           omx * 16, out_my * 16, s->grain_scale, s->clarity_strength,
                                           row_qb + (size_t)omx * 6U);
            }
        }
    }
    return NULL;
}

typedef struct {
    JpegStreamState* state;
    const JpegQuantBlock* blocks;
    int num_mcus;
    int ok;
} EmitJob;

static void* fast_emit_worker(void* arg) {
    EmitJob* j = (EmitJob*)arg;
    j->ok = native_jpeg_emit_quantized_blocks(j->state, j->blocks, j->num_mcus);
    return NULL;
}

int native_fast_transcode_jpeg(const Lut17Context* lut,
                               const char* input_jpeg_path,
                               const char* output_jpeg_path,
                               int scale_denom,
                               int quality,
                               int grain_strength,
                               int clarity_strength) {
    if (!lut || !input_jpeg_path || !output_jpeg_path) return 0;
    fs_init_tables();
    init_static_grain_table();

    FILE* fp = fopen(input_jpeg_path, "rb");
    if (!fp) return 0;

    // Heap-allocate FastJpegInfo (~20KB) to keep Android Bionic JNI worker thread stack usage < 2KB
    FastJpegInfo* info = (FastJpegInfo*)calloc(1, sizeof(FastJpegInfo));
    if (!info) {
        fclose(fp);
        return 0;
    }
    if (!parse_jpeg_headers(fp, info)) {
        free(info);
        fclose(fp);
        return 0;
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
        return 0;
    }

    // 4 output MCU rows per parallel band (1 MCU row per Cortex-A5 core; low RAM ~3.3MB)
    const int band_out_rows = 4;
    int in_rows_per_out = (info->samp_mode == 0 ? 1 : 2) * scale;
    int band_in_max_rows = band_out_rows * in_rows_per_out;

    size_t in_band_blks = (size_t)band_in_max_rows * (size_t)info->in_mcus_x * (size_t)info->blks_per_mcu;
    size_t q_band_blks = (size_t)band_out_rows * (size_t)info->out_mcus_x * 6U;

    DecBlock* in_band = (DecBlock*)malloc(in_band_blks * sizeof(DecBlock));
    JpegQuantBlock* q_work = (JpegQuantBlock*)malloc(q_band_blks * sizeof(JpegQuantBlock));
    JpegQuantBlock* q_emit = (JpegQuantBlock*)malloc(q_band_blks * sizeof(JpegQuantBlock));
    FastBitReader* br = (FastBitReader*)calloc(1, sizeof(FastBitReader));

    if (!in_band || !q_work || !q_emit || !br) {
        free(in_band); free(q_work); free(q_emit); free(br);
        native_jpeg_stream_close(out_state, 0);
        free(info);
        fclose(fp);
        return 0;
    }
    br->fp = fp;

    int dc_pred[3] = { 0, 0, 0 };
    int mcus_until_rst = info->restart_interval;
    int grain_scale = (grain_strength == 2) ? 16 : (grain_strength == 1) ? 8 : 0;
    int success = 1;
    int cur_in_my = 0;

    EmitJob emit_job;
    memset(&emit_job, 0, sizeof(emit_job));
    emit_job.ok = 1;
#ifndef _WIN32
    pthread_t emit_thread;
    int emit_active = 0;
#endif

    for (int out_my0 = 0; out_my0 < info->out_mcus_y && success; out_my0 += band_out_rows) {
        int cur_out_rows = info->out_mcus_y - out_my0;
        if (cur_out_rows > band_out_rows) cur_out_rows = band_out_rows;

        int need_in_end = (out_my0 + cur_out_rows) * in_rows_per_out;
        if (need_in_end > info->in_mcus_y) need_in_end = info->in_mcus_y;
        int band_in_my0 = cur_in_my;
        int band_in_rows = need_in_end - band_in_my0;
        if (band_in_rows < 0) band_in_rows = 0;

        // Stage 1: Core 0 Huffman-decodes input MCU rows (overlapped with previous band's background Huffman emit!)
        for (int r = 0; r < band_in_rows && success; r++) {
            DecBlock* row_blk = in_band + (size_t)r * (size_t)info->in_mcus_x * (size_t)info->blks_per_mcu;
            for (int mx = 0; mx < info->in_mcus_x; mx++) {
                if (info->restart_interval > 0 && mcus_until_rst == 0) {
                    if (!fbr_consume_restart(br)) { success = 0; break; }
                    dc_pred[0] = dc_pred[1] = dc_pred[2] = 0;
                    mcus_until_rst = info->restart_interval;
                }
                DecBlock* mb = row_blk + (size_t)mx * (size_t)info->blks_per_mcu;
                int y_cnt = (info->samp_mode == 0) ? 4 : ((info->samp_mode == 1) ? 2 : 1);
                for (int b = 0; b < y_cnt; b++) {
                    if (!decode_huff_block(br, &info->dc_huff[info->td[0]], &info->ac_huff[info->ta[0]],
                                           &dc_pred[0], mb[b].c, &mb[b].has_ac)) { success = 0; break; }
                }
                if (!success) break;
                if (!decode_huff_block(br, &info->dc_huff[info->td[1]], &info->ac_huff[info->ta[1]],
                                       &dc_pred[1], mb[y_cnt].c, &mb[y_cnt].has_ac)) { success = 0; break; }
                if (!decode_huff_block(br, &info->dc_huff[info->td[2]], &info->ac_huff[info->ta[2]],
                                       &dc_pred[2], mb[y_cnt + 1].c, &mb[y_cnt + 1].has_ac)) { success = 0; break; }
                if (info->restart_interval > 0) mcus_until_rst--;
            }
        }
        cur_in_my += band_in_rows;
        if (!success) break;

        // Stage 2: ALL 4 Cortex-A5 cores run L1-cached AAN IDCT + 3D LUT + AAN FDCT + Quantization in parallel!
        FastBandSlice slices[4];
        int cur_lm = 0;
        for (int k = 0; k < 4; k++) {
            int rem = cur_out_rows - cur_lm;
            int rem_c = 4 - k;
            int sl = (rem > 0) ? ((rem + rem_c - 1) / rem_c) : 0;
            slices[k].info = info;
            slices[k].lut = lut;
            slices[k].out_state = out_state;
            slices[k].in_band = in_band;
            slices[k].band_in_my0 = band_in_my0;
            slices[k].band_in_rows = band_in_rows > 0 ? band_in_rows : 1;
            slices[k].out_my_base = out_my0;
            slices[k].local_my_start = cur_lm;
            slices[k].local_my_end = cur_lm + sl;
            slices[k].grain_scale = grain_scale;
            slices[k].clarity_strength = clarity_strength;
            slices[k].q_out_base = q_work;
            cur_lm += sl;
        }

#ifndef _WIN32
        pthread_t threads[3];
        int spawned[3] = { 0, 0, 0 };
        for (int k = 1; k < 4; k++) {
            if (slices[k].local_my_end > slices[k].local_my_start) {
                if (pthread_create(&threads[k - 1], NULL, fast_band_worker, &slices[k]) == 0) spawned[k - 1] = 1;
            }
        }
        if (slices[0].local_my_end > slices[0].local_my_start) fast_band_worker(&slices[0]);
        for (int k = 1; k < 4; k++) {
            if (slices[k].local_my_end > slices[k].local_my_start && !spawned[k - 1]) fast_band_worker(&slices[k]);
        }
        for (int k = 1; k < 4; k++) {
            if (spawned[k - 1]) pthread_join(threads[k - 1], NULL);
        }

        // Wait for previous band's Huffman emit thread, swap pointers (0 memcpy!), launch next emit
        if (emit_active) {
            pthread_join(emit_thread, NULL);
            emit_active = 0;
            if (!emit_job.ok) { success = 0; break; }
        }
        JpegQuantBlock* tmp = q_work; q_work = q_emit; q_emit = tmp;
        emit_job.state = out_state;
        emit_job.blocks = q_emit;
        emit_job.num_mcus = cur_out_rows * info->out_mcus_x;
        emit_job.ok = 1;
        if (pthread_create(&emit_thread, NULL, fast_emit_worker, &emit_job) == 0) {
            emit_active = 1;
        } else {
            if (!native_jpeg_emit_quantized_blocks(out_state, q_emit, emit_job.num_mcus)) success = 0;
        }
#else
        for (int k = 0; k < 4; k++) {
            if (slices[k].local_my_end > slices[k].local_my_start) fast_band_worker(&slices[k]);
        }
        if (!native_jpeg_emit_quantized_blocks(out_state, q_work, cur_out_rows * info->out_mcus_x)) success = 0;
#endif
    }

#ifndef _WIN32
    if (emit_active) {
        pthread_join(emit_thread, NULL);
        if (!emit_job.ok) success = 0;
    }
#endif

    free(in_band);
    free(q_work);
    free(q_emit);
    free(br);
    free(info);
    fclose(fp);

    int closed = native_jpeg_stream_close(out_state, success);
    if (!success || !closed) {
        remove(output_jpeg_path);
        return 0;
    }
    return 1;
}
