#include "native_jpeg_writer.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

static const uint8_t ZIGZAG[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5, 12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51, 58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};

static const uint8_t BASE_Q_LUMA[64] = {
    16,11,10,16,24,40,51,61, 12,12,14,19,26,58,60,55, 14,13,16,24,40,57,69,56, 14,17,22,29,51,87,80,62,
    18,22,37,56,68,109,103,77,24,35,55,64,81,104,113,92,49,64,78,87,103,121,120,101,72,92,95,98,112,100,103,99
};

static const uint8_t BASE_Q_CHROMA[64] = {
    17,18,24,47,99,99,99,99, 18,21,26,66,99,99,99,99, 24,26,56,99,99,99,99,99, 47,66,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99
};

static const uint8_t DHT_MARKER_DATA[] = {
    0x00, 0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0, 0,1,2,3,4,5,6,7,8,9,10,11,
    0x10, 0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7D,
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xA1,0x08,
    0x23,0x42,0xB1,0xC1,0x15,0x52,0xD1,0xF0,0x24,0x33,0x62,0x72,0x82,0x09,0x0A,0x16,0x17,0x18,0x19,0x1A,0x25,0x26,0x27,0x28,
    0x29,0x2A,0x34,0x35,0x36,0x37,0x38,0x39,0x3A,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4A,0x53,0x54,0x55,0x56,0x57,0x58,0x59,
    0x5A,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6A,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8A,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9A,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,0xA8,0xA9,0xAA,0xB2,0xB3,0xB4,0xB5,0xB6,
    0xB7,0xB8,0xB9,0xBA,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,0xE1,0xE2,
    0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9,0xFA,
    0x01, 0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0, 0,1,2,3,4,5,6,7,8,9,10,11,
    0x11, 0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77,
    0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,
    0xA1,0xB1,0xC1,0x09,0x23,0x33,0x52,0xF0,0x15,0x62,0x72,0xD1,0x0A,0x16,0x24,0x34,0xE1,0x25,0xF1,0x17,0x18,0x19,0x1A,0x26,
    0x27,0x28,0x29,0x2A,0x35,0x36,0x37,0x38,0x39,0x3A,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4A,0x53,0x54,0x55,0x56,0x57,0x58,
    0x59,0x5A,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6A,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7A,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8A,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9A,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,0xA8,0xA9,0xAA,0xB2,0xB3,0xB4,
    0xB5,0xB6,0xB7,0xB8,0xB9,0xBA,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,
    0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9,0xFA
};

// Kept for compatibility check
static const int16_t DCT_M[8][8] = {
    { 362, 362, 362, 362, 362, 362, 362, 362}, { 502, 426, 284, 100,-100,-284,-426,-502},
    { 473, 196,-196,-473,-473,-196, 196, 473}, { 426,-100,-502,-284, 284, 502, 100,-426},
    { 362,-362,-362, 362, 362,-362,-362, 362}, { 284,-502, 100, 426,-426,-100, 502,-284},
    { 196,-473, 473,-196,-196, 473,-473, 196}, { 100,-284, 426,-502, 502,-426, 284,-100}
};

// Precomputed L1-cache RGB->YCbCr lookup tables (9KB total; eliminates 10 multiplies per 2x2 block)
static int32_t g_yr[256], g_yg[256], g_yb[256];
static int32_t g_cbr[256], g_cbg[256], g_cbb[256];
static int32_t g_crr[256], g_crg[256], g_crb[256];
static int g_ycbcr_ready = 0;

static void init_ycbcr_tables(void) {
    if (g_ycbcr_ready) return;
    for (int i = 0; i < 256; i++) {
        g_yr[i]  =  19595 * i; g_yg[i]  =  38470 * i; g_yb[i]  =   7471 * i + 32768;
        g_cbr[i] = -11059 * i; g_cbg[i] = -21709 * i; g_cbb[i] =  32768 * i + 32768;
        g_crr[i] =  32768 * i; g_crg[i] = -27439 * i; g_crb[i] =  -5329 * i + 32768;
    }
    g_ycbcr_ready = 1;
}

// Buffered BitWriter (64KB write buffer tuned for FAT32 clusters; heap-allocated to avoid JNI stack overflow)
typedef struct {
    FILE* fp;
    uint32_t buf;
    int bits;
    uint8_t obuf[65536];
    int opos;
} BitWriter;

static inline void bw_put_byte(BitWriter* bw, uint8_t b) {
    bw->obuf[bw->opos++] = b;
    if (b == 0xFF) bw->obuf[bw->opos++] = 0x00;
    if (bw->opos >= 65528) { fwrite(bw->obuf, 1, bw->opos, bw->fp); bw->opos = 0; }
}

