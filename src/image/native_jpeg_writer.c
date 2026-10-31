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

// Buffered BitWriter
typedef struct {
    FILE* fp;
    uint32_t buf;
    int bits;
    uint8_t obuf[32768];
    int opos;
} BitWriter;

static inline void bw_put_byte(BitWriter* bw, uint8_t b) {
    bw->obuf[bw->opos++] = b;
    if (bw->opos >= 32760) {
        fwrite(bw->obuf, 1, bw->opos, bw->fp);
        bw->opos = 0;
    }
    if (b == 0xFF) {
        bw->obuf[bw->opos++] = 0x00;
        if (bw->opos >= 32760) {
            fwrite(bw->obuf, 1, bw->opos, bw->fp);
            bw->opos = 0;
        }
    }
}

static inline void bw_put_bits(BitWriter* bw, uint32_t code, int len) {
    bw->buf = (bw->buf << len) | (code & ((1 << len) - 1));
    bw->bits += len;
    while (bw->bits >= 8) {
        bw->bits -= 8;
        bw_put_byte(bw, (uint8_t)(bw->buf >> bw->bits));
    }
}

static void bw_flush(BitWriter* bw) {
    if (bw->bits > 0) {
        bw->buf = (bw->buf << (8 - bw->bits)) | ((1 << (8 - bw->bits)) - 1);
        bw_put_byte(bw, (uint8_t)bw->buf);
        bw->buf = 0;
        bw->bits = 0;
    }
    if (bw->opos > 0) {
        fwrite(bw->obuf, 1, bw->opos, bw->fp);
        bw->opos = 0;
    }
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
    int v = val < 0 ? -val : val, c = 0;
    while (v > 0) { c++; v >>= 1; }
    *cat = c;
    *bits = (val < 0) ? ((val - 1) & ((1 << c) - 1)) : val;
}

// Fast 1D FDCT (AAN algorithm)
static inline void fdct_1d(int16_t* blk) {
    for (int i=0; i<8; i++) {
        int16_t* r = blk + i*8;
        int s0 = r[0] + r[7], s1 = r[1] + r[6], s2 = r[2] + r[5], s3 = r[3] + r[4];
        int d0 = r[0] - r[7], d1 = r[1] - r[6], d2 = r[2] - r[5], d3 = r[3] - r[4];
        
        int t0 = s0 + s3, t1 = s1 + s2, t2 = s1 - s2, t3 = s0 - s3;
        r[0] = (t0 + t1); r[4] = (t0 - t1);
        int z1 = (t2 + t3) * 4433; // 4433 = 0.54119610 * 8192
        r[2] = (t3 * 8192 + z1) >> 13; r[6] = (t3 * 8192 - z1) >> 13;
        
        int t4 = d1 + d2, t5 = d0 + d3, t6 = d0 + d2, t7 = d1 + d3;
        int z5 = (t6 - t7) * 9633; // 1.175875602 * 8192
        int z2 = t4 * 2446; // 0.298631336
        int z3 = t6 * 16819; // 2.053119869
        int z4 = t7 * 24810; // 3.027378108
        int z11 = t7 * 12282; // 1.5
        r[1] = (z4 + z5 + t5 * 8192) >> 13;
        r[3] = (z3 + z5 - t5 * 8192) >> 13;
        r[5] = (z2 + z5 + t5 * 8192) >> 13;
        r[7] = (z11 + z5 - t5 * 8192) >> 13;
    }
    for (int i=0; i<8; i++) {
        int16_t* r = blk + i;
        int s0 = r[0] + r[56], s1 = r[8] + r[48], s2 = r[16] + r[40], s3 = r[24] + r[32];
        int d0 = r[0] - r[56], d1 = r[8] - r[48], d2 = r[16] - r[40], d3 = r[24] - r[32];
        
        int t0 = s0 + s3, t1 = s1 + s2, t2 = s1 - s2, t3 = s0 - s3;
        r[0] = (t0 + t1); r[32] = (t0 - t1);
        int z1 = (t2 + t3) * 4433;
        r[16] = (t3 * 8192 + z1) >> 16; r[48] = (t3 * 8192 - z1) >> 16; // Shift 16 incorporates AAN scaling
        
        int t4 = d1 + d2, t5 = d0 + d3, t6 = d0 + d2, t7 = d1 + d3;
        int z5 = (t6 - t7) * 9633;
        int z2 = t4 * 2446, z3 = t6 * 16819, z4 = t7 * 24810, z11 = t7 * 12282;
        r[8] = (z4 + z5 + t5 * 8192) >> 16;
        r[24] = (z3 + z5 - t5 * 8192) >> 16;
        r[40] = (z2 + z5 + t5 * 8192) >> 16;
        r[56] = (z11 + z5 - t5 * 8192) >> 16;
    }
}