static void bw_flush(BitWriter* bw) {
    if (bw->bits > 0) {
        bw->buf = (bw->buf << (8 - bw->bits)) | ((1U << (8 - bw->bits)) - 1U);
        bw_put_byte(bw, (uint8_t)bw->buf);
        bw->buf = 0; bw->bits = 0;
    }
    if (bw->opos > 0) { fwrite(bw->obuf, 1, bw->opos, bw->fp); bw->opos = 0; }
}

static void build_huff_table(const uint8_t* bits, const uint8_t* vals, uint16_t* codes, uint8_t* sizes) {
    uint16_t code = 0; int k = 0;
    for (int len = 1; len <= 16; len++) {
        for (int i = 0; i < bits[len - 1]; i++) {
            uint8_t sym = vals[k++];
            codes[sym] = code++; sizes[sym] = (uint8_t)len;
        }
        code <<= 1;
    }
}

static inline void get_cat_bits(int val, int* cat, int* bits) {
    if (val == 0) { *cat = 0; *bits = 0; return; }
    int v = val < 0 ? -val : val;
#if defined(__GNUC__) || defined(__clang__)
    int c = 32 - __builtin_clz((unsigned int)v); // 1-cycle ARM CLZ instruction
#else
    int c = 0;
    while (v > 0) { c++; v >>= 1; }
#endif
    *cat = c;
    *bits = (val < 0) ? ((val - 1) & ((1 << c) - 1)) : val;
}

// Precomputed Q12 AAN 1D inverse scale factors: round(4096 / (a_k))
static const uint16_t AAN_IFACT[8] = { 4096, 2953, 3135, 3483, 4096, 5213, 7568, 14846 };

// Fast 1D FDCT (True 5-multiply AAN butterfly with zero-row/col early skip; post-scaling folded into inv_q)
static inline void fdct_1d(int16_t* blk) {
    for (int i = 0; i < 8; i++) {
        int16_t* r = blk + i * 8;
        int d3 = r[3] - r[4], d2 = r[2] - r[5], d1 = r[1] - r[6], d0 = r[0] - r[7];
        int s0 = r[0] + r[7], s1 = r[1] + r[6], s2 = r[2] + r[5], s3 = r[3] + r[4];
        int t0 = s0 + s3, t3 = s0 - s3, t1 = s1 + s2, t2 = s1 - s2;
        r[0] = (int16_t)(t0 + t1);
        r[4] = (int16_t)(t0 - t1);
        if ((d0 | d1 | d2 | d3 | t2 | t3) == 0) {
            r[1] = r[2] = r[3] = r[5] = r[6] = r[7] = 0;
            continue;
        }
        int z1 = ((t2 + t3) * 181) >> 8;
        r[2] = (int16_t)(t3 + z1);
        r[6] = (int16_t)(t3 - z1);
        int t4 = d2 + d3, t5 = d1 + d2, t6 = d0 + d1;
        int z5 = ((t4 - t6) * 98) >> 8;
        int z2 = ((t4 * 139) >> 8) + z5;
        int z4 = ((t6 * 334) >> 8) + z5;
        int z3 = (t5 * 181) >> 8;
        int z11 = d0 + z3, z13 = d0 - z3;
        r[5] = (int16_t)(z13 + z2);
        r[3] = (int16_t)(z13 - z2);
        r[1] = (int16_t)(z11 + z4);
        r[7] = (int16_t)(z11 - z4);
    }
    for (int i = 0; i < 8; i++) {
        int16_t* r = blk + i;
        int d3 = r[24] - r[32], d2 = r[16] - r[40], d1 = r[8] - r[48], d0 = r[0] - r[56];
        int s0 = r[0] + r[56], s1 = r[8] + r[48], s2 = r[16] + r[40], s3 = r[24] + r[32];
        if ((d0 | d1 | d2 | d3 | s0 | s1 | s2 | s3) == 0) {
            r[0] = r[8] = r[16] = r[24] = r[32] = r[40] = r[48] = r[56] = 0;
            continue;
        }
        int t0 = s0 + s3, t3 = s0 - s3, t1 = s1 + s2, t2 = s1 - s2;
        r[0]  = (int16_t)((t0 + t1) >> 3);
        r[32] = (int16_t)((t0 - t1) >> 3);
        int z1 = ((t2 + t3) * 181) >> 8;
        r[16] = (int16_t)((t3 + z1) >> 3);
        r[48] = (int16_t)((t3 - z1) >> 3);
        int t4 = d2 + d3, t5 = d1 + d2, t6 = d0 + d1;
        int z5 = ((t4 - t6) * 98) >> 8;
        int z2 = ((t4 * 139) >> 8) + z5;
        int z4 = ((t6 * 334) >> 8) + z5;
        int z3 = (t5 * 181) >> 8;
        int z11 = d0 + z3, z13 = d0 - z3;
        r[40] = (int16_t)((z13 + z2) >> 3);
        r[24] = (int16_t)((z13 - z2) >> 3);
        r[8]  = (int16_t)((z11 + z4) >> 3);
        r[56] = (int16_t)((z11 - z4) >> 3);
    }
}