static void encode_block_fast(BitWriter* bw, int16_t* blk, const uint16_t* inv_q,
                              const uint16_t* dc_codes, const uint8_t* dc_sizes,
                              const uint16_t* ac_codes, const uint8_t* ac_sizes, int* last_dc) {
    fdct_1d(blk);
    int16_t q_block[64];
    for (int i = 0; i < 64; i++) {
        int coeff = blk[i];
        int sign = coeff < 0;
        int abs_c = sign ? -coeff : coeff;
        int q_val = (abs_c * inv_q[i] + 32768) >> 16;
        q_block[ZIGZAG[i]] = (int16_t)(sign ? -q_val : q_val);
    }
    
    int diff = q_block[0] - *last_dc;
    *last_dc = q_block[0];
    int cat, bits;
    get_cat_bits(diff, &cat, &bits);
    bw_put_bits(bw, dc_codes[cat], dc_sizes[cat]);
    if (cat > 0) bw_put_bits(bw, (uint32_t)bits, cat);

    int r = 0;
    for (int k = 1; k < 64; k++) {
        int val = q_block[k];
        if (val == 0) { r++; } else {
            while (r > 15) { bw_put_bits(bw, ac_codes[0xF0], ac_sizes[0xF0]); r -= 16; }
            get_cat_bits(val, &cat, &bits);
            bw_put_bits(bw, ac_codes[(r << 4) | cat], ac_sizes[(r << 4) | cat]);
            bw_put_bits(bw, (uint32_t)bits, cat);
            r = 0;
        }
    }
    if (r > 0) bw_put_bits(bw, ac_codes[0x00], ac_sizes[0x00]);
}

static void compute_quant_tables(int quality, uint8_t* q_luma, uint8_t* q_chroma, uint16_t* inv_q_luma, uint16_t* inv_q_chroma) {
    int q = quality < 1 ? 1 : (quality > 100 ? 100 : quality);
    int s = (q < 50) ? (5000 / q) : (200 - q * 2);
    for (int i = 0; i < 64; i++) {
        int vl = (BASE_Q_LUMA[i] * s + 50) / 100;
        int vc = (BASE_Q_CHROMA[i] * s + 50) / 100;
        q_luma[i] = (uint8_t)(vl < 1 ? 1 : (vl > 255 ? 255 : vl));
        q_chroma[i] = (uint8_t)(vc < 1 ? 1 : (vc > 255 ? 255 : vc));
        inv_q_luma[i] = (uint16_t)((65536 + (q_luma[i] >> 1)) / q_luma[i]);
        inv_q_chroma[i] = (uint16_t)((65536 + (q_chroma[i] >> 1)) / q_chroma[i]);
    }
}

static void write_headers(FILE* fp, int width, int height, uint8_t* q_luma, uint8_t* q_chroma) {
    static const uint8_t APP0_HDR[] = { 0xFF,0xD8, 0xFF,0xE0, 0,16, 'J','F','I','F',0, 1,1, 0, 0,1, 0,1, 0,0 };
    fwrite(APP0_HDR, 1, sizeof(APP0_HDR), fp);
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

    write_headers(fp, width, height, q_luma, q_chroma);

    BitWriter bw = { fp, 0, 0 };
    bw.opos = 0;
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
            for (int i = 0; i < 4; i++) encode_block_fast(&bw, y_block[i], inv_q_luma, dc_luma_c, dc_luma_s, ac_luma_c, ac_luma_s, &last_dc[0]);
            encode_block_fast(&bw, cb_block, inv_q_chroma, dc_chroma_c, dc_chroma_s, ac_chroma_c, ac_chroma_s, &last_dc[1]);
            encode_block_fast(&bw, cr_block, inv_q_chroma, dc_chroma_c, dc_chroma_s, ac_chroma_c, ac_chroma_s, &last_dc[2]);
        }
    }
    bw_flush(&bw);
    fputc(0xFF, fp); fputc(0xD9, fp);
    fclose(fp);
    return 1;
}