static inline void quantize_block_fast(int16_t* blk, const uint16_t* inv_q_zz, JpegQuantBlock* qb) {
    fdct_1d(blk);
    int c0 = blk[0];
    int s0 = c0 < 0;
    int q0 = (((s0 ? -c0 : c0) * inv_q_zz[0]) + 32768) >> 16;
    qb->q[0] = (int16_t)(s0 ? -q0 : q0);
    int last_nz = 0;
    for (int i = 1; i < 64; i++) {
        int coeff = blk[ZIGZAG[i]];
        if (coeff == 0) continue;
        int sign = coeff < 0;
        int abs_c = sign ? -coeff : coeff;
        int q_val = (abs_c * inv_q_zz[i] + 32768) >> 16;
        if (q_val != 0) {
            for (int z = last_nz + 1; z < i; z++) qb->q[z] = 0;
            if (q_val > 1023) q_val = 1023;
            qb->q[i] = (int16_t)(sign ? -q_val : q_val);
            last_nz = i;
        }
    }
    qb->last_nz = (uint8_t)last_nz;
}

static inline void emit_quant_block(BitWriter* bw, const JpegQuantBlock* qb,
                                    const uint16_t* dc_codes, const uint8_t* dc_sizes,
                                    const uint16_t* ac_codes, const uint8_t* ac_sizes, int* last_dc) {
    uint32_t buf = bw->buf;
    int bits = bw->bits, opos = bw->opos;
    uint8_t* obuf = bw->obuf;
    #define BW_EMIT(code, len) do { \
        buf = (buf << (len)) | (uint32_t)(code); bits += (len); \
        while (bits >= 8) { bits -= 8; uint8_t _b = (uint8_t)(buf >> bits); obuf[opos++] = _b; if (_b == 0xFF) obuf[opos++] = 0x00; } \
    } while (0)

    int diff = qb->q[0] - *last_dc;
    *last_dc = qb->q[0];
    if (diff > 2047) diff = 2047; else if (diff < -2047) diff = -2047;
    int cat, abits;
    get_cat_bits(diff, &cat, &abits);
    int dlen = dc_sizes[cat];
    if (cat == 0) { BW_EMIT(dc_codes[0], dlen); }
    else if (dlen + cat <= 24) { BW_EMIT(((uint32_t)dc_codes[cat] << cat) | (uint32_t)abits, dlen + cat); }
    else { BW_EMIT(dc_codes[cat], dlen); BW_EMIT(abits, cat); }

    int r = 0, last_nz = qb->last_nz;
    for (int k = 1; k <= last_nz; k++) {
        int val = qb->q[k];
        if (val == 0) { r++; } else {
            while (r > 15) { BW_EMIT(ac_codes[0xF0], ac_sizes[0xF0]); r -= 16; }
            get_cat_bits(val, &cat, &abits);
            int sym = (r << 4) | cat, alen = ac_sizes[sym];
            if (alen + cat <= 24) { BW_EMIT(((uint32_t)ac_codes[sym] << cat) | (uint32_t)abits, alen + cat); }
            else { BW_EMIT(ac_codes[sym], alen); BW_EMIT(abits, cat); }
            r = 0;
        }
    }
    if (last_nz < 63) BW_EMIT(ac_codes[0x00], ac_sizes[0x00]);
    #undef BW_EMIT
    if (opos >= 65000) { fwrite(obuf, 1, opos, bw->fp); opos = 0; }
    bw->buf = buf; bw->bits = bits; bw->opos = opos;
}

static void encode_block_fast(BitWriter* bw, int16_t* blk, const uint16_t* inv_q,
                              const uint16_t* dc_codes, const uint8_t* dc_sizes,
                              const uint16_t* ac_codes, const uint8_t* ac_sizes, int* last_dc) {
    JpegQuantBlock qb;
    quantize_block_fast(blk, inv_q, &qb);
    emit_quant_block(bw, &qb, dc_codes, dc_sizes, ac_codes, ac_sizes, last_dc);
}

static void compute_quant_tables(int quality, uint8_t* q_luma, uint8_t* q_chroma, uint16_t* inv_q_luma_zz, uint16_t* inv_q_chroma_zz) {
    int q = quality < 1 ? 1 : (quality > 100 ? 100 : quality);
    int s = (q < 50) ? (5000 / q) : (200 - q * 2);
    for (int i = 0; i < 64; i++) {
        int vl = (BASE_Q_LUMA[i] * s + 50) / 100;
        int vc = (BASE_Q_CHROMA[i] * s + 50) / 100;
        q_luma[i] = (uint8_t)(vl < 1 ? 1 : (vl > 255 ? 255 : vl));
        q_chroma[i] = (uint8_t)(vc < 1 ? 1 : (vc > 255 ? 255 : vc));
    }
    for (int i = 0; i < 64; i++) {
        int nat = ZIGZAG[i];
        uint32_t aan_num = (uint32_t)AAN_IFACT[nat >> 3] * (uint32_t)AAN_IFACT[nat & 7];
        uint32_t il = (aan_num + ((uint32_t)q_luma[nat] << 7)) / ((uint32_t)q_luma[nat] << 8);
        uint32_t ic = (aan_num + ((uint32_t)q_chroma[nat] << 7)) / ((uint32_t)q_chroma[nat] << 8);
        inv_q_luma_zz[i] = (uint16_t)(il > 65535U ? 65535U : il);
        inv_q_chroma_zz[i] = (uint16_t)(ic > 65535U ? 65535U : ic);
    }
}

static void write_headers(FILE* fp, int width, int height, uint8_t* q_luma, uint8_t* q_chroma,
                          const uint8_t* exif_app1, size_t exif_len) {
    static const uint8_t APP0_HDR[] = { 0xFF,0xD8, 0xFF,0xE0, 0,16, 'J','F','I','F',0, 1,1, 0, 0,1, 0,1, 0,0 };
    fwrite(APP0_HDR, 1, sizeof(APP0_HDR), fp);
    if (exif_app1 != NULL && exif_len >= 4 && exif_app1[0] == 0xFF && exif_app1[1] == 0xE1) {
        fwrite(exif_app1, 1, exif_len, fp);
    }
    fputc(0xFF, fp); fputc(0xDB, fp); fputc(0x00, fp); fputc(0x84, fp); fputc(0x00, fp);
    for (int i = 0; i < 64; i++) fputc(q_luma[ZIGZAG[i]], fp);
    fputc(0x01, fp);
    for (int i = 0; i < 64; i++) fputc(q_chroma[ZIGZAG[i]], fp);

    uint8_t sof0[] = { 0xFF,0xC0, 0,17, 8, (uint8_t)(height >> 8), (uint8_t)height,
                       (uint8_t)(width >> 8), (uint8_t)width, 3, 1,0x22,0, 2,0x11,1, 3,0x11,1 };
    fwrite(sof0, 1, sizeof(sof0), fp);

    fputc(0xFF, fp); fputc(0xC4, fp);
    uint16_t dht_len = (uint16_t)(sizeof(DHT_MARKER_DATA) + 2);
    fputc((uint8_t)(dht_len >> 8), fp); fputc((uint8_t)dht_len, fp);
    fwrite(DHT_MARKER_DATA, 1, sizeof(DHT_MARKER_DATA), fp);

    static const uint8_t SOS_HDR[] = { 0xFF,0xDA, 0,12, 3, 1,0x00, 2,0x11, 3,0x11, 0,63,0 };
    fwrite(SOS_HDR, 1, sizeof(SOS_HDR), fp);
}