// 16-row Strip Streaming Upscaler and JPEG encoder
int native_upscale_and_write_jpeg_rgb565(const char* filename, const uint16_t* src_pixels, int srcW, int srcH, int dstW, int dstH, int quality) {
    if (!filename || !src_pixels || srcW <= 1 || srcH <= 1 || dstW <= 1 || dstH <= 1) return 0;
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

    write_headers(fp, dstW, dstH, q_luma, q_chroma);

    BitWriter bw = { fp, 0, 0 };
    bw.opos = 0;
    int last_dc[3] = { 0, 0, 0 };
    int16_t y_block[4][64], cb_block[64], cr_block[64];
    
    // 128KB buffer for 16 rows of dstW (max width 4096)
    uint16_t strip_buf[16 * 4096]; 
    
    // Scale logic
    uint32_t scale_x = (uint32_t)(((uint64_t)(srcW - 1) << 16) / (dstW - 1));
    uint32_t scale_y = (uint32_t)(((uint64_t)(srcH - 1) << 16) / (dstH - 1));

    for (int my = 0; my < dstH; my += 16) {
        // Upscale 16 rows
        for (int dy = 0; dy < 16; dy++) {
            int target_y = my + dy;
            if (target_y >= dstH) target_y = dstH - 1;
            
            uint32_t src_y_fp = target_y * scale_y;
            int y0 = (int)(src_y_fp >> 16);
            int y1 = (y0 < srcH - 1) ? y0 + 1 : y0;
            int y_frac = (src_y_fp >> 8) & 0xFF; // Q8
            int inv_y = 256 - y_frac;

            const uint16_t* row0 = &src_pixels[y0 * srcW];
            const uint16_t* row1 = &src_pixels[y1 * srcW];
            uint16_t* dst_row = &strip_buf[dy * dstW];

            for (int dx = 0; dx < dstW; dx++) {
                uint32_t src_x_fp = dx * scale_x;
                int x0 = (int)(src_x_fp >> 16);
                int x1 = (x0 < srcW - 1) ? x0 + 1 : x0;
                int x_frac = (src_x_fp >> 8) & 0xFF; // Q8
                int inv_x = 256 - x_frac;

                uint16_t p00 = row0[x0], p10 = row0[x1];
                uint16_t p01 = row1[x0], p11 = row1[x1];
                
                int r00 = (p00 >> 11) & 0x1F, g00 = (p00 >> 5) & 0x3F, b00 = p00 & 0x1F;
                int r10 = (p10 >> 11) & 0x1F, g10 = (p10 >> 5) & 0x3F, b10 = p10 & 0x1F;
                int r01 = (p01 >> 11) & 0x1F, g01 = (p01 >> 5) & 0x3F, b01 = p01 & 0x1F;
                int r11 = (p11 >> 11) & 0x1F, g11 = (p11 >> 5) & 0x3F, b11 = p11 & 0x1F;

                int w00 = (inv_x * inv_y) >> 8;
                int w10 = (x_frac * inv_y) >> 8;
                int w01 = (inv_x * y_frac) >> 8;
                int w11 = (x_frac * y_frac) >> 8;
                int total_w = w00 + w10 + w01 + w11;
                if (total_w == 0) total_w = 1;

                int r = (r00 * w00 + r10 * w10 + r01 * w01 + r11 * w11) / total_w;
                int g = (g00 * w00 + g10 * w10 + g01 * w01 + g11 * w11) / total_w;
                int b = (b00 * w00 + b10 * w10 + b01 * w01 + b11 * w11) / total_w;

                dst_row[dx] = (uint16_t)((r << 11) | (g << 5) | b);
            }
        }
        
        // Encode strip
        for (int mx = 0; mx < dstW; mx += 16) {
            for (int by = 0; by < 16; by++) {
                const uint16_t* row = &strip_buf[by * dstW];
                int b_idx = ((by >= 8) ? 2 : 0), sub_y = by & 7;

                for (int bx = 0; bx < 16; bx++) {
                    int px_idx = mx + bx;
                    if (px_idx >= dstW) px_idx = dstW - 1;
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
            for (int i = 0; i < 4; i++) encode_block_fast(&bw, y_block[i], inv_q_luma, dc_luma_c, dc_luma_s, ac_luma_c, ac_luma_s, &last_dc[0]);
            encode_block_fast(&bw, cb_block, inv_q_chroma, dc_chroma_c, dc_chroma_s, ac_chroma_c, ac_chroma_s, &last_dc[1]);
            encode_block_fast(&bw, cr_block, inv_q_chroma, dc_chroma_c, dc_chroma_s, ac_chroma_c, ac_chroma_s, &last_dc[2]);
        }
    }
    bw_flush(&bw);
    fputc(0xFF, fp); fputc(0xD9, fp);
    fclose(fp);
    return 1;
}