int native_write_jpeg_rgb565(const char* filename, const uint16_t* rgb565, int width, int height, int quality) {
    if (!filename || !rgb565 || width <= 0 || height <= 0) return 0;
    FILE* fp = fopen(filename, "wb");
    if (!fp) return 0;

    uint8_t q_luma[64], q_chroma[64];
    uint16_t inv_q_luma[64], inv_q_chroma[64];
    compute_quant_tables(quality, q_luma, q_chroma, inv_q_luma, inv_q_chroma);

    uint16_t dc_luma_c[256], ac_luma_c[256], dc_chroma_c[256], ac_chroma_c[256];
    uint8_t dc_luma_s[256], ac_luma_s[256], dc_chroma_s[256], ac_chroma_s[256];
    memset(dc_luma_s, 0, sizeof(dc_luma_s)); memset(ac_luma_s, 0, sizeof(ac_luma_s));
    memset(dc_chroma_s, 0, sizeof(dc_chroma_s)); memset(ac_chroma_s, 0, sizeof(ac_chroma_s));

    build_huff_table(&DHT_MARKER_DATA[1], &DHT_MARKER_DATA[17], dc_luma_c, dc_luma_s);
    build_huff_table(&DHT_MARKER_DATA[30], &DHT_MARKER_DATA[46], ac_luma_c, ac_luma_s);
    build_huff_table(&DHT_MARKER_DATA[209], &DHT_MARKER_DATA[225], dc_chroma_c, dc_chroma_s);
    build_huff_table(&DHT_MARKER_DATA[238], &DHT_MARKER_DATA[254], ac_chroma_c, ac_chroma_s);

    write_headers(fp, width, height, q_luma, q_chroma, NULL, 0);

    BitWriter* bw = (BitWriter*)calloc(1, sizeof(BitWriter));
    if (!bw) { fclose(fp); return 0; }
    bw->fp = fp;
    int last_dc[3] = { 0, 0, 0 };
    int16_t y_block[4][64], cb_block[64], cr_block[64];

    for (int my = 0; my < height; my += 16) {
        for (int mx = 0; mx < width; mx += 16) {
            for (int by = 0; by < 16; by++) {
                int py = my + by;
                if (py >= height) py = height - 1;
                const uint16_t* row = &rgb565[py * width];
                int b_idx = ((by >= 8) ? 2 : 0), sub_y = by & 7;

                for (int bx = 0; bx < 16; bx++) {
                    int px_idx = mx + bx;
                    if (px_idx >= width) px_idx = width - 1;
                    uint16_t px = row[px_idx];

                    int r = (((px >> 11) & 0x1F) * 527 + 23) >> 6;
                    int g = (((px >> 5) & 0x3F) * 259 + 33) >> 6;
                    int b = ((px & 0x1F) * 527 + 23) >> 6;

                    int y_val = (19595 * r + 38470 * g + 7471 * b + 32768) >> 16;
                    int blk = b_idx + ((bx >= 8) ? 1 : 0), sub_x = bx & 7;
                    y_block[blk][sub_y * 8 + sub_x] = (int16_t)(y_val - 128);

                    if ((by & 1) == 0 && (bx & 1) == 0) {
                        int cb_val = (-11059 * r - 21709 * g + 32768 * b + 32768) >> 16;
                        int cr_val = (32768 * r - 27439 * g - 5329 * b + 32768) >> 16;
                        int c_idx = (by >> 1) * 8 + (bx >> 1);
                        cb_block[c_idx] = (int16_t)cb_val;
                        cr_block[c_idx] = (int16_t)cr_val;
                    }
                }
            }
            for (int i = 0; i < 4; i++) encode_block_fast(bw, y_block[i], inv_q_luma, dc_luma_c, dc_luma_s, ac_luma_c, ac_luma_s, &last_dc[0]);
            encode_block_fast(bw, cb_block, inv_q_chroma, dc_chroma_c, dc_chroma_s, ac_chroma_c, ac_chroma_s, &last_dc[1]);
            encode_block_fast(bw, cr_block, inv_q_chroma, dc_chroma_c, dc_chroma_s, ac_chroma_c, ac_chroma_s, &last_dc[2]);
        }
    }
    bw_flush(bw);
    free(bw);
    fputc(0xFF, fp); fputc(0xD9, fp);
    fclose(fp);
    return 1;
}

// Stream state definition matching native_jpeg_writer.h
struct JpegStreamState {
    FILE* fp;
    int width, height, quality;
    uint8_t q_luma[64], q_chroma[64];
    uint16_t inv_q_luma[64], inv_q_chroma[64];
    uint16_t dc_luma_c[256], ac_luma_c[256], dc_chroma_c[256], ac_chroma_c[256];
    uint8_t dc_luma_s[256], ac_luma_s[256], dc_chroma_s[256], ac_chroma_s[256];
    BitWriter bw;
    int last_dc[3];
};

JpegStreamState* native_jpeg_stream_open_with_exif(const char* filename, int width, int height, int quality,
                                                   const uint8_t* exif_app1, size_t exif_len) {
    if (!filename || width <= 0 || height <= 0) return NULL;
    FILE* fp = fopen(filename, "wb");
    if (!fp) return NULL;
    JpegStreamState* state = (JpegStreamState*)calloc(1, sizeof(JpegStreamState));
    if (!state) { fclose(fp); return NULL; }
    state->fp = fp; state->width = width; state->height = height; state->quality = quality;
    init_ycbcr_tables();
    compute_quant_tables(quality, state->q_luma, state->q_chroma, state->inv_q_luma, state->inv_q_chroma);
    build_huff_table(&DHT_MARKER_DATA[1], &DHT_MARKER_DATA[17], state->dc_luma_c, state->dc_luma_s);
    build_huff_table(&DHT_MARKER_DATA[30], &DHT_MARKER_DATA[46], state->ac_luma_c, state->ac_luma_s);
    build_huff_table(&DHT_MARKER_DATA[209], &DHT_MARKER_DATA[225], state->dc_chroma_c, state->dc_chroma_s);
    build_huff_table(&DHT_MARKER_DATA[238], &DHT_MARKER_DATA[254], state->ac_chroma_c, state->ac_chroma_s);
    write_headers(fp, width, height, state->q_luma, state->q_chroma, exif_app1, exif_len);
    state->bw.fp = fp;
    return state;
}

JpegStreamState* native_jpeg_stream_open(const char* filename, int width, int height, int quality) {
    return native_jpeg_stream_open_with_exif(filename, width, height, quality, NULL, 0);
}

int native_jpeg_get_stream_width(const JpegStreamState* state) { return state ? state->width : 0; }
int native_jpeg_get_stream_height(const JpegStreamState* state) { return state ? state->height : 0; }

int native_jpeg_quantize_mcu_row_rgb565(const JpegStreamState* state, const uint16_t* strip_pixels,
                                        int strip_w, int strip_h, int row_offset, int my, JpegQuantBlock* out_blocks) {
    if (!state || !strip_pixels || !out_blocks || strip_w != state->width) return 0;
    int16_t y_block[4][64], cb_block[64], cr_block[64];
    JpegQuantBlock* qb = out_blocks;
    for (int mx = 0; mx < state->width; mx += 16) {
        for (int by = 0; by < 16; by++) {
            int local_y = my + by;
            if (local_y >= strip_h) local_y = strip_h - 1;
            int global_y = row_offset + local_y;
            if (global_y >= state->height) {
                local_y = (state->height - 1) - row_offset;
                if (local_y < 0) local_y = 0;
                if (local_y >= strip_h) local_y = strip_h - 1;
            }
            const uint16_t* row = &strip_pixels[local_y * strip_w];
            int b_idx = ((by >= 8) ? 2 : 0), sub_y = by & 7;
            for (int bx = 0; bx < 16; bx++) {
                int px_idx = mx + bx;
                if (px_idx >= state->width) px_idx = state->width - 1;
                uint16_t px = row[px_idx];
                int r = (((px >> 11) & 0x1F) * 527 + 23) >> 6;
                int g = (((px >> 5) & 0x3F) * 259 + 33) >> 6;
                int b = ((px & 0x1F) * 527 + 23) >> 6;
                int blk = b_idx + ((bx >= 8) ? 1 : 0);
                y_block[blk][sub_y * 8 + (bx & 7)] = (int16_t)(((19595 * r + 38470 * g + 7471 * b + 32768) >> 16) - 128);
                if ((by & 1) == 0 && (bx & 1) == 0) {
                    int c_idx = (by >> 1) * 8 + (bx >> 1);
                    cb_block[c_idx] = (int16_t)((-11059 * r - 21709 * g + 32768 * b + 32768) >> 16);
                    cr_block[c_idx] = (int16_t)((32768 * r - 27439 * g - 5329 * b + 32768) >> 16);
                }
            }
        }
        for (int i = 0; i < 4; i++) quantize_block_fast(y_block[i], state->inv_q_luma, qb++);
        quantize_block_fast(cb_block, state->inv_q_chroma, qb++);
        quantize_block_fast(cr_block, state->inv_q_chroma, qb++);
    }
    return 1;
}

int native_jpeg_quantize_mcu_row_argb8888(const JpegStreamState* state, const uint32_t* strip_pixels,
                                          int strip_w, int strip_h, int row_offset, int my, JpegQuantBlock* out_blocks) {
    if (!state || !strip_pixels || !out_blocks || strip_w != state->width) return 0;
    init_ycbcr_tables();
    int16_t y_block[4][64], cb_block[64], cr_block[64];
    int global_mcu_y = row_offset + my;
    int full_y = (my + 16 <= strip_h) && (global_mcu_y + 16 <= state->height);
    JpegQuantBlock* qb = out_blocks;

    for (int mx = 0; mx < state->width; mx += 16) {
        if (full_y && mx + 16 <= state->width) {
            for (int by = 0; by < 16; by++) {
                const uint32_t* row = &strip_pixels[(my + by) * strip_w + mx];
                int b_idx = (by >= 8) ? 2 : 0, sub_y8 = (by & 7) * 8;
                int16_t* yl = &y_block[b_idx][sub_y8];
                int16_t* yr = &y_block[b_idx + 1][sub_y8];
                if ((by & 1) == 0) {
                    int c_row = (by >> 1) * 8;
                    for (int bx = 0; bx < 16; bx += 2) {
                        uint32_t p0 = row[bx], p1 = row[bx + 1];
                        int r0 = p0 & 0xFF, g0 = (p0 >> 8) & 0xFF, b0 = (p0 >> 16) & 0xFF;
                        int r1 = p1 & 0xFF, g1 = (p1 >> 8) & 0xFF, b1 = (p1 >> 16) & 0xFF;
                        int16_t* dst = (bx < 8) ? yl : yr;
                        int sx = bx & 7;
                        dst[sx]     = (int16_t)(((g_yr[r0] + g_yg[g0] + g_yb[b0]) >> 16) - 128);
                        dst[sx + 1] = (int16_t)(((g_yr[r1] + g_yg[g1] + g_yb[b1]) >> 16) - 128);
                        int c_idx = c_row + (bx >> 1);
                        cb_block[c_idx] = (int16_t)((g_cbr[r0] + g_cbg[g0] + g_cbb[b0]) >> 16);
                        cr_block[c_idx] = (int16_t)((g_crr[r0] + g_crg[g0] + g_crb[b0]) >> 16);
                    }
                } else {
                    for (int bx = 0; bx < 8; bx++) {
                        uint32_t p0 = row[bx], p1 = row[bx + 8];
                        yl[bx] = (int16_t)(((g_yr[p0 & 0xFF] + g_yg[(p0 >> 8) & 0xFF] + g_yb[(p0 >> 16) & 0xFF]) >> 16) - 128);
                        yr[bx] = (int16_t)(((g_yr[p1 & 0xFF] + g_yg[(p1 >> 8) & 0xFF] + g_yb[(p1 >> 16) & 0xFF]) >> 16) - 128);
                    }
                }
            }
        } else {
            for (int by = 0; by < 16; by++) {
                int local_y = my + by;
                if (local_y >= strip_h) local_y = strip_h - 1;
                int global_y = row_offset + local_y;
                if (global_y >= state->height) {
                    local_y = (state->height - 1) - row_offset;
                    if (local_y < 0) local_y = 0;
                    if (local_y >= strip_h) local_y = strip_h - 1;
                }
                const uint32_t* row = &strip_pixels[local_y * strip_w];
                int b_idx = ((by >= 8) ? 2 : 0), sub_y = by & 7;
                for (int bx = 0; bx < 16; bx++) {
                    int px_idx = mx + bx;
                    if (px_idx >= state->width) px_idx = state->width - 1;
                    uint32_t px = row[px_idx];
                    int r = px & 0xFF, g = (px >> 8) & 0xFF, b = (px >> 16) & 0xFF;
                    int blk = b_idx + ((bx >= 8) ? 1 : 0);
                    y_block[blk][sub_y * 8 + (bx & 7)] = (int16_t)(((g_yr[r] + g_yg[g] + g_yb[b]) >> 16) - 128);
                    if ((by & 1) == 0 && (bx & 1) == 0) {
                        int c_idx = (by >> 1) * 8 + (bx >> 1);
                        cb_block[c_idx] = (int16_t)((g_cbr[r] + g_cbg[g] + g_cbb[b]) >> 16);
                        cr_block[c_idx] = (int16_t)((g_crr[r] + g_crg[g] + g_crb[b]) >> 16);
                    }
                }
            }
        }
        for (int i = 0; i < 4; i++) quantize_block_fast(y_block[i], state->inv_q_luma, qb++);
        quantize_block_fast(cb_block, state->inv_q_chroma, qb++);
        quantize_block_fast(cr_block, state->inv_q_chroma, qb++);
    }
    return 1;
}

int native_jpeg_quantize_ycbcr_mcu(const JpegStreamState* state, int16_t y_blk[4][64],
                                   int16_t cb_blk[64], int16_t cr_blk[64], JpegQuantBlock* out_blocks) {
    if (!state || !out_blocks) return 0;
    for (int i = 0; i < 4; i++) quantize_block_fast(y_blk[i], state->inv_q_luma, out_blocks + i);
    quantize_block_fast(cb_blk, state->inv_q_chroma, out_blocks + 4);
    quantize_block_fast(cr_blk, state->inv_q_chroma, out_blocks + 5);
    return 1;
}

int native_jpeg_emit_quantized_blocks(JpegStreamState* state, const JpegQuantBlock* blocks, int num_mcus) {
    if (!state || !state->fp || !blocks || num_mcus < 0) return 0;
    const JpegQuantBlock* qb = blocks;
    for (int m = 0; m < num_mcus; m++) {
        for (int i = 0; i < 4; i++) {
            emit_quant_block(&state->bw, qb++, state->dc_luma_c, state->dc_luma_s,
                             state->ac_luma_c, state->ac_luma_s, &state->last_dc[0]);
        }
        emit_quant_block(&state->bw, qb++, state->dc_chroma_c, state->dc_chroma_s,
                         state->ac_chroma_c, state->ac_chroma_s, &state->last_dc[1]);
        emit_quant_block(&state->bw, qb++, state->dc_chroma_c, state->dc_chroma_s,
                         state->ac_chroma_c, state->ac_chroma_s, &state->last_dc[2]);
    }
    return 1;
}

int native_jpeg_stream_write_strip_rgb565(JpegStreamState* state, const uint16_t* strip_pixels, int strip_w, int strip_h, int row_offset) {
    if (!state || !state->fp || !strip_pixels || strip_w <= 0 || strip_h <= 0 || strip_w != state->width) return 0;
    int mcus_x = (state->width + 15) >> 4;
    JpegQuantBlock* row_buf = (JpegQuantBlock*)malloc((size_t)mcus_x * 6U * sizeof(JpegQuantBlock));
    if (!row_buf) return 0;
    for (int my = 0; my < strip_h; my += 16) {
        if (row_offset + my >= state->height) break;
        native_jpeg_quantize_mcu_row_rgb565(state, strip_pixels, strip_w, strip_h, row_offset, my, row_buf);
        native_jpeg_emit_quantized_blocks(state, row_buf, mcus_x);
    }
    free(row_buf);
    return 1;
}

int native_write_jpeg_strip(JpegStreamState* state, const uint16_t* strip_pixels, int strip_w, int strip_h, int row_offset) {
    return native_jpeg_stream_write_strip_rgb565(state, strip_pixels, strip_w, strip_h, row_offset);
}

int native_jpeg_stream_write_strip_argb8888(JpegStreamState* state, const uint32_t* strip_pixels, int strip_w, int strip_h, int row_offset) {
    if (!state || !state->fp || !strip_pixels || strip_w <= 0 || strip_h <= 0 || strip_w != state->width) return 0;
    int mcus_x = (state->width + 15) >> 4;
    JpegQuantBlock* row_buf = (JpegQuantBlock*)malloc((size_t)mcus_x * 6U * sizeof(JpegQuantBlock));
    if (!row_buf) return 0;
    for (int my = 0; my < strip_h; my += 16) {
        if (row_offset + my >= state->height) break;
        native_jpeg_quantize_mcu_row_argb8888(state, strip_pixels, strip_w, strip_h, row_offset, my, row_buf);
        native_jpeg_emit_quantized_blocks(state, row_buf, mcus_x);
    }
    free(row_buf);
    return 1;
}

int native_write_jpeg_strip_argb8888(JpegStreamState* state, const uint32_t* strip_pixels, int strip_w, int strip_h, int row_offset) {
    return native_jpeg_stream_write_strip_argb8888(state, strip_pixels, strip_w, strip_h, row_offset);
}

int native_process_image_stream_fast(const char* input_jpeg_path, const char* output_jpeg_path, int grain_strength, int clarity_strength) {
    if (!input_jpeg_path || !output_jpeg_path) return 0;
    // Pure C fast-path hook for hardware libjpeg decompressor symbols.
    // If unavailable or running on host test machines, returns 0 to allow BitmapRegionDecoder fallback.
    return 0;
}

int native_jpeg_stream_close(JpegStreamState* state, int success) {
    if (!state) return 0;
    int ret = 0;
    if (state->fp) {
        if (success) {
            bw_flush(&state->bw);
            fputc(0xFF, state->fp);
            fputc(0xD9, state->fp);
            fflush(state->fp);
            ret = 1;
        }
        fclose(state->fp);
        state->fp = NULL;
    }
    free(state);
    return ret;
}
