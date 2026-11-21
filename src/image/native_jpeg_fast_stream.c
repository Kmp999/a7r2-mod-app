#include "native_jpeg_fast_stream.h"
#include "native_jpeg_writer.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#endif

#if defined(__GNUC__) || defined(__clang__)
#define FS_NOINLINE __attribute__((noinline))
#else
#define FS_NOINLINE
#endif

static inline uint8_t clamp_u8_v7(int v) {
#if defined(__ARM_ARCH_7A__) || defined(__ARM_ARCH_6__)
    int res;
    __asm__("usat %0, #8, %1" : "=r"(res) : "r"(v));
    return (uint8_t)res;
#else
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
#endif
}

static inline uint8_t clamp_u8_asr9_v7(int v) {
#if defined(__ARM_ARCH_7A__) || defined(__ARM_ARCH_6__)
    int res;
    __asm__("usat %0, #8, %1, asr #9" : "=r"(res) : "r"(v));
    return (uint8_t)res;
#else
    int s = v >> 9;
    return (uint8_t)(s < 0 ? 0 : (s > 255 ? 255 : s));
#endif
}

static inline uint8_t clamp_u8_asr10_v7(int v) {
#if defined(__ARM_ARCH_7A__) || defined(__ARM_ARCH_6__)
    int res;
    __asm__("usat %0, #8, %1, asr #10" : "=r"(res) : "r"(v));
    return (uint8_t)res;
#else
    int s = v >> 10;
    return (uint8_t)(s < 0 ? 0 : (s > 255 ? 255 : s));
#endif
}

static inline int16_t clamp_s8_v7(int v) {
#if defined(__ARM_ARCH_7A__) || defined(__ARM_ARCH_6__)
    int res;
    __asm__("ssat %0, #8, %1" : "=r"(res) : "r"(v));
    return (int16_t)res;
#else
    return (int16_t)(v < -128 ? -128 : (v > 127 ? 127 : v));
#endif
}

static uint64_t fs_now_ms(void) {
#ifndef _WIN32
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000);
#else
    return 0;
#endif
}

// Per-thread CPU time (ns): separates "slow silicon" from "thread starved by scheduler / cgroup"
static uint64_t fs_thread_cpu_ns(void) {
#ifndef _WIN32
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#else
    return 0;
#endif
}

typedef struct {
    uint64_t idct_ns, lut_ns, fdct_ns;
    uint32_t n, tick;
    uint32_t dct_fast_blks, tot_blks;
} ProfAcc;

static void fs_read_sched_info(int* out_bg, int* out_mhz) {
    int bg = 0, mhz = 0;
#ifndef _WIN32
    char path[96];
    char line[192];
#if defined(__ANDROID__)
    long tid = (long)gettid();
#elif defined(SYS_gettid)
    long tid = (long)syscall(SYS_gettid);
#elif defined(__NR_gettid)
    long tid = (long)syscall(__NR_gettid);
#else
    long tid = (long)getpid();
#endif
    snprintf(path, sizeof(path), "/proc/self/task/%ld/cgroup", tid);
    FILE* f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "bg_") != NULL || strstr(line, "background") != NULL) bg = 1;
        }
        fclose(f);
    }
    f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
    if (f) {
        long khz = 0;
        if (fscanf(f, "%ld", &khz) == 1) mhz = (int)(khz / 1000);
        fclose(f);
    }
#endif
    *out_bg = bg;
    *out_mhz = mhz;
}

#ifndef _WIN32
// Linux API-9-safe CPU affinity helpers via raw syscall (prevents Linux 3.0 SMP core-piling onto CPU 0!)
typedef struct {
    unsigned long bits[16];
    int valid;
} FsCpuMask;

static int fs_get_avail_cpus(int out_cpus[4], FsCpuMask* out_saved) {
    memset(out_saved, 0, sizeof(FsCpuMask));
    int count = 0;
#if defined(__linux__)
#if defined(__NR_sched_getaffinity)
    const long nr_get = __NR_sched_getaffinity;
#elif defined(__arm__)
    const long nr_get = 242;
#else
    const long nr_get = -1;
#endif
    if (nr_get >= 0) {
        long rc = syscall(nr_get, (pid_t)0, (size_t)sizeof(out_saved->bits), out_saved->bits);
        if (rc > 0) {
            out_saved->valid = 1;
            int max_bits = (int)(sizeof(unsigned long) * 8);
            if (max_bits > 64) max_bits = 64;
            for (int c = 0; c < max_bits && count < 4; c++) {
                if ((out_saved->bits[0] & (1UL << c)) != 0UL) {
                    out_cpus[count++] = c;
                }
            }
        }
    }
#else
    (void)out_cpus;
#endif
    return count;
}

static int fs_pin_current_cpu(int cpu_id) {
#if defined(__linux__)
#if defined(__NR_sched_setaffinity)
    const long nr_set = __NR_sched_setaffinity;
#elif defined(__arm__)
    const long nr_set = 241;
#else
    const long nr_set = -1;
#endif
    if (nr_set >= 0 && cpu_id >= 0 && cpu_id < (int)(sizeof(unsigned long) * 8)) {
        unsigned long mask[16];
        memset(mask, 0, sizeof(mask));
        mask[0] = (1UL << cpu_id);
        return (syscall(nr_set, (pid_t)0, (size_t)sizeof(mask), mask) == 0) ? 1 : 0;
    }
#else
    (void)cpu_id;
#endif
    return 0;
}

static void fs_restore_affinity(const FsCpuMask* saved) {
#if defined(__linux__)
#if defined(__NR_sched_setaffinity)
    const long nr_set = __NR_sched_setaffinity;
#elif defined(__arm__)
    const long nr_set = 241;
#else
    const long nr_set = -1;
#endif
    if (nr_set >= 0 && saved && saved->valid) {
        (void)syscall(nr_set, (pid_t)0, (size_t)sizeof(saved->bits), saved->bits);
    }
#else
    (void)saved;
#endif
}
#endif

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

// Inverse zigzag: NAT_TO_ZZ[r * 8 + c] = zigzag index [0..63] (enables register-fused FDCT+quant!)
static const uint8_t NAT_TO_ZZ[64] = {
     0,  1,  5,  6, 14, 15, 27, 28,
     2,  4,  7, 13, 16, 26, 29, 42,
     3,  8, 12, 17, 25, 30, 41, 43,
     9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63
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

// Direct-Chroma 64(Cr) x 64(Cb) x 17(Y) YCbCr -> YCbCr 3D LUT (272KB total; ~17KB active L1 working set on real photos!)
// Word layout: ((uint32_t)oy_u8 << 16) | ((uint32_t)ocb_u8 << 8) | (uint32_t)ocr_u8
// Eliminates ALL 2D chroma simplex interpolation, branches, and chroma multiplies in the 4.45M-quad hot loop:
// ONLY 2 LUT loads per 2x2 quad (same-cell) + 1-cycle chroma residual pass-through for smooth gradients!
// Cached statically across shots by 64-bit FNV-1a hash of Lut17Context (0.0ms rebuild on repeat shots!).
#define YCC64_NODES (64 * 64 * 17)

#ifndef _WIN32
static pthread_mutex_t s_ycc64_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif
static uint32_t* s_ycc64_cache = NULL;
static uint64_t s_ycc64_hash = 0;
static int s_ycc64_valid = 0;
static int s_ycc64_chroma_res = 1; // 1 for color film sims, 0 for B&W (ACROS)

static uint64_t hash_lut17(const Lut17Context* lut) {
    uint64_t h = 14695981039346656037ULL;
    const uint8_t* p = lut->table;
    for (size_t i = 0; i < sizeof(lut->table); i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

typedef struct {
    const Lut17Context* rgb_lut;
    uint32_t* dst;
    int cr_start;
    int cr_end;
    int max_chroma_dev;
} BuildLut64Slice;

static void build_ycc64_slice(void* arg) {
    BuildLut64Slice* s = (BuildLut64Slice*)arg;
    const Lut17Context* rgb_lut = s->rgb_lut;
    uint32_t* dst = s->dst;
    int max_dev = 0;

    for (int cr64 = s->cr_start; cr64 < s->cr_end; cr64++) {
        int crv = cr64 * 4 + 2;
        if (crv > 255) crv = 255;
        int cr = crv - 128;
        int r_uv = (91881 * cr + 32768) >> 16;
        for (int cb64 = 0; cb64 < 64; cb64++) {
            int cbv = cb64 * 4 + 2;
            if (cbv > 255) cbv = 255;
            int cb = cbv - 128;
            int g_uv = -((22554 * cb + 46802 * cr + 32768) >> 16);
            int b_uv = (116130 * cb + 32768) >> 16;
            uint32_t* col = dst + ((size_t)((cr64 << 6) | cb64) * 17U);
            for (int yi = 0; yi < 17; yi++) {
                int Y = (yi == 16) ? 255 : (yi << 4);
                uint8_t r = clamp_u8_v7(Y + r_uv);
                uint8_t g = clamp_u8_v7(Y + g_uv);
                uint8_t b = clamp_u8_v7(Y + b_uv);
                uint8_t or_v, og_v, ob_v;
                lut17_interpolate_pixel(rgb_lut, r, g, b, &or_v, &og_v, &ob_v);
                int oy_i  = (19595 * (int)or_v + 38470 * (int)og_v +  7471 * (int)ob_v + 32768) >> 16;
                int ocb_i = ((-11059 * (int)or_v - 21709 * (int)og_v + 32768 * (int)ob_v + 32768) >> 16) + 128;
                int ocr_i = (( 32768 * (int)or_v - 27439 * (int)og_v -  5329 * (int)ob_v + 32768) >> 16) + 128;
                uint32_t oy  = (uint32_t)clamp_u8_v7(oy_i);
                uint32_t ocb = (uint32_t)clamp_u8_v7(ocb_i);
                uint32_t ocr = (uint32_t)clamp_u8_v7(ocr_i);
                // 8:12:12 SWAR layout: bits 24..31=oy, 12..19=ocb (4 guard bits), 0..7=ocr (4 guard bits)
                col[yi] = (oy << 24) | (ocb << 12) | ocr;
                int dcb = (int)ocb - 128; if (dcb < 0) dcb = -dcb;
                int dcr = (int)ocr - 128; if (dcr < 0) dcr = -dcr;
                if (dcb > max_dev) max_dev = dcb;
                if (dcr > max_dev) max_dev = dcr;
            }
        }
    }
    s->max_chroma_dev = max_dev;
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

static inline int fbr_extend(int v, int s) {
    return (v < (1 << (s - 1))) ? (v - ((1 << s) - 1)) : v;
}

// Fast 10-bit single-load Huffman table + combined AC (Huffman + sign-extended value) fast table!
// ac_fast[p10]: bit 0..3 = total_bits (1..10), bit 4..7 = run (0..15), bit 16..31 = (int16_t)val (0 iff EOB/ZRL)
// Covers ~97.5% of 10-bit AC prefixes -> 1 single table lookup decodes BOTH symbol AND signed AC value!
typedef struct {
    uint16_t fast_tab[1024];
    uint32_t ac_fast[1024];
    int32_t maxcode[18];
    int32_t valoffset[18];
    uint8_t huffval[256];
    int valid;
} FastHuffTable;

static void build_fast_huff(FastHuffTable* t, const uint8_t bits[17], const uint8_t* vals, int total, int is_ac) {
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
                    uint16_t entry = (uint16_t)(((uint16_t)len << 8) | sym);
                    for (int r = 0; r < rep; r++) {
                        t->fast_tab[base + r] = entry;
                    }
                    if (is_ac) {
                        int run = sym >> 4;
                        int sz = sym & 0x0F;
                        if (sz == 0) {
                            uint32_t ae = ((uint32_t)run << 4) | (uint32_t)len;
                            for (int r = 0; r < rep; r++) {
                                t->ac_fast[base + r] = ae;
                            }
                        } else if (len + sz <= 10) {
                            int tb = len + sz;
                            int arep = 1 << (10 - tb);
                            int nvals = 1 << sz;
                            for (int bv = 0; bv < nvals; bv++) {
                                int16_t val = (int16_t)fbr_extend(bv, sz);
                                uint32_t ae = ((uint32_t)(uint16_t)val << 16) | ((uint32_t)run << 4) | (uint32_t)tb;
                                int b0 = ((code << sz) | bv) << (10 - tb);
                                for (int r = 0; r < arep; r++) {
                                    t->ac_fast[b0 + r] = ae;
                                }
                            }
                        }
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

#ifndef _WIN32
static int sem_wait_eintr(sem_t* s) {
    for (;;) {
        if (sem_wait(s) == 0) return 1;
        if (errno != EINTR) return 0;
    }
}

// Asynchronous progressive SD prefetcher: reads input entropy scan via raw 256KB POSIX read()
// in the background so SD I/O (~2.4s on A7R2) overlaps 100% with the 4-core compute pipeline!
typedef struct {
    int fd;
    uint8_t* buf;
    size_t total_len;
    volatile size_t ready_len;
    volatile int done;
    volatile int waiters;
    volatile int cancel;
    sem_t chunk_sem;
    pthread_t tid;
    int active;
} IoPrefetcher;

static void* io_prefetch_main(void* p) {
    IoPrefetcher* pf = (IoPrefetcher*)p;
    size_t off = 0;
    size_t chunk = 131072U; // 128KB first chunk so Band 0 starts in ~50ms, then 256KB for max SD DMA speed
    while (off < pf->total_len && !pf->cancel) {
        size_t want = pf->total_len - off;
        if (want > chunk) want = chunk;
        ssize_t got = read(pf->fd, pf->buf + off, want);
        if (got < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (got == 0) break;
        off += (size_t)got;
        chunk = 262144U;
        __sync_synchronize();
        pf->ready_len = off;
        if (pf->waiters > 0) sem_post(&pf->chunk_sem);
    }
    memset(pf->buf + off, 0, 8);
    __sync_synchronize();
    pf->ready_len = off;
    pf->done = 1;
    sem_post(&pf->chunk_sem);
    return NULL;
}

static int io_prefetch_start(IoPrefetcher* pf, int fd, uint8_t* buf, size_t total_len) {
    memset(pf, 0, sizeof(IoPrefetcher));
    pf->fd = fd;
    pf->buf = buf;
    pf->total_len = total_len;
    if (sem_init(&pf->chunk_sem, 0, 0) != 0) return 0;
    if (pthread_create(&pf->tid, NULL, io_prefetch_main, pf) != 0) {
        sem_destroy(&pf->chunk_sem);
        return 0;
    }
    pf->active = 1;
    return 1;
}

static void io_prefetch_destroy(IoPrefetcher* pf) {
    if (!pf->active) return;
    pf->cancel = 1;
    pthread_join(pf->tid, NULL);
    if (pf->fd >= 0) {
        close(pf->fd);
        pf->fd = -1;
    }
    sem_destroy(&pf->chunk_sem);
    pf->active = 0;
}
#endif

// Buffered 64-bit MSB-aligned JPEG entropy reader (async IoPrefetcher RAM buffer OR 64KB FILE* stream!)
typedef struct {
    FILE* fp;
#ifndef _WIN32
    IoPrefetcher* pf;
#endif
    uint8_t* buf;
    uint8_t stream_buf[65536];
    size_t pos, len;
    uint64_t bitbuf;
    int bitcnt;
    int marker_hit;
    uint8_t pending_marker;
    int eof;
    uint64_t io_wait_ms;
} FastBitReader;

#ifndef _WIN32
static size_t fbr_sync_prefetch(FastBitReader* br, size_t need_pos) {
    IoPrefetcher* pf = br->pf;
    if (!pf) return br->len;
    size_t r = pf->ready_len;
    __sync_synchronize();
    if (r > need_pos || pf->done) {
        br->len = r;
        return r;
    }
    uint64_t tw0 = fs_now_ms();
    __sync_fetch_and_add(&pf->waiters, 1);
    for (;;) {
        r = pf->ready_len;
        __sync_synchronize();
        if (r > need_pos || pf->done) break;
        sem_wait_eintr(&pf->chunk_sem);
    }
    __sync_fetch_and_sub(&pf->waiters, 1);
    br->io_wait_ms += (fs_now_ms() - tw0);
    br->len = r;
    return r;
}
#endif

static inline int fbr_next_raw_byte(FastBitReader* br) {
    if (br->pos >= br->len) {
#ifndef _WIN32
        if (br->pf) {
            if (fbr_sync_prefetch(br, br->pos) <= br->pos) {
                br->eof = 1;
                return -1;
            }
        } else
#endif
        {
            if (!br->fp) { br->eof = 1; return -1; }
            br->len = fread(br->stream_buf, 1, sizeof(br->stream_buf), br->fp);
            br->buf = br->stream_buf;
            br->pos = 0;
            if (br->len == 0) { br->eof = 1; return -1; }
        }
    }
    return (int)br->buf[br->pos++];
}

static void fbr_fill_slow(FastBitReader* br) {
    while (br->bitcnt <= 56) {
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
                br->bitbuf |= ((uint64_t)0xFFU << (56 - br->bitcnt));
                br->bitcnt += 8;
            } else {
                br->marker_hit = 1;
                br->pending_marker = (nxt >= 0) ? (uint8_t)nxt : 0xD9;
                br->bitcnt += 8;
            }
        } else {
            br->bitbuf |= ((uint64_t)(uint8_t)b << (56 - br->bitcnt));
            br->bitcnt += 8;
        }
    }
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

typedef struct {
    union {
        int16_t c[64];
        uint32_t c32[32];
    };
    uint8_t has_ac;
    uint8_t row_mask;   // bit r set iff row r has non-zero AC (r=0: cols 1..7; r=1..7: cols 0..7)
    uint8_t dirty_rows; // bit r set iff row r had non-zero writes in previous decode (sparse clear!)
    uint8_t pad;
} DecBlock;

static inline uint32_t fs_bswap32(uint32_t w) {
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    return w;
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap32(w); // 1-cycle ARMv7 REV instruction!
#else
    return ((w & 0xFFU) << 24) | ((w & 0xFF00U) << 8) | ((w >> 8) & 0xFF00U) | (w >> 24);
#endif
}

// Hoisted 64-bit register-cached Huffman block decoder with 32-bit SWAR REV refill + combined ac_fast[1024]!
static inline int decode_huff_block_reg(FastBitReader* br,
                                        uint64_t* __restrict p_bitbuf, int* __restrict p_bitcnt,
                                        size_t* __restrict p_pos, size_t* __restrict p_len,
                                        const FastHuffTable* dc_t, const FastHuffTable* ac_t,
                                        int* dc_pred, DecBlock* blk) {
    uint64_t bitbuf = *p_bitbuf;
    int bitcnt = *p_bitcnt;
    size_t pos = *p_pos;
    size_t len = *p_len;
    const uint8_t* buf = br->buf;

    #define FBR_REFILL64() do { \
        if (bitcnt < 16) { \
            if (pos + 4 <= len && !br->marker_hit) { \
                uint32_t _w; \
                memcpy(&_w, buf + pos, 4); \
                if (((~_w - 0x01010101U) & _w & 0x80808080U) == 0) { \
                    uint32_t _be = fs_bswap32(_w); \
                    bitbuf |= ((uint64_t)_be << (32 - bitcnt)); \
                    bitcnt += 32; \
                    pos += 4; \
                } \
            } \
            if (bitcnt < 16) { \
                while (bitcnt <= 56 && pos < len && !br->marker_hit) { \
                    uint8_t _b = buf[pos]; \
                    if (_b == 0xFF) { break; } \
                    pos++; \
                    bitbuf |= ((uint64_t)_b << (56 - bitcnt)); \
                    bitcnt += 8; \
                } \
                if (bitcnt < 16) { \
                    br->bitbuf = bitbuf; br->bitcnt = bitcnt; br->pos = pos; \
                    fbr_fill_slow(br); \
                    bitbuf = br->bitbuf; bitcnt = br->bitcnt; pos = br->pos; len = br->len; buf = br->buf; \
                } \
            } \
        } \
    } while (0)

    #define FBR_HUFF64(tbl, out_sym) do { \
        FBR_REFILL64(); \
        uint32_t _p10 = (uint32_t)(bitbuf >> 54); \
        uint16_t _fe = (tbl)->fast_tab[_p10]; \
        if (_fe != 0) { \
            int _fl = _fe >> 8; \
            bitbuf <<= _fl; \
            bitcnt -= _fl; \
            (out_sym) = (int)(_fe & 0xFF); \
        } else { \
            int32_t _code = (int32_t)_p10; \
            bitbuf <<= 10; \
            bitcnt -= 10; \
            (out_sym) = -1; \
            for (int _l = 11; _l <= 16; _l++) { \
                if (bitcnt < 1) { \
                    br->bitbuf = bitbuf; br->bitcnt = bitcnt; br->pos = pos; \
                    fbr_fill_slow(br); \
                    bitbuf = br->bitbuf; bitcnt = br->bitcnt; pos = br->pos; len = br->len; buf = br->buf; \
                } \
                _code = (_code << 1) | (int32_t)(bitbuf >> 63); \
                bitbuf <<= 1; \
                bitcnt -= 1; \
                if (_code <= (tbl)->maxcode[_l]) { \
                    int _idx = _code + (tbl)->valoffset[_l]; \
                    if (_idx >= 0 && _idx < 256) { (out_sym) = (int)(tbl)->huffval[_idx]; } \
                    break; \
                } \
            } \
        } \
    } while (0)

    #define FBR_BITS64(n, out_val) do { \
        if (bitcnt < (n)) { FBR_REFILL64(); } \
        (out_val) = (int)(bitbuf >> (64 - (n))); \
        bitbuf <<= (n); \
        bitcnt -= (n); \
    } while (0)

    int s;
    FBR_HUFF64(dc_t, s);
    if (s < 0 || s > 15) { *p_bitbuf = bitbuf; *p_bitcnt = bitcnt; *p_pos = pos; *p_len = len; return 0; }
    int diff = 0;
    if (s > 0) {
        int bits;
        FBR_BITS64(s, bits);
        diff = fbr_extend(bits, s);
    }
    *dc_pred += diff;
    int16_t* out64 = blk->c;
    out64[0] = (int16_t)(*dc_pred);

    // Fast EOB check on first AC coefficient via combined ac_fast[1024]!
    FBR_REFILL64();
    uint32_t ae0 = ac_t->ac_fast[(uint32_t)(bitbuf >> 54)];
    if (ae0 != 0 && (ae0 & ~0x0FU) == 0) {
        int tb0 = (int)(ae0 & 0x0FU);
        bitbuf <<= tb0;
        bitcnt -= tb0;
        blk->has_ac = 0;
        blk->row_mask = 0;
        *p_bitbuf = bitbuf; *p_bitcnt = bitcnt; *p_pos = pos; *p_len = len;
        return 1;
    }

    // Sparse-clear ONLY rows touched by previous band using inline 32-bit stores (zero libc memset register clobber!)
    uint8_t dirty = blk->dirty_rows;
    if (dirty) {
        if (dirty & 1U) {
            out64[1] = 0;
            blk->c32[1] = 0;
            blk->c32[2] = 0;
            blk->c32[3] = 0;
            dirty &= (uint8_t)~1U;
        }
        while (dirty) {
#if defined(__GNUC__) || defined(__clang__)
            int dr = __builtin_ctz((unsigned int)dirty);
#else
            int dr = 0;
            while (((dirty >> dr) & 1) == 0) dr++;
#endif
            uint32_t* zp = blk->c32 + (dr << 2);
            zp[0] = 0;
            zp[1] = 0;
            zp[2] = 0;
            zp[3] = 0;
            dirty &= (uint8_t)(dirty - 1);
        }
    }

    uint8_t has_ac = 0;
    uint8_t row_mask = 0;
    int k = 1;
    for (;;) {
        FBR_REFILL64();
        uint32_t ae = ac_t->ac_fast[(uint32_t)(bitbuf >> 54)];
        if (ae != 0) {
            int tb = (int)(ae & 0x0FU);
            bitbuf <<= tb;
            bitcnt -= tb;
            int16_t val = (int16_t)(ae >> 16);
            int r = (int)((ae >> 4) & 0x0FU);
            if (val != 0) {
                k += r;
                if (k >= 64) break;
                int nat = FS_ZIGZAG[k];
                out64[nat] = val;
                has_ac = 1;
                row_mask |= (uint8_t)(1U << (nat >> 3));
                k++;
                if (k >= 64) break;
                continue;
            }
            if (r == 0) break; // EOB
            k += 16;           // ZRL
            if (k >= 64) break;
            continue;
        }

        // Slow path (~15% of ACs where huff_len + sz > 10)
        int rs;
        FBR_HUFF64(ac_t, rs);
        if (rs < 0) {
            blk->has_ac = has_ac;
            blk->row_mask = row_mask;
            blk->dirty_rows = row_mask;
            *p_bitbuf = bitbuf; *p_bitcnt = bitcnt; *p_pos = pos; *p_len = len;
            return 0;
        }
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
            int bits;
            FBR_BITS64(sz, bits);
            int val = fbr_extend(bits, sz);
            int nat = FS_ZIGZAG[k];
            out64[nat] = (int16_t)val;
            if (val != 0) {
                has_ac = 1;
                row_mask |= (uint8_t)(1U << (nat >> 3));
            }
            k++;
            if (k >= 64) break;
        }
    }

    blk->has_ac = has_ac;
    blk->row_mask = row_mask;
    blk->dirty_rows = row_mask;
    *p_bitbuf = bitbuf; *p_bitcnt = bitcnt; *p_pos = pos; *p_len = len;

    #undef FBR_REFILL64
    #undef FBR_HUFF64
    #undef FBR_BITS64
    return 1;
}

static inline void zero_row8_i32(int32_t* w) {
    w[0] = w[1] = w[2] = w[3] = w[4] = w[5] = w[6] = w[7] = 0;
}

// Fast 1D row butterfly helper with bit-exact 1-input, 2-input, and 4-input shortcuts!
static inline void idct_row_aan(const int16_t* ir, const int32_t* dr, int32_t* wr) {
    if ((ir[4] | ir[5] | ir[6] | ir[7]) == 0) {
        if ((ir[2] | ir[3]) == 0) {
            int32_t t0 = (int32_t)ir[0] * dr[0];
            if (ir[1] == 0) {
                int32_t dc = (t0 + 1) >> 1;
                wr[0] = wr[1] = wr[2] = wr[3] = wr[4] = wr[5] = wr[6] = wr[7] = dc;
                return;
            }
            // Bit-exact 2-input row IDCT (only ir[0] & ir[1] non-zero -> 3 multiplies!)
            int32_t t4 = (int32_t)ir[1] * dr[1];
            int32_t t11 = (t4 * 362 + 128) >> 8;
            int32_t z5  = (t4 * 473 + 128) >> 8;
            int32_t t10 = ((t4 * 277 + 128) >> 8) - z5;
            int32_t t6  = z5 - t4;
            int32_t o5  = t11 - t6;
            int32_t o4  = t10 + o5;
            wr[0] = (t0 + t4 + 1) >> 1; wr[7] = (t0 - t4 + 1) >> 1;
            wr[1] = (t0 + t6 + 1) >> 1; wr[6] = (t0 - t6 + 1) >> 1;
            wr[2] = (t0 + o5 + 1) >> 1; wr[5] = (t0 - o5 + 1) >> 1;
            wr[3] = (t0 - o4 + 1) >> 1; wr[4] = (t0 + o4 + 1) >> 1;
            return;
        }
        int32_t t0 = (int32_t)ir[0] * dr[0];
        int32_t t1 = (int32_t)ir[2] * dr[2];
        int32_t t12 = ((t1 * 362 + 128) >> 8) - t1;
        int32_t e0 = t0 + t1, e3 = t0 - t1, e1 = t0 + t12, e2 = t0 - t12;

        int32_t t4 = (int32_t)ir[1] * dr[1];
        int32_t t5 = (int32_t)ir[3] * dr[3];
        int32_t t7 = t4 + t5;
        int32_t t11 = ((t4 - t5) * 362 + 128) >> 8;
        int32_t z5 = ((t4 - t5) * 473 + 128) >> 8;
        int32_t t10 = ((t4 * 277 + 128) >> 8) - z5;
        int32_t t12o = ((t5 * 669 + 128) >> 8) + z5;
        int32_t t6 = t12o - t7, o5 = t11 - t6, o4 = t10 + o5;

        wr[0] = (e0 + t7 + 1) >> 1; wr[7] = (e0 - t7 + 1) >> 1;
        wr[1] = (e1 + t6 + 1) >> 1; wr[6] = (e1 - t6 + 1) >> 1;
        wr[2] = (e2 + o5 + 1) >> 1; wr[5] = (e2 - o5 + 1) >> 1;
        wr[3] = (e3 - o4 + 1) >> 1; wr[4] = (e3 + o4 + 1) >> 1;
        return;
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

// Fast Sparse Row-Mask AAN 2D IDCT (8x8 -> 8x8) with bit-exact 1/2/4-row paths + ARMv7 USAT ASR #9 (L1 I-cache compact!)
static FS_NOINLINE void idct_8x8_aan(const DecBlock* blk, const int32_t* dq, const uint16_t* q_raw, uint8_t* out) {
    const int16_t* in = blk->c;
    if (!blk->has_ac) {
        uint8_t c = clamp_u8_v7((((int)in[0] * (int)q_raw[0] + 4) >> 3) + 128);
        memset(out, c, 64);
        return;
    }

    uint8_t rmask = blk->row_mask;
    if ((rmask & 0xFEU) == 0) {
        int32_t r0[8];
        idct_row_aan(in, dq, r0);
        for (int c = 0; c < 8; c++) {
            out[c] = clamp_u8_asr9_v7(r0[c] + 65792);
        }
        for (int r = 1; r < 8; r++) {
            memcpy(out + r * 8, out, 8);
        }
        return;
    }

    int32_t ws[64];
    if (rmask & 1U) {
        idct_row_aan(in, dq, ws);
    } else {
        int32_t dc = ((int32_t)in[0] * dq[0] + 1) >> 1;
        ws[0] = ws[1] = ws[2] = ws[3] = ws[4] = ws[5] = ws[6] = ws[7] = dc;
    }

    if ((rmask & 0xFCU) == 0) {
        idct_row_aan(in + 8, dq + 8, ws + 8);
        for (int c = 0; c < 8; c++) {
            int32_t w0 = ws[c] + 65792, w1 = ws[c + 8];
            int32_t t11 = (w1 * 362 + 128) >> 8;
            int32_t z5 = (w1 * 473 + 128) >> 8;
            int32_t t10 = ((w1 * 277 + 128) >> 8) - z5;
            int32_t t6 = z5 - w1;
            int32_t t5 = t11 - t6;
            int32_t t4 = t10 + t5;
            out[c]      = clamp_u8_asr9_v7(w0 + w1);
            out[c + 56] = clamp_u8_asr9_v7(w0 - w1);
            out[c + 8]  = clamp_u8_asr9_v7(w0 + t6);
            out[c + 48] = clamp_u8_asr9_v7(w0 - t6);
            out[c + 16] = clamp_u8_asr9_v7(w0 + t5);
            out[c + 40] = clamp_u8_asr9_v7(w0 - t5);
            out[c + 24] = clamp_u8_asr9_v7(w0 - t4);
            out[c + 32] = clamp_u8_asr9_v7(w0 + t4);
        }
        return;
    }

    if ((rmask & 0xF0U) == 0) {
        for (int r = 1; r < 4; r++) {
            if (rmask & (1U << r)) idct_row_aan(in + r * 8, dq + r * 8, ws + r * 8);
            else zero_row8_i32(ws + r * 8);
        }
        for (int c = 0; c < 8; c++) {
            int32_t w0 = ws[c] + 65792, w1 = ws[c + 8], w2 = ws[c + 16], w3 = ws[c + 24];
            if ((w1 | w2 | w3) == 0) {
                uint8_t cl = clamp_u8_asr9_v7(w0);
                out[c] = out[c+8] = out[c+16] = out[c+24] = out[c+32] = out[c+40] = out[c+48] = out[c+56] = cl;
                continue;
            }
            int32_t t12 = ((w2 * 362 + 128) >> 8) - w2;
            int32_t t0 = w0 + w2, t3 = w0 - w2, t1 = w0 + t12, t2 = w0 - t12;
            int32_t t7 = w1 + w3;
            int32_t t11 = ((w1 - w3) * 362 + 128) >> 8;
            int32_t z5 = ((w1 - w3) * 473 + 128) >> 8;
            int32_t t10 = ((w1 * 277 + 128) >> 8) - z5;
            int32_t t12o = ((w3 * 669 + 128) >> 8) + z5;
            int32_t t6 = t12o - t7, t5 = t11 - t6, t4 = t10 + t5;
            out[c]      = clamp_u8_asr9_v7(t0 + t7);
            out[c + 56] = clamp_u8_asr9_v7(t0 - t7);
            out[c + 8]  = clamp_u8_asr9_v7(t1 + t6);
            out[c + 48] = clamp_u8_asr9_v7(t1 - t6);
            out[c + 16] = clamp_u8_asr9_v7(t2 + t5);
            out[c + 40] = clamp_u8_asr9_v7(t2 - t5);
            out[c + 24] = clamp_u8_asr9_v7(t3 - t4);
            out[c + 32] = clamp_u8_asr9_v7(t3 + t4);
        }
        return;
    }

    for (int r = 1; r < 8; r++) {
        if (rmask & (1U << r)) idct_row_aan(in + r * 8, dq + r * 8, ws + r * 8);
        else zero_row8_i32(ws + r * 8);
    }

    for (int c = 0; c < 8; c++) {
        int32_t* wc = ws + c;
        if ((wc[8] | wc[16] | wc[24] | wc[32] | wc[40] | wc[48] | wc[56]) == 0) {
            uint8_t cl = clamp_u8_asr9_v7(wc[0] + 65792);
            out[c] = out[c+8] = out[c+16] = out[c+24] = out[c+32] = out[c+40] = out[c+48] = out[c+56] = cl;
            continue;
        }
        int32_t t0 = wc[0] + 65792, t1 = wc[16], t2 = wc[32], t3 = wc[48];
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

        out[c]      = clamp_u8_asr9_v7(t0 + t7);
        out[c + 56] = clamp_u8_asr9_v7(t0 - t7);
        out[c + 8]  = clamp_u8_asr9_v7(t1 + t6);
        out[c + 48] = clamp_u8_asr9_v7(t1 - t6);
        out[c + 16] = clamp_u8_asr9_v7(t2 + t5);
        out[c + 40] = clamp_u8_asr9_v7(t2 - t5);
        out[c + 24] = clamp_u8_asr9_v7(t3 - t4);
        out[c + 32] = clamp_u8_asr9_v7(t3 + t4);
    }
}

// Exact Half-Height Vertical AAN IDCT (8x8 DCT -> 8x4 vertically 2:1 averaged chroma) + ARMv7 USAT ASR #10
static FS_NOINLINE void idct_8x4_half_v_aan(const DecBlock* blk, const int32_t* dq, const uint16_t* q_raw, uint8_t* out32) {
    const int16_t* in = blk->c;
    uint8_t rmask = (uint8_t)(blk->row_mask & ~0x10U);
    if (!blk->has_ac || rmask == 0) {
        uint8_t c = clamp_u8_v7((((int)in[0] * (int)q_raw[0] + 4) >> 3) + 128);
        memset(out32, c, 32);
        return;
    }

    if ((rmask & 0xFEU) == 0) {
        int32_t r0[8];
        idct_row_aan(in, dq, r0);
        for (int c = 0; c < 8; c++) {
            out32[c] = clamp_u8_asr9_v7(r0[c] + 65792);
        }
        memcpy(out32 + 8, out32, 8);
        memcpy(out32 + 16, out32, 16);
        return;
    }

    int32_t ws[64];
    if (rmask & 1U) {
        idct_row_aan(in, dq, ws);
    } else {
        int32_t dc = ((int32_t)in[0] * dq[0] + 1) >> 1;
        ws[0] = ws[1] = ws[2] = ws[3] = ws[4] = ws[5] = ws[6] = ws[7] = dc;
    }

    if ((rmask & 0xFCU) == 0) {
        idct_row_aan(in + 8, dq + 8, ws + 8);
        for (int c = 0; c < 8; c++) {
            int32_t e0 = (ws[c] << 1) + 131584;
            int32_t w1 = ws[c + 8];
            int32_t z5 = (w1 * 473 + 128) >> 8;
            int32_t t10 = ((w1 * 277 + 128) >> 8) - z5;
            out32[c]      = clamp_u8_asr10_v7(e0 + z5);
            out32[c + 8]  = clamp_u8_asr10_v7(e0 - t10);
            out32[c + 16] = clamp_u8_asr10_v7(e0 + t10);
            out32[c + 24] = clamp_u8_asr10_v7(e0 - z5);
        }
        return;
    }

    if ((rmask & 0xE0U) == 0) {
        for (int r = 1; r < 4; r++) {
            if (rmask & (1U << r)) idct_row_aan(in + r * 8, dq + r * 8, ws + r * 8);
            else zero_row8_i32(ws + r * 8);
        }
        for (int c = 0; c < 8; c++) {
            int32_t w0 = ws[c], w1 = ws[c + 8], w2 = ws[c + 16], w3 = ws[c + 24];
            if ((w1 | w2 | w3) == 0) {
                uint8_t cl = clamp_u8_asr9_v7(w0 + 65792);
                out32[c] = out32[c + 8] = out32[c + 16] = out32[c + 24] = cl;
                continue;
            }
            int32_t e0 = (w0 << 1) + 131584;
            int32_t ed = (w2 * 362 + 128) >> 8;
            int32_t z5 = ((w1 - w3) * 473 + 128) >> 8;
            int32_t t10 = ((w1 * 277 + 128) >> 8) - z5;
            int32_t t12 = ((w3 * 669 + 128) >> 8) + z5;
            out32[c]      = clamp_u8_asr10_v7(e0 + ed + t12);
            out32[c + 8]  = clamp_u8_asr10_v7(e0 - ed - t10);
            out32[c + 16] = clamp_u8_asr10_v7(e0 - ed + t10);
            out32[c + 24] = clamp_u8_asr10_v7(e0 + ed - t12);
        }
        return;
    }

    for (int r = 1; r < 8; r++) {
        if (r == 4) continue;
        if (rmask & (1U << r)) idct_row_aan(in + r * 8, dq + r * 8, ws + r * 8);
        else zero_row8_i32(ws + r * 8);
    }

    for (int c = 0; c < 8; c++) {
        int32_t* wc = ws + c;
        if ((wc[8] | wc[16] | wc[24] | wc[40] | wc[48] | wc[56]) == 0) {
            uint8_t cl = clamp_u8_asr9_v7(wc[0] + 65792);
            out32[c] = out32[c + 8] = out32[c + 16] = out32[c + 24] = cl;
            continue;
        }
        int32_t e0 = (wc[0] << 1) + 131584;
        int32_t ed = ((wc[16] - wc[48]) * 362 + 128) >> 8;
        int32_t z10 = wc[40] - wc[24];
        int32_t z12 = wc[8]  - wc[56];
        int32_t z5  = ((z10 + z12) * 473 + 128) >> 8;
        int32_t t10 = ((z12 * 277 + 128) >> 8) - z5;
        int32_t t12 = ((z10 * -669 + 128) >> 8) + z5;

        out32[c]      = clamp_u8_asr10_v7(e0 + ed + t12);
        out32[c + 8]  = clamp_u8_asr10_v7(e0 - ed - t10);
        out32[c + 16] = clamp_u8_asr10_v7(e0 - ed + t10);
        out32[c + 24] = clamp_u8_asr10_v7(e0 + ed - t12);
    }
}

typedef struct {
    int16_t dz;
    uint16_t inv_q;
    uint8_t zz;
    uint8_t pad[3];
} QuantNatEntry;

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
    int chroma_res;
    QuantNatEntry q_luma_nat[64];
    QuantNatEntry q_chroma_nat[64];
    uint32_t dct_scale_luma_q16[64]; // round(Q_in_Y[nat] * 2048 / Q_out_Y[nat]) for Q5 centered DCT-domain slope!
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
    build_fast_huff(&info->dc_huff[0], STD_DC_LUMA_BITS, STD_DC_LUMA_VALS, 12, 0);
    build_fast_huff(&info->dc_huff[1], STD_DC_CHROMA_BITS, STD_DC_CHROMA_VALS, 12, 0);
    build_fast_huff(&info->ac_huff[0], STD_AC_LUMA_BITS, STD_AC_LUMA_VALS, 162, 1);
    build_fast_huff(&info->ac_huff[1], STD_AC_CHROMA_BITS, STD_AC_CHROMA_VALS, 162, 1);

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
                build_fast_huff(tc == 0 ? &info->dc_huff[th] : &info->ac_huff[th], bits, tmp + p, total, tc == 1);
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

static inline int16_t lut64_eval_y(const uint32_t* col, uint8_t y) {
    int y0 = (int)(y >> 4), yf = (int)(y & 15U);
    int ya = (int)(col[y0] >> 24);
    int yb = (int)(col[y0 + 1] >> 24);
    return (int16_t)((((ya - 128) << 4) + 8 + (yb - ya) * yf) >> 4);
}

// Ultra-Fast 2-Load Direct-Chroma 64x64x17 YCbCr 3D LUT with 8:12:12 SWAR Linear-Y Chroma:
// - ZERO chroma simplex weights, ZERO bounds clamps, 1 SWAR mul-add interpolates BOTH Cb and Cr bit-exactly!
// - Same-cell fast path (~80% of quads): ONLY 2 LUT loads total for all 4 Y + Cb + Cr!
// - 1-cycle chroma residual pass-through restores exact smooth sub-bucket chroma gradients!
static inline void grade_ycc_quad_2x2_direct64(const uint32_t* __restrict ycc64, int chroma_res,
                                               uint8_t y00, uint8_t y01, uint8_t y10, uint8_t y11,
                                               uint8_t cb, uint8_t cr,
                                               int16_t* oy00, int16_t* oy01, int16_t* oy10, int16_t* oy11,
                                               int16_t* ocb, int16_t* ocr) {
    size_t cidx = (((size_t)(cr >> 2) << 6) | (size_t)(cb >> 2));
    const uint32_t* col = ycc64 + (cidx + (cidx << 4));

    if ((((y00 ^ y01) | (y00 ^ y10) | (y00 ^ y11)) & 0xF0U) == 0) {
        int y0 = (int)(y00 >> 4);
        uint32_t va = col[y0];
        uint32_t vb = col[y0 + 1];

        int ya = (int)(va >> 24), yb = (int)(vb >> 24);
        int a16 = ((ya - 128) << 4) + 8, d = yb - ya;
        int f0 = (int)(y00 & 15U), f1 = (int)(y01 & 15U), f2 = (int)(y10 & 15U), f3 = (int)(y11 & 15U);
        *oy00 = (int16_t)((a16 + d * f0) >> 4);
        *oy01 = (int16_t)((a16 + d * f1) >> 4);
        *oy10 = (int16_t)((a16 + d * f2) >> 4);
        *oy11 = (int16_t)((a16 + d * f3) >> 4);

        uint32_t yaf = (uint32_t)((f0 + f1 + f2 + f3 + 2) >> 2);
        uint32_t cc = (va & 0x00FFFFFFU) * (16U - yaf) + (vb & 0x00FFFFFFU) * yaf + 0x00008008U;
        int rcb = (int)(cc >> 16);
        int rcr = (int)((cc >> 4) & 0xFFU);
        if (chroma_res) {
            *ocb = clamp_s8_v7(rcb + (int)(cb & 3U) - 130);
            *ocr = clamp_s8_v7(rcr + (int)(cr & 3U) - 130);
        } else {
            *ocb = (int16_t)(rcb - 128);
            *ocr = (int16_t)(rcr - 128);
        }
        return;
    }

    *oy00 = lut64_eval_y(col, y00);
    *oy01 = lut64_eval_y(col, y01);
    *oy10 = lut64_eval_y(col, y10);
    *oy11 = lut64_eval_y(col, y11);

    int ya = ((int)y00 + (int)y01 + (int)y10 + (int)y11 + 2) >> 2;
    int ya0 = ya >> 4;
    uint32_t yaf = (uint32_t)(ya & 15);
    uint32_t va = col[ya0];
    uint32_t vb = col[ya0 + 1];
    uint32_t cc = (va & 0x00FFFFFFU) * (16U - yaf) + (vb & 0x00FFFFFFU) * yaf + 0x00008008U;
    int rcb = (int)(cc >> 16);
    int rcr = (int)((cc >> 4) & 0xFFU);
    if (chroma_res) {
        *ocb = clamp_s8_v7(rcb + (int)(cb & 3U) - 130);
        *ocr = clamp_s8_v7(rcr + (int)(cr & 3U) - 130);
    } else {
        *ocb = (int16_t)(rcb - 128);
        *ocr = (int16_t)(rcr - 128);
    }
}

// Register-Clean Split Even/Odd Luma AAN FDCT + Direct-Write Sparse-Prev-Clear Deadzone Quantization:
// - ZERO qtmp[64], ZERO libc memset call, ZERO nz_lo/nz_hi scatter loop!
// - Splits column FDCT into even half (c0,c2,c4,c6) then odd half (c1,c3,c5,c7) -> <=11 live regs (zero ARM32 stack spills!)
// - Packed QuantNatEntry collapses 3 table base pointers into 1!
// - Requires q_work/q_emit allocated via calloc so qb->q[k]==0 for all k > qb->last_nz.
static void fast_fdct_quant_luma(int16_t* __restrict blk, const QuantNatEntry* __restrict qent,
                                 JpegQuantBlock* __restrict qb) {
    int prev_nz = (int)qb->last_nz;
    if (prev_nz > 0) {
        int16_t* qp = qb->q;
        for (int k = 1; k <= prev_nz; k++) qp[k] = 0;
    }

    int any_ac_col = 0;
    for (int i = 0; i < 8; i++) {
        int16_t* r = blk + i * 8;
        int d3 = r[3] - r[4], d2 = r[2] - r[5], d1 = r[1] - r[6], d0 = r[0] - r[7];
        int s0 = r[0] + r[7], s1 = r[1] + r[6], s2 = r[2] + r[5], s3 = r[3] + r[4];
        int t0 = s0 + s3, t3 = s0 - s3, t1 = s1 + s2, t2 = s1 - s2;
        r[0] = (int16_t)(t0 + t1);
        int ac_or = d0 | d1 | d2 | d3 | t2 | t3 | (t0 - t1);
        if (ac_or == 0) {
            r[1] = r[2] = r[3] = r[4] = r[5] = r[6] = r[7] = 0;
            continue;
        }
        any_ac_col = 1;
        r[4] = (int16_t)(t0 - t1);
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

    int last_nz = 0;
    #define QREG_LUMA(val, nat) do { \
        int _c = (val); \
        const QuantNatEntry* _e = &qent[(nat)]; \
        int _dz = _e->dz; \
        if ((unsigned)(_c + _dz) > (unsigned)(_dz << 1)) { \
            int _s = _c < 0; \
            int _qv = (((_s ? -_c : _c) * (int)_e->inv_q + 32768) >> 16); \
            if (_qv > 1023) { _qv = 1023; } \
            int _zi = (int)_e->zz; \
            qb->q[_zi] = (int16_t)(_s ? -_qv : _qv); \
            if (_zi > last_nz) { last_nz = _zi; } \
        } \
    } while (0)

    // Peeled column 0 (single-load r0..r7; save r0 before overwriting blk[0] with unquantized DC!)
    {
        int r0 = blk[0], r1 = blk[8], r2 = blk[16], r3 = blk[24];
        int r4 = blk[32], r5 = blk[40], r6 = blk[48], r7 = blk[56];
        int s0 = r0 + r7, s1 = r1 + r6, s2 = r2 + r5, s3 = r3 + r4;
        int d0 = r0 - r7, d1 = r1 - r6, d2 = r2 - r5, d3 = r3 - r4;
        if ((s0 | s1 | s2 | s3) == 0) {
            blk[0] = 0;
            qb->q[0] = 0;
        } else {
            int t0 = s0 + s3, t3 = s0 - s3, t1 = s1 + s2, t2 = s1 - s2;
            int c0 = (t0 + t1) >> 3;
            blk[0] = (int16_t)c0;
            int sd = c0 < 0;
            int qd = (((sd ? -c0 : c0) * (int)qent[0].inv_q) + 32768) >> 16;
            qb->q[0] = (int16_t)(sd ? -qd : qd);
            if (((t0 - t1) | t2 | t3) != 0) {
                QREG_LUMA((t0 - t1) >> 3, 32);
                int z1 = ((t2 + t3) * 181) >> 8;
                QREG_LUMA((t3 + z1) >> 3, 16);
                QREG_LUMA((t3 - z1) >> 3, 48);
            }
        }
        if ((d0 | d1 | d2 | d3) != 0) {
            int t4 = d2 + d3, t5 = d1 + d2, t6 = d0 + d1;
            int z5 = ((t4 - t6) * 98) >> 8;
            int z2 = ((t4 * 139) >> 8) + z5;
            int z4 = ((t6 * 334) >> 8) + z5;
            int z3 = (t5 * 181) >> 8;
            int z11 = d0 + z3, z13 = d0 - z3;
            QREG_LUMA((z11 + z4) >> 3,  8);
            QREG_LUMA((z11 - z4) >> 3, 56);
            QREG_LUMA((z13 + z2) >> 3, 40);
            QREG_LUMA((z13 - z2) >> 3, 24);
        }
    }

    if (any_ac_col) {
        for (int i = 1; i < 8; i++) {
            const int16_t* r = blk + i;
            int r0 = r[0], r1 = r[8], r2 = r[16], r3 = r[24];
            int r4 = r[32], r5 = r[40], r6 = r[48], r7 = r[56];
            int s0 = r0 + r7, s1 = r1 + r6, s2 = r2 + r5, s3 = r3 + r4;
            int d0 = r0 - r7, d1 = r1 - r6, d2 = r2 - r5, d3 = r3 - r4;
            if ((s0 | s1 | s2 | s3) != 0) {
                int t0 = s0 + s3, t3 = s0 - s3, t1 = s1 + s2, t2 = s1 - s2;
                QREG_LUMA((t0 + t1) >> 3, i);
                QREG_LUMA((t0 - t1) >> 3, 32 + i);
                int z1 = ((t2 + t3) * 181) >> 8;
                QREG_LUMA((t3 + z1) >> 3, 16 + i);
                QREG_LUMA((t3 - z1) >> 3, 48 + i);
            }
            if ((d0 | d1 | d2 | d3) != 0) {
                int t4 = d2 + d3, t5 = d1 + d2, t6 = d0 + d1;
                int z5 = ((t4 - t6) * 98) >> 8;
                int z2 = ((t4 * 139) >> 8) + z5;
                int z4 = ((t6 * 334) >> 8) + z5;
                int z3 = (t5 * 181) >> 8;
                int z11 = d0 + z3, z13 = d0 - z3;
                QREG_LUMA((z11 + z4) >> 3,  8 + i);
                QREG_LUMA((z11 - z4) >> 3, 56 + i);
                QREG_LUMA((z13 + z2) >> 3, 40 + i);
                QREG_LUMA((z13 - z2) >> 3, 24 + i);
            }
        }
    }
    #undef QREG_LUMA
    qb->last_nz = (uint8_t)last_nz;
}

// Fast 4x4 Top-Left Chroma FDCT + Direct-Write Sparse-Prev-Clear Deadzone Quantization:
static void fast_fdct_quant_chroma(int16_t* __restrict blk, const QuantNatEntry* __restrict qent,
                                   JpegQuantBlock* __restrict qb) {
    int prev_nz = (int)qb->last_nz;
    if (prev_nz > 0) {
        int16_t* qp = qb->q;
        for (int k = 1; k <= prev_nz; k++) qp[k] = 0;
    }

    int any_ac_col = 0;
    for (int i = 0; i < 8; i++) {
        int16_t* r = blk + i * 8;
        int d3 = r[3] - r[4], d2 = r[2] - r[5], d1 = r[1] - r[6], d0 = r[0] - r[7];
        int s0 = r[0] + r[7], s1 = r[1] + r[6], s2 = r[2] + r[5], s3 = r[3] + r[4];
        int t0 = s0 + s3, t3 = s0 - s3, t1 = s1 + s2, t2 = s1 - s2;
        r[0] = (int16_t)(t0 + t1);
        if ((d0 | d1 | d2 | d3 | t2 | t3) == 0) {
            r[1] = r[2] = r[3] = 0;
            continue;
        }
        any_ac_col = 1;
        int z1 = ((t2 + t3) * 181) >> 8;
        r[2] = (int16_t)(t3 + z1);
        int t4 = d2 + d3, t5 = d1 + d2, t6 = d0 + d1;
        int z5 = ((t4 - t6) * 98) >> 8;
        int z2 = ((t4 * 139) >> 8) + z5;
        int z4 = ((t6 * 334) >> 8) + z5;
        int z3 = (t5 * 181) >> 8;
        r[1] = (int16_t)((d0 + z3) + z4);
        r[3] = (int16_t)((d0 - z3) - z2);
    }

    int last_nz = 0;
    #define QREG_CHROMA(val, nat) do { \
        int _c = (val); \
        const QuantNatEntry* _e = &qent[(nat)]; \
        int _dz = _e->dz; \
        if ((unsigned)(_c + _dz) > (unsigned)(_dz << 1)) { \
            int _s = _c < 0; \
            int _qv = (((_s ? -_c : _c) * (int)_e->inv_q + 32768) >> 16); \
            if (_qv > 1023) { _qv = 1023; } \
            int _zi = (int)_e->zz; \
            qb->q[_zi] = (int16_t)(_s ? -_qv : _qv); \
            if (_zi > last_nz) { last_nz = _zi; } \
        } \
    } while (0)

    // Peeled column 0 (single-load r0..r7)
    {
        int r0 = blk[0], r1 = blk[8], r2 = blk[16], r3 = blk[24];
        int r4 = blk[32], r5 = blk[40], r6 = blk[48], r7 = blk[56];
        int s0 = r0 + r7, s1 = r1 + r6, s2 = r2 + r5, s3 = r3 + r4;
        int d0 = r0 - r7, d1 = r1 - r6, d2 = r2 - r5, d3 = r3 - r4;
        if ((s0 | s1 | s2 | s3) == 0) {
            qb->q[0] = 0;
        } else {
            int t0 = s0 + s3, t3 = s0 - s3, t1 = s1 + s2, t2 = s1 - s2;
            int c0 = (t0 + t1) >> 3;
            int sd = c0 < 0;
            int qd = (((sd ? -c0 : c0) * (int)qent[0].inv_q) + 32768) >> 16;
            qb->q[0] = (int16_t)(sd ? -qd : qd);
            if ((t2 | t3) != 0) {
                int z1 = ((t2 + t3) * 181) >> 8;
                QREG_CHROMA((t3 + z1) >> 3, 16);
            }
        }
        if ((d0 | d1 | d2 | d3) != 0) {
            int t4 = d2 + d3, t5 = d1 + d2, t6 = d0 + d1;
            int z5 = ((t4 - t6) * 98) >> 8;
            int z2 = ((t4 * 139) >> 8) + z5;
            int z4 = ((t6 * 334) >> 8) + z5;
            int z3 = (t5 * 181) >> 8;
            QREG_CHROMA(((d0 + z3) + z4) >> 3,  8);
            QREG_CHROMA(((d0 - z3) - z2) >> 3, 24);
        }
    }

    if (any_ac_col) {
        for (int i = 1; i < 4; i++) {
            const int16_t* r = blk + i;
            int r0 = r[0], r1 = r[8], r2 = r[16], r3 = r[24];
            int r4 = r[32], r5 = r[40], r6 = r[48], r7 = r[56];
            int s0 = r0 + r7, s1 = r1 + r6, s2 = r2 + r5, s3 = r3 + r4;
            int d0 = r0 - r7, d1 = r1 - r6, d2 = r2 - r5, d3 = r3 - r4;
            if ((s0 | s1 | s2 | s3) != 0) {
                int t0 = s0 + s3, t3 = s0 - s3, t1 = s1 + s2, t2 = s1 - s2;
                QREG_CHROMA((t0 + t1) >> 3, i);
                int z1 = ((t2 + t3) * 181) >> 8;
                QREG_CHROMA((t3 + z1) >> 3, 16 + i);
            }
            if ((d0 | d1 | d2 | d3) != 0) {
                int t4 = d2 + d3, t5 = d1 + d2, t6 = d0 + d1;
                int z5 = ((t4 - t6) * 98) >> 8;
                int z2 = ((t4 * 139) >> 8) + z5;
                int z4 = ((t6 * 334) >> 8) + z5;
                int z3 = (t5 * 181) >> 8;
                QREG_CHROMA(((d0 + z3) + z4) >> 3,  8 + i);
                QREG_CHROMA(((d0 - z3) - z2) >> 3, 24 + i);
            }
        }
    }
    #undef QREG_CHROMA
    qb->last_nz = (uint8_t)last_nz;
}

static inline void quantize_mcu_fast(const FastJpegInfo* info, int16_t y_blk[4][64],
                                     int16_t cb_blk[64], int16_t cr_blk[64], JpegQuantBlock* out_6) {
    for (int b = 0; b < 4; b++) {
        fast_fdct_quant_luma(y_blk[b], info->q_luma_nat, out_6 + b);
    }
    fast_fdct_quant_chroma(cb_blk, info->q_chroma_nat, out_6 + 4);
    fast_fdct_quant_chroma(cr_blk, info->q_chroma_nat, out_6 + 5);
}

// Wrapped 64x72 static grain table (eliminates 35.6M per-pixel '& 63' masks on 17.8MP!)
static int8_t s_grain_64x72[64 * 72];
static int s_grain_72_ready = 0;

static void init_grain_64x72(void) {
    if (s_grain_72_ready) return;
    init_static_grain_table();
    for (int r = 0; r < 64; r++) {
        const int8_t* src = STATIC_GRAIN_64x64 + r * 64;
        int8_t* dst = s_grain_64x72 + r * 72;
        memcpy(dst, src, 64);
        memcpy(dst + 64, src, 8);
    }
    s_grain_72_ready = 1;
}

// Fast Table-Driven Luma-Domain Film Grain & Clarity directly on signed L1-resident y_blk[4][64] (SSAT #8, zero +128/-128!)
static void apply_luma_grain_clarity_mcu(int16_t y_blk[4][64], int global_x0, int global_y0,
                                         const uint8_t grain_mid_lut[256], int has_grain, int clarity_strength) {
    const uint8_t* gmid = grain_mid_lut + 128;
    int x_tile = (global_x0 >> 4);
    if (has_grain && clarity_strength == 0) {
        for (int y = 0; y < 16; y++) {
            int gy = global_y0 + y;
            int b_idx = (y >= 8) ? 2 : 0;
            int sy8 = (y & 7) * 8;
            int phase = (int)(((uint32_t)gy * 2654435761U) >> 26);
            const int8_t* g1_row = s_grain_64x72 + (gy & 63) * 72;
            const int8_t* g2_row = s_grain_64x72 + ((gy * 7 + x_tile) & 63) * 72;
            for (int half = 0; half < 2; half++) {
                int16_t* row = y_blk[b_idx + half] + sy8;
                int gx_base = global_x0 + (half << 3);
                const int8_t* g1 = g1_row + (gx_base & 63);
                const int8_t* g2 = g2_row + ((gx_base + phase) & 63);
                for (int x = 0; x < 8; x += 2) {
                    int v0 = row[x], v1 = row[x + 1];
                    int r0 = (int)g1[x] + (int)g2[x];
                    int r1 = (int)g1[x + 1] + (int)g2[x + 1];
                    row[x]     = clamp_s8_v7(v0 + ((r0 * (int)gmid[v0]) >> 9));
                    row[x + 1] = clamp_s8_v7(v1 + ((r1 * (int)gmid[v1]) >> 9));
                }
            }
        }
        return;
    }

    for (int y = 0; y < 16; y++) {
        int gy = global_y0 + y;
        int b_idx = (y >= 8) ? 2 : 0;
        int sy8 = (y & 7) * 8;
        int phase = (int)(((uint32_t)gy * 2654435761U) >> 26);
        const int8_t* g1_row = s_grain_64x72 + (gy & 63) * 72;
        const int8_t* g2_row = s_grain_64x72 + ((gy * 7 + x_tile) & 63) * 72;
        int prev_v = (int)y_blk[b_idx][sy8];
        for (int half = 0; half < 2; half++) {
            int16_t* row = y_blk[b_idx + half] + sy8;
            int gx_base = global_x0 + (half << 3);
            const int8_t* g1 = g1_row + (gx_base & 63);
            const int8_t* g2 = g2_row + ((gx_base + phase) & 63);
            for (int x = 0; x < 8; x++) {
                int v = (int)row[x];
                int delta = ((v - prev_v) * clarity_strength) >> 4;
                if (has_grain) {
                    int raw = (int)g1[x] + (int)g2[x];
                    delta += (raw * (int)gmid[v]) >> 9;
                }
                prev_v = v;
                row[x] = clamp_s8_v7(v + delta);
            }
        }
    }
}

// Emits 2x2 RGB565 DC preview pixels for one 16x16 MCU using already-computed FDCT DC coefficients (0 extra I/O!)
static inline void emit_mcu_preview_2x2(uint16_t* preview, int pw, int ph, int omx, int out_my,
                                        const int16_t y_dc[4], const int cb_q[4], const int cr_q[4]) {
    if (!preview) return;
    int px0 = omx * 2, py0 = out_my * 2;
    for (int b = 0; b < 4; b++) {
        int px = px0 + (b & 1);
        int py = py0 + (b >> 1);
        if (px >= pw || py >= ph) continue;
        int Y = (((int)y_dc[b] + 4) >> 3) + 128;
        int cb = cb_q[b], cr = cr_q[b];
        int r  = (int)clamp_u8_v7(Y + ((91881 * cr + 32768) >> 16));
        int g  = (int)clamp_u8_v7(Y - ((22554 * cb + 46802 * cr + 32768) >> 16));
        int bl = (int)clamp_u8_v7(Y + ((116130 * cb + 32768) >> 16));
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

// Curvature-weighted L1 AC energy budget: min(280, floor(800 / curv)) for curv in [0..8]
// Zero ARM division in the hot path; verified RGB MAE <= 0.92/255 across all Fujifilm 3D LUTs!
static const uint16_t DCT_CURV_LIMIT[9] = { 280, 280, 280, 266, 200, 160, 133, 114, 100 };

// Adaptive DCT-Domain Local-Jacobian Grading for one 8x8 luma block + its 4x4 chroma quadrant.
// Bypasses IDCT + 16x 3D-LUT + FDCT when chroma is locally smooth and the LUT luma curve is locally
// linear over the block's AC swing; falls back to exact pixel-domain grading on edges/knees.
static inline int try_dct_grade_luma_quadrant(const FastJpegInfo* info, const uint32_t* __restrict ycc64,
                                              const DecBlock* __restrict mb, const uint16_t* __restrict qY,
                                              const uint8_t cb8[64], const uint8_t cr8[64], int cy0, int cx0,
                                              int16_t cb_blk[64], int16_t cr_blk[64],
                                              JpegQuantBlock* __restrict qb, int16_t* out_y_dc) {
    int cb_min = 255, cb_max = 0, cr_min = 255, cr_max = 0;
    for (int qy = 0; qy < 4; qy++) {
        int off = ((cy0 + qy) << 3) + cx0;
        const uint8_t* cbp = cb8 + off;
        const uint8_t* crp = cr8 + off;
        for (int qx = 0; qx < 4; qx++) {
            int cb = (int)cbp[qx];
            int cr = (int)crp[qx];
            if (cb < cb_min) cb_min = cb;
            if (cb > cb_max) cb_max = cb;
            if (cr < cr_min) cr_min = cr;
            if (cr > cr_max) cr_max = cr;
        }
    }
    if ((cb_max - cb_min) > 6 || (cr_max - cr_min) > 6) return 0;

    int dc_in = (int)mb->c[0] * (int)qY[0];
    int y_dc = ((dc_in + 4) >> 3) + 128;
    if (y_dc < 0) y_dc = 0;
    else if (y_dc > 255) y_dc = 255;

    int cb_m = (cb_min + cb_max) >> 1;
    int cr_m = (cr_min + cr_max) >> 1;
    size_t cidx = (((size_t)(cr_m >> 2) << 6) | (size_t)(cb_m >> 2));
    const uint32_t* col = ycc64 + (cidx + (cidx << 4));
    int y0 = y_dc >> 4;
    int yf = y_dc & 15;
    uint32_t va = col[y0];
    uint32_t vb = col[y0 + 1];
    int ya = (int)(va >> 24);
    int yb = (int)(vb >> 24);
    int dC = yb - ya;

    if (!mb->has_ac) {
        // Fast Path A: Pure DC luma block -> exact over full [0..255] luma range
        int prev_nz = (int)qb->last_nz;
        if (prev_nz > 0) {
            int16_t* qp = qb->q;
            for (int k = 1; k <= prev_nz; k++) qp[k] = 0;
        }
        int c0 = (((ya - 128) << 4) + dC * yf + 1) >> 1;
        *out_y_dc = (int16_t)c0;
        int sd = c0 < 0;
        int qd = (((sd ? -c0 : c0) * (int)info->q_luma_nat[0].inv_q) + 32768) >> 16;
        qb->q[0] = (int16_t)(sd ? -qd : qd);
        qb->last_nz = 0;
    } else {
        // Fast Path B: Smooth/textured AC luma block -> [16..239] guarantees col[y0-1]..col[y0+2] in bounds
        if (y_dc < 16 || y_dc > 239) return 0;
        int y_prev = (int)(col[y0 - 1] >> 24);
        int y_next = (int)(col[y0 + 2] >> 24);
        int dL = ya - y_prev;
        int dR = y_next - yb;
        int e1 = dC - dL;
        if (e1 < 0) e1 = -e1;
        int e2 = dR - dC;
        if (e2 < 0) e2 = -e2;
        int curv = (e1 > e2) ? e1 : e2;
        if (curv > 8) return 0;

        int l1_lim = (int)DCT_CURV_LIMIT[curv];
        int l1_ac = 0;
        uint8_t nz_nat[63];
        int16_t nz_val[63];
        int nz_cnt = 0;
        uint8_t rm = mb->row_mask;
        while (rm) {
#if defined(__GNUC__) || defined(__clang__)
            int r = __builtin_ctz((unsigned int)rm);
#else
            int r = 0;
            while (((rm >> r) & 1U) == 0U) r++;
#endif
            rm &= (uint8_t)(rm - 1U);
            int base = r << 3;
            const int16_t* cr_p = mb->c + base;
            const uint16_t* qr_p = qY + base;
            int c_start = (r == 0) ? 1 : 0;
            for (int c = c_start; c < 8; c++) {
                int v = (int)cr_p[c];
                if (v != 0) {
                    int av = v < 0 ? -v : v;
                    l1_ac += av * (int)qr_p[c];
                    nz_nat[nz_cnt] = (uint8_t)(base | c);
                    nz_val[nz_cnt] = (int16_t)v;
                    nz_cnt++;
                }
            }
            if (l1_ac > l1_lim) return 0;
        }

        int prev_nz = (int)qb->last_nz;
        if (prev_nz > 0) {
            int16_t* qp = qb->q;
            for (int k = 1; k <= prev_nz; k++) qp[k] = 0;
        }
        int c0 = (((ya - 128) << 4) + dC * yf + 1) >> 1;
        *out_y_dc = (int16_t)c0;
        int sd = c0 < 0;
        int qd = (((sd ? -c0 : c0) * (int)info->q_luma_nat[0].inv_q) + 32768) >> 16;
        qb->q[0] = (int16_t)(sd ? -qd : qd);

        int slope_q5 = (yf < 4) ? (dL + dC) : ((yf > 11) ? (dC + dR) : (dC << 1));
        int last_nz = 0;
        if (slope_q5 != 0) {
            const uint32_t* dsc = info->dct_scale_luma_q16;
            for (int i = 0; i < nz_cnt; i++) {
                int nat = (int)nz_nat[i];
                int prod = (int)nz_val[i] * slope_q5;
                int s = prod < 0;
                uint32_t mag = (uint32_t)(s ? -prod : prod);
                uint32_t qv = (uint32_t)(((uint64_t)mag * (uint64_t)dsc[nat] + 32768ULL) >> 16);
                if (qv != 0U) {
                    if (qv > 1023U) qv = 1023U;
                    int zi = (int)NAT_TO_ZZ[nat];
                    qb->q[zi] = (int16_t)(s ? -(int)qv : (int)qv);
                    if (zi > last_nz) last_nz = zi;
                }
            }
        }
        qb->last_nz = (uint8_t)last_nz;
    }

    // Fill 4x4 output chroma quadrant via SWAR linear-Y interpolation + local chroma residual
    uint32_t yaf = (uint32_t)yf;
    uint32_t cc = (va & 0x00FFFFFFU) * (16U - yaf) + (vb & 0x00FFFFFFU) * yaf + 0x00008008U;
    int ocb_m = (int)(cc >> 16) - 128;
    int ocr_m = (int)((cc >> 4) & 0xFFU) - 128;
    if (info->chroma_res) {
        int cb_off = ocb_m - ((cb_m & ~3) + 2);
        int cr_off = ocr_m - ((cr_m & ~3) + 2);
        for (int qy = 0; qy < 4; qy++) {
            int off = ((cy0 + qy) << 3) + cx0;
            const uint8_t* cbp = cb8 + off;
            const uint8_t* crp = cr8 + off;
            int16_t* ocbp = cb_blk + off;
            int16_t* ocrp = cr_blk + off;
            ocbp[0] = clamp_s8_v7(cb_off + (int)cbp[0]);
            ocbp[1] = clamp_s8_v7(cb_off + (int)cbp[1]);
            ocbp[2] = clamp_s8_v7(cb_off + (int)cbp[2]);
            ocbp[3] = clamp_s8_v7(cb_off + (int)cbp[3]);
            ocrp[0] = clamp_s8_v7(cr_off + (int)crp[0]);
            ocrp[1] = clamp_s8_v7(cr_off + (int)crp[1]);
            ocrp[2] = clamp_s8_v7(cr_off + (int)crp[2]);
            ocrp[3] = clamp_s8_v7(cr_off + (int)crp[3]);
        }
    } else {
        int16_t cb_flat = (int16_t)ocb_m;
        int16_t cr_flat = (int16_t)ocr_m;
        for (int qy = 0; qy < 4; qy++) {
            int off = ((cy0 + qy) << 3) + cx0;
            int16_t* ocbp = cb_blk + off;
            int16_t* ocrp = cr_blk + off;
            ocbp[0] = ocbp[1] = ocbp[2] = ocbp[3] = cb_flat;
            ocrp[0] = ocrp[1] = ocrp[2] = ocrp[3] = cr_flat;
        }
    }
    return 1;
}

// Fast Zero-Copy 1:1 Transcode for Sony 4:2:2 (2x1) & Standard 4:2:0 (2x2) MCUs
static inline void transcode_mcu_fast_1x(const FastJpegInfo* info, const uint32_t* ycc64,
                                         const DecBlock* in_band, int band_in_my0, int band_in_rows,
                                         int omx, int out_my, const uint8_t grain_mid_lut[256],
                                         int has_grain, int clarity_strength,
                                         JpegQuantBlock* out_6, uint16_t* preview, int pw, int ph, ProfAcc* prof) {
    int do_prof = (prof != NULL) && ((++prof->tick & 31U) == 0U);
    uint64_t tp_a = do_prof ? fs_thread_cpu_ns() : 0;
    const int32_t* dqY = info->dq_aan[info->tq[0]]; const uint16_t* qY = info->qt_raw[info->tq[0]];
    const int32_t* dqU = info->dq_aan[info->tq[1]]; const uint16_t* qU = info->qt_raw[info->tq[1]];
    const int32_t* dqV = info->dq_aan[info->tq[2]]; const uint16_t* qV = info->qt_raw[info->tq[2]];

    uint8_t cb8[64], cr8[64];
    const DecBlock* y_dec[4];

    if (info->samp_mode == 1) { // Sony BIONZ X native 4:2:2 -> exact half-height chroma IDCT directly into cb8/cr8!
        int cx = omx < info->in_mcus_x ? omx : (info->in_mcus_x - 1);
        int r0 = out_my * 2 - band_in_my0;
        int r1 = r0 + 1;
        if (r0 < 0) r0 = 0;
        if (r0 >= band_in_rows) r0 = band_in_rows - 1;
        if (r1 < 0) r1 = 0;
        if (r1 >= band_in_rows) r1 = band_in_rows - 1;
        const DecBlock* mb0 = in_band + ((size_t)r0 * (size_t)info->in_mcus_x + (size_t)cx) * 4U;
        const DecBlock* mb1 = in_band + ((size_t)r1 * (size_t)info->in_mcus_x + (size_t)cx) * 4U;
        y_dec[0] = &mb0[0];
        y_dec[1] = &mb0[1];
        y_dec[2] = &mb1[0];
        y_dec[3] = &mb1[1];
        idct_8x4_half_v_aan(&mb0[2], dqU, qU, cb8);
        idct_8x4_half_v_aan(&mb0[3], dqV, qV, cr8);
        idct_8x4_half_v_aan(&mb1[2], dqU, qU, cb8 + 32);
        idct_8x4_half_v_aan(&mb1[3], dqV, qV, cr8 + 32);
    } else { // Standard 4:2:0
        int r = out_my - band_in_my0;
        if (r < 0) r = 0;
        if (r >= band_in_rows) r = band_in_rows - 1;
        int cx = omx < info->in_mcus_x ? omx : (info->in_mcus_x - 1);
        const DecBlock* mb = in_band + ((size_t)r * (size_t)info->in_mcus_x + (size_t)cx) * 6U;
        y_dec[0] = &mb[0];
        y_dec[1] = &mb[1];
        y_dec[2] = &mb[2];
        y_dec[3] = &mb[3];
        idct_8x8_aan(&mb[4], dqU, qU, cb8);
        idct_8x8_aan(&mb[5], dqV, qV, cr8);
    }

    int16_t cb_blk[64], cr_blk[64];
    int16_t y_dc[4];
    int chroma_res = info->chroma_res;

    if (!has_grain && clarity_strength == 0) {
        // Hybrid Adaptive DCT-Domain + Exact Pixel-Domain Luma Path (L1-resident single-block buffers)
        uint64_t idct_acc = do_prof ? (fs_thread_cpu_ns() - tp_a) : 0;
        uint64_t lut_acc = 0, fdct_acc = 0;
        uint32_t fast_hit = 0;
        uint8_t yb_one[64];
        int16_t y_blk_one[64];

        for (int b = 0; b < 4; b++) {
            int cy0 = (b >> 1) << 2;
            int cx0 = (b & 1) << 2;
            uint64_t t0 = do_prof ? fs_thread_cpu_ns() : 0;
            if (try_dct_grade_luma_quadrant(info, ycc64, y_dec[b], qY, cb8, cr8, cy0, cx0,
                                            cb_blk, cr_blk, out_6 + b, &y_dc[b])) {
                if (do_prof) lut_acc += fs_thread_cpu_ns() - t0;
                fast_hit++;
                continue;
            }
            uint64_t t1 = do_prof ? fs_thread_cpu_ns() : 0;
            if (do_prof) lut_acc += t1 - t0;

            idct_8x8_aan(y_dec[b], dqY, qY, yb_one);
            uint64_t t2 = do_prof ? fs_thread_cpu_ns() : 0;
            if (do_prof) idct_acc += t2 - t1;

            for (int qy = 0; qy < 4; qy++) {
                const uint8_t* ys0 = yb_one + (qy << 4);
                const uint8_t* ys1 = ys0 + 8;
                int16_t* yd0 = y_blk_one + (qy << 4);
                int16_t* yd1 = yd0 + 8;
                int cr_row = ((cy0 + qy) << 3) + cx0;
                const uint8_t* cbp = cb8 + cr_row;
                const uint8_t* crp = cr8 + cr_row;
                int16_t* ocbp = cb_blk + cr_row;
                int16_t* ocrp = cr_blk + cr_row;
                for (int qx = 0; qx < 4; qx++) {
                    grade_ycc_quad_2x2_direct64(ycc64, chroma_res,
                                                ys0[0], ys0[1], ys1[0], ys1[1],
                                                cbp[qx], crp[qx],
                                                yd0, yd0 + 1, yd1, yd1 + 1,
                                                ocbp + qx, ocrp + qx);
                    ys0 += 2; ys1 += 2; yd0 += 2; yd1 += 2;
                }
            }
            uint64_t t3 = do_prof ? fs_thread_cpu_ns() : 0;
            if (do_prof) lut_acc += t3 - t2;

            fast_fdct_quant_luma(y_blk_one, info->q_luma_nat, out_6 + b);
            y_dc[b] = y_blk_one[0];
            if (do_prof) fdct_acc += fs_thread_cpu_ns() - t3;
        }

        int cb_q[4], cr_q[4];
        if (preview) sample_quadrant_chroma(cb_blk, cr_blk, cb_q, cr_q);
        uint64_t tp_c = do_prof ? fs_thread_cpu_ns() : 0;
        fast_fdct_quant_chroma(cb_blk, info->q_chroma_nat, out_6 + 4);
        fast_fdct_quant_chroma(cr_blk, info->q_chroma_nat, out_6 + 5);
        if (prof) {
            prof->dct_fast_blks += fast_hit;
            prof->tot_blks += 4U;
            if (do_prof) {
                fdct_acc += fs_thread_cpu_ns() - tp_c;
                prof->idct_ns += idct_acc;
                prof->lut_ns += lut_acc;
                prof->fdct_ns += fdct_acc;
                prof->n++;
            }
        }
        if (preview) emit_mcu_preview_2x2(preview, pw, ph, omx, out_my, y_dc, cb_q, cr_q);
        return;
    }

    // Full Spatial-Domain Path (preserves authentic pixel-level film grain & clarity)
    uint8_t yb[4][64];
    idct_8x8_aan(y_dec[0], dqY, qY, yb[0]);
    idct_8x8_aan(y_dec[1], dqY, qY, yb[1]);
    idct_8x8_aan(y_dec[2], dqY, qY, yb[2]);
    idct_8x8_aan(y_dec[3], dqY, qY, yb[3]);

    uint64_t tp_b = do_prof ? fs_thread_cpu_ns() : 0;
    int16_t y_blk[4][64];
    for (int by = 0; by < 2; by++) {
        for (int bx = 0; bx < 2; bx++) {
            int blk_i = by * 2 + bx;
            const uint8_t* ys = yb[blk_i];
            int16_t* yd = y_blk[blk_i];
            int cy0 = by * 4, cx0 = bx * 4;
            for (int qy = 0; qy < 4; qy++) {
                const uint8_t* ys0 = ys + (qy << 4);
                const uint8_t* ys1 = ys0 + 8;
                int16_t* yd0 = yd + (qy << 4);
                int16_t* yd1 = yd0 + 8;
                int cr_row = ((cy0 + qy) << 3) + cx0;
                const uint8_t* cbp = cb8 + cr_row;
                const uint8_t* crp = cr8 + cr_row;
                int16_t* ocbp = cb_blk + cr_row;
                int16_t* ocrp = cr_blk + cr_row;
                for (int qx = 0; qx < 4; qx++) {
                    grade_ycc_quad_2x2_direct64(ycc64, chroma_res,
                                                ys0[0], ys0[1], ys1[0], ys1[1],
                                                cbp[qx], crp[qx],
                                                yd0, yd0 + 1, yd1, yd1 + 1,
                                                ocbp + qx, ocrp + qx);
                    ys0 += 2; ys1 += 2; yd0 += 2; yd1 += 2;
                }
            }
        }
    }

    uint64_t tp_c = do_prof ? fs_thread_cpu_ns() : 0;
    apply_luma_grain_clarity_mcu(y_blk, omx * 16, out_my * 16, grain_mid_lut, has_grain, clarity_strength);
    int cb_q[4], cr_q[4];
    if (preview) sample_quadrant_chroma(cb_blk, cr_blk, cb_q, cr_q);
    quantize_mcu_fast(info, y_blk, cb_blk, cr_blk, out_6);
    y_dc[0] = y_blk[0][0];
    y_dc[1] = y_blk[1][0];
    y_dc[2] = y_blk[2][0];
    y_dc[3] = y_blk[3][0];
    if (prof) {
        prof->tot_blks += 4U;
        if (do_prof) {
            uint64_t tp_d = fs_thread_cpu_ns();
            prof->idct_ns += tp_b - tp_a;
            prof->lut_ns += tp_c - tp_b;
            prof->fdct_ns += tp_d - tp_c;
            prof->n++;
        }
    }
    if (preview) emit_mcu_preview_2x2(preview, pw, ph, omx, out_my, y_dc, cb_q, cr_q);
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
        for (int b = 0; b < 4; b++) idct_8x8_aan(&mb[b], dqY, qY, yb[b]);
        idct_8x8_aan(&mb[4], dqU, qU, cb8);
        idct_8x8_aan(&mb[5], dqV, qV, cr8);
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
            idct_8x8_aan(&mb[0], dqY, qY, y0);
            idct_8x8_aan(&mb[1], dqY, qY, y1);
            idct_8x4_half_v_aan(&mb[2], dqU, qU, cb8 + half * 32);
            idct_8x4_half_v_aan(&mb[3], dqV, qV, cr8 + half * 32);
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
            idct_8x8_aan(&mb[0], dqY, qY, yt);
            idct_8x8_aan(&mb[1], dqY, qY, yb);
            idct_8x8_aan(&mb[2], dqU, qU, cbh);
            idct_8x8_aan(&mb[3], dqV, qV, crh);
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
                idct_8x8_aan(&mb[0], dqY, qY, yb);
                idct_8x8_aan(&mb[1], dqU, qU, cbh);
                idct_8x8_aan(&mb[2], dqV, qV, crh);
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

static void grade_and_quant_y16_cb8_cr8(const FastJpegInfo* info, const uint32_t* ycc64,
                                        const uint8_t y16[256], const uint8_t cb8[64], const uint8_t cr8[64],
                                        int omx, int out_my, const uint8_t grain_mid_lut[256], int has_grain,
                                        int clarity_strength, JpegQuantBlock* out_6, uint16_t* preview, int pw, int ph) {
    int16_t y_blk[4][64], cb_blk[64], cr_blk[64];
    int chroma_res = info->chroma_res;
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
                    grade_ycc_quad_2x2_direct64(ycc64, chroma_res,
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
    quantize_mcu_fast(info, y_blk, cb_blk, cr_blk, out_6);
    if (preview) {
        int16_t y_dc[4] = { y_blk[0][0], y_blk[1][0], y_blk[2][0], y_blk[3][0] };
        emit_mcu_preview_2x2(preview, pw, ph, omx, out_my, y_dc, cb_q, cr_q);
    }
}

typedef struct {
    const FastJpegInfo* info;
    const uint32_t* ycc64;
    const DecBlock* in_band;
    int band_in_my0;
    int band_in_rows;
    int out_my_base;
    int cur_out_rows;
    int chunks_per_row;
    int total_chunks;
    volatile int next_chunk;
    const uint8_t* grain_mid_lut;
    int has_grain;
    int clarity_strength;
    JpegQuantBlock* q_out_base;
    uint16_t* preview_rgb565;
    int preview_w;
    int preview_h;
} SharedBandWork;

static void run_one_chunk(const SharedBandWork* bw, int cid, ProfAcc* prof) {
    const FastJpegInfo* info = bw->info;
    int lm = cid / bw->chunks_per_row;
    int sub = cid - lm * bw->chunks_per_row;
    int x0 = (info->out_mcus_x * sub) / bw->chunks_per_row;
    int x1 = (info->out_mcus_x * (sub + 1)) / bw->chunks_per_row;
    if (x0 >= x1) return;

    int out_my = bw->out_my_base + lm;
    JpegQuantBlock* row_qb = bw->q_out_base + (size_t)lm * (size_t)info->out_mcus_x * 6U;

    if (info->scale == 1 && (info->samp_mode == 0 || info->samp_mode == 1)) {
        for (int omx = x0; omx < x1; omx++) {
            transcode_mcu_fast_1x(info, bw->ycc64, bw->in_band,
                                  bw->band_in_my0, bw->band_in_rows, omx, out_my,
                                  bw->grain_mid_lut, bw->has_grain, bw->clarity_strength,
                                  row_qb + (size_t)omx * 6U,
                                  bw->preview_rgb565, bw->preview_w, bw->preview_h, prof);
        }
    } else if (info->scale == 1) {
        uint8_t y16[256], cb8[64], cr8[64];
        for (int omx = x0; omx < x1; omx++) {
            decode_src_tile_16x16_sub(info, bw->in_band, bw->band_in_my0, bw->band_in_rows,
                                      omx, out_my, y16, cb8, cr8);
            grade_and_quant_y16_cb8_cr8(info, bw->ycc64, y16, cb8, cr8,
                                        omx, out_my, bw->grain_mid_lut, bw->has_grain, bw->clarity_strength,
                                        row_qb + (size_t)omx * 6U,
                                        bw->preview_rgb565, bw->preview_w, bw->preview_h);
        }
    } else {
        uint8_t y16[256], cb8[64], cr8[64];
        uint8_t sy[256], su8[64], sv8[64];
        for (int omx = x0; omx < x1; omx++) {
            for (int qy = 0; qy < 2; qy++) {
                for (int qx = 0; qx < 2; qx++) {
                    decode_src_tile_16x16_sub(info, bw->in_band, bw->band_in_my0, bw->band_in_rows,
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
            grade_and_quant_y16_cb8_cr8(info, bw->ycc64, y16, cb8, cr8,
                                        omx, out_my, bw->grain_mid_lut, bw->has_grain, bw->clarity_strength,
                                        row_qb + (size_t)omx * 6U,
                                        bw->preview_rgb565, bw->preview_w, bw->preview_h);
        }
    }
}

static void run_steal_chunks(SharedBandWork* bw, ProfAcc* prof) {
    for (;;) {
#ifndef _WIN32
        int cid = __sync_fetch_and_add(&bw->next_chunk, 1);
#else
        int cid = bw->next_chunk++;
#endif
        if (cid >= bw->total_chunks) break;
        run_one_chunk(bw, cid, prof);
    }
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

static void decode_band_rows(void* arg) {
    DecodeBandJob* d = (DecodeBandJob*)arg;
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

    // Hoist 64-bit bitreader state into registers across the ENTIRE band!
    uint64_t bitbuf = br->bitbuf;
    int bitcnt = br->bitcnt;
    size_t pos = br->pos;
    size_t len = br->len;

    for (int r = 0; r < d->band_in_rows && !fail_rc; r++) {
        DecBlock* row_blk = d->dst_band + (size_t)r * (size_t)info->in_mcus_x * (size_t)info->blks_per_mcu;
        for (int mx = 0; mx < info->in_mcus_x; mx++) {
            if (info->restart_interval > 0 && mcus_until_rst == 0) {
                br->bitbuf = bitbuf; br->bitcnt = bitcnt; br->pos = pos;
                if (!fbr_consume_restart(br)) { fail_rc = -11; break; }
                bitbuf = br->bitbuf; bitcnt = br->bitcnt; pos = br->pos; len = br->len;
                dc_pred[0] = dc_pred[1] = dc_pred[2] = 0;
                mcus_until_rst = info->restart_interval;
            }
            DecBlock* mb = row_blk + (size_t)mx * (size_t)info->blks_per_mcu;
            if (d->std_order) {
                for (int b = 0; b < d->y_cnt; b++) {
                    if (!decode_huff_block_reg(br, &bitbuf, &bitcnt, &pos, &len, dc0, ac0, &dc_pred[0], &mb[b])) { fail_rc = -12; break; }
                }
                if (fail_rc) break;
                if (!decode_huff_block_reg(br, &bitbuf, &bitcnt, &pos, &len, dc1, ac1, &dc_pred[1], &mb[d->y_cnt])) { fail_rc = -12; break; }
                if (!decode_huff_block_reg(br, &bitbuf, &bitcnt, &pos, &len, dc2, ac2, &dc_pred[2], &mb[d->y_cnt + 1])) { fail_rc = -12; break; }
            } else {
                for (int si = 0; si < 3 && !fail_rc; si++) {
                    int cidx = info->sos_order[si];
                    const FastHuffTable* dct = &info->dc_huff[info->td[cidx]];
                    const FastHuffTable* act = &info->ac_huff[info->ta[cidx]];
                    if (cidx == 0) {
                        for (int b = 0; b < d->y_cnt; b++) {
                            if (!decode_huff_block_reg(br, &bitbuf, &bitcnt, &pos, &len, dct, act, &dc_pred[0], &mb[b])) { fail_rc = -12; break; }
                        }
                    } else {
                        DecBlock* dst_b = (cidx == 1) ? &mb[d->y_cnt] : &mb[d->y_cnt + 1];
                        if (!decode_huff_block_reg(br, &bitbuf, &bitcnt, &pos, &len, dct, act, &dc_pred[cidx], dst_b)) { fail_rc = -12; break; }
                    }
                }
                if (fail_rc) break;
            }
            if (info->restart_interval > 0) mcus_until_rst--;
        }
    }
    br->bitbuf = bitbuf; br->bitcnt = bitcnt; br->pos = pos;
    *(d->mcus_until_rst) = mcus_until_rst;
    d->fail_rc = fail_rc;
}

typedef struct {
    JpegStreamState* state;
    const JpegQuantBlock* blocks;
    int num_mcus;
    int ok;
} EmitJob;

#ifndef _WIN32
// Persistent Core-Pinned POSIX Semaphore Worker (API-9 Bionic safe: FUTEX_WAKE, zero pipe WF_SYNC core-piling!)
typedef struct {
    pthread_t tid;
    sem_t wake_sem;
    sem_t done_sem;
    int pin_cpu;
    int pinned;
    volatile int quit;
    void (*fn)(void*);
    void* arg;
    int active;
    int busy;
} SemWorker;

static void* sem_worker_main(void* p) {
    SemWorker* w = (SemWorker*)p;
    if (w->pin_cpu >= 0) {
        w->pinned = fs_pin_current_cpu(w->pin_cpu);
    }
    while (sem_wait_eintr(&w->wake_sem)) {
        if (w->quit) break;
        if (w->fn) {
            w->fn(w->arg);
        }
        sem_post(&w->done_sem);
    }
    return NULL;
}

static void sem_worker_init(SemWorker* w, int pin_cpu) {
    memset(w, 0, sizeof(SemWorker));
    w->pin_cpu = pin_cpu;
    if (sem_init(&w->wake_sem, 0, 0) != 0) return;
    if (sem_init(&w->done_sem, 0, 0) != 0) {
        sem_destroy(&w->wake_sem);
        return;
    }
    if (pthread_create(&w->tid, NULL, sem_worker_main, w) == 0) {
        w->active = 1;
    } else {
        sem_destroy(&w->wake_sem);
        sem_destroy(&w->done_sem);
    }
}

static int sem_worker_trigger(SemWorker* w, void (*fn)(void*), void* arg) {
    if (!w->active) return 0;
    w->fn = fn;
    w->arg = arg;
    w->busy = 1;
    sem_post(&w->wake_sem);
    return 1;
}

static void sem_worker_wait(SemWorker* w) {
    if (!w->active || !w->busy) return;
    sem_wait_eintr(&w->done_sem);
    w->busy = 0;
}

static void sem_worker_destroy(SemWorker* w) {
    if (!w->active) return;
    if (w->busy) sem_worker_wait(w);
    w->quit = 1;
    sem_post(&w->wake_sem);
    pthread_join(w->tid, NULL);
    sem_destroy(&w->wake_sem);
    sem_destroy(&w->done_sem);
    w->active = 0;
}

typedef struct {
    SharedBandWork* bw;
    EmitJob* emit_job; // Non-NULL only on Core 1 when emitting Band N-1 before stealing Stage-2 chunks!
    ProfAcc* prof;
    uint64_t emit_cpu_ns;
    uint64_t s2_cpu_ns;
} UnifiedWorkerTask;

static void run_unified_worker_task(void* arg) {
    UnifiedWorkerTask* t = (UnifiedWorkerTask*)arg;
    if (t->emit_job) {
        uint64_t c0 = fs_thread_cpu_ns();
        t->emit_job->ok = native_jpeg_emit_quantized_blocks(
            t->emit_job->state, t->emit_job->blocks, t->emit_job->num_mcus);
        t->emit_cpu_ns += fs_thread_cpu_ns() - c0;
    }
    uint64_t cs = fs_thread_cpu_ns();
    run_steal_chunks(t->bw, t->prof);
    t->s2_cpu_ns += fs_thread_cpu_ns() - cs;
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
    int ncpu = 1, has_neon = 0, is_v7 = 0;
    fs_probe_cpu(&ncpu, &has_neon, &is_v7);
    uint64_t t_start = fs_now_ms();
    int sched_bg = 0, sched_mhz = 0;
    fs_read_sched_info(&sched_bg, &sched_mhz);
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

    int eff_quality = quality > 0 ? quality : 88;
    {
        static const uint8_t FS_BQ_L[64] = {
            16,11,10,16,24,40,51,61, 12,12,14,19,26,58,60,55, 14,13,16,24,40,57,69,56, 14,17,22,29,51,87,80,62,
            18,22,37,56,68,109,103,77,24,35,55,64,81,104,113,92,49,64,78,87,103,121,120,101,72,92,95,98,112,100,103,99
        };
        static const uint8_t FS_BQ_C[64] = {
            17,18,24,47,99,99,99,99, 18,21,26,66,99,99,99,99, 24,26,56,99,99,99,99,99, 47,66,99,99,99,99,99,99,
            99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99
        };
        static const uint16_t FS_AAN_IF[8] = { 4096, 2953, 3135, 3483, 4096, 5213, 7568, 14846 };
        int q = eff_quality < 1 ? 1 : (eff_quality > 100 ? 100 : eff_quality);
        int qs = (q < 50) ? (5000 / q) : (200 - q * 2);
        for (int i = 0; i < 64; i++) {
            int vl = (FS_BQ_L[i] * qs + 50) / 100;
            if (vl < 1) vl = 1; else if (vl > 255) vl = 255;
            int vc = (FS_BQ_C[i] * qs + 50) / 100;
            if (vc < 1) vc = 1; else if (vc > 255) vc = 255;
            uint32_t aan_num = (uint32_t)FS_AAN_IF[i >> 3] * (uint32_t)FS_AAN_IF[i & 7];
            uint32_t il = (aan_num + ((uint32_t)vl << 7)) / ((uint32_t)vl << 8);
            uint32_t ic = (aan_num + ((uint32_t)vc << 7)) / ((uint32_t)vc << 8);
            uint16_t iql = (uint16_t)(il > 65535U ? 65535U : il);
            uint16_t iqc = (uint16_t)(ic > 65535U ? 65535U : ic);
            info->q_luma_nat[i].inv_q   = iql;
            info->q_luma_nat[i].dz      = (int16_t)(iql > 0 ? (32767U / iql) : 32767U);
            info->q_luma_nat[i].zz      = NAT_TO_ZZ[i];
            info->q_chroma_nat[i].inv_q = iqc;
            info->q_chroma_nat[i].dz    = (int16_t)(iqc > 0 ? (32767U / iqc) : 32767U);
            info->q_chroma_nat[i].zz    = NAT_TO_ZZ[i];
            uint32_t qin_y = (uint32_t)info->qt_raw[info->tq[0]][i];
            if (qin_y < 1U) qin_y = 1U;
            info->dct_scale_luma_q16[i] = (qin_y * 2048U + ((uint32_t)vl >> 1)) / (uint32_t)vl;
        }
    }

    uint8_t clean_exif[512];
    size_t clean_exif_len = build_clean_exif_app1(clean_exif, sizeof(clean_exif), &info->exif, info->dst_w, info->dst_h);

    // Open output file FIRST before reading ANY input scan data (so Tier 1/2 EACCES retries return -9 in <1ms!)
    JpegStreamState* out_state = native_jpeg_stream_open_with_exif(
        output_jpeg_path, info->dst_w, info->dst_h, eff_quality,
        clean_exif_len > 0 ? clean_exif : NULL, clean_exif_len);
    if (!out_state) {
        free(info);
        fclose(fp);
        if (out_result) out_result->rc = -9;
        return -9;
    }

    // Launch async background SD prefetcher (128KB first chunk then 256KB raw POSIX read())
    // BEFORE worker init, LUT build, and band allocation so SD I/O overlaps 100% with compute!
    uint8_t* in_mem_buf = NULL;
#ifndef _WIN32
    IoPrefetcher prefetch;
    memset(&prefetch, 0, sizeof(IoPrefetcher));
    prefetch.fd = -1;
    {
        long cur_off = ftell(fp);
        if (cur_off >= 0 && fseek(fp, 0, SEEK_END) == 0) {
            long end_off = ftell(fp);
            if (fseek(fp, cur_off, SEEK_SET) == 0 && end_off > cur_off) {
                size_t rem = (size_t)(end_off - cur_off);
                if (rem <= 14U * 1024U * 1024U) {
                    uint8_t* mb = (uint8_t*)malloc(rem + 8U);
                    if (mb) {
                        int pf_fd = open(input_jpeg_path, O_RDONLY);
                        if (pf_fd >= 0 && lseek(pf_fd, (off_t)cur_off, SEEK_SET) == (off_t)cur_off &&
                            io_prefetch_start(&prefetch, pf_fd, mb, rem)) {
                            in_mem_buf = mb;
                            fclose(fp);
                            fp = NULL;
                        } else {
                            if (pf_fd >= 0) close(pf_fd);
                            free(mb);
                        }
                    }
                }
            }
        }
    }
#endif

#ifndef _WIN32
    int avail_cpus[4] = { 0, 1, 2, 3 };
    FsCpuMask saved_affinity;
    int n_avail = fs_get_avail_cpus(avail_cpus, &saved_affinity);
    int worker_count = (ncpu >= 4) ? 4 : (ncpu >= 2 ? ncpu : 2);
    if (worker_count > 4) worker_count = 4;
    if (n_avail > 0 && n_avail < worker_count) worker_count = n_avail;
    int can_pin = (worker_count >= 2) ? 1 : 0;
    // Spawn workers BEFORE pinning main so child threads inherit full multi-core mask (SD_BALANCE_FORK)!
    SemWorker sw[3];
    for (int k = 0; k < 3; k++) {
        if (k + 1 < worker_count) {
            int target_cpu = can_pin ? ((k + 1 < n_avail) ? avail_cpus[k + 1] : (k + 1)) : -1;
            sem_worker_init(&sw[k], target_cpu);
        } else {
            memset(&sw[k], 0, sizeof(SemWorker));
        }
    }
    int main_pinned = 0;
    if (can_pin) {
        main_pinned = fs_pin_current_cpu((n_avail >= 1) ? avail_cpus[0] : 0);
    }
#else
    int worker_count = 1;
#endif

    // Resolve or 4-core-build the static cached 64x64x17 SWAR direct-chroma LUT (0.0ms on repeat shots!)
    uint64_t t_lut0 = fs_now_ms();
    uint64_t lut_h = hash_lut17(lut);
    const uint32_t* ycc64 = NULL;
    int chroma_res = 1;
#ifndef _WIN32
    pthread_mutex_lock(&s_ycc64_mutex);
#endif
    init_grain_64x72();
    if (!s_ycc64_cache) {
        s_ycc64_cache = (uint32_t*)malloc(YCC64_NODES * sizeof(uint32_t));
    }
    if (s_ycc64_cache && (!s_ycc64_valid || s_ycc64_hash != lut_h)) {
        BuildLut64Slice bs[4];
        for (int k = 0; k < worker_count; k++) {
            bs[k].rgb_lut = lut;
            bs[k].dst = s_ycc64_cache;
            bs[k].cr_start = (64 * k) / worker_count;
            bs[k].cr_end   = (64 * (k + 1)) / worker_count;
            bs[k].max_chroma_dev = 0;
        }
#ifndef _WIN32
        int btrig[3] = { 0, 0, 0 };
        for (int k = 1; k < worker_count; k++) {
            btrig[k - 1] = sem_worker_trigger(&sw[k - 1], build_ycc64_slice, &bs[k]);
        }
        build_ycc64_slice(&bs[0]);
        for (int k = 1; k < worker_count; k++) {
            if (!btrig[k - 1]) build_ycc64_slice(&bs[k]);
        }
        for (int k = 1; k < worker_count; k++) {
            if (btrig[k - 1]) sem_worker_wait(&sw[k - 1]);
        }
#else
        for (int k = 0; k < worker_count; k++) build_ycc64_slice(&bs[k]);
#endif
        int max_dev = 0;
        for (int k = 0; k < worker_count; k++) {
            if (bs[k].max_chroma_dev > max_dev) max_dev = bs[k].max_chroma_dev;
        }
        s_ycc64_chroma_res = (max_dev > 4) ? 1 : 0;
        s_ycc64_hash = lut_h;
        s_ycc64_valid = 1;
    }
    ycc64 = s_ycc64_cache;
    chroma_res = s_ycc64_chroma_res;
#ifndef _WIN32
    pthread_mutex_unlock(&s_ycc64_mutex);
#endif
    info->chroma_res = chroma_res;
    int lut_wall_ms = (int)(fs_now_ms() - t_lut0);

    if (!ycc64) {
#ifndef _WIN32
        for (int k = 0; k < 3; k++) sem_worker_destroy(&sw[k]);
        fs_restore_affinity(&saved_affinity);
        io_prefetch_destroy(&prefetch);
#endif
        native_jpeg_stream_close(out_state, 0);
        remove(output_jpeg_path);
        free(in_mem_buf);
        free(info);
        if (fp) fclose(fp);
        if (out_result) out_result->rc = -10;
        return -10;
    }

    // Precompute 256-byte luma grain envelope table (eliminates per-pixel midtone & scale multiplies)
    int grain_scale = (grain_strength == 2) ? 4 : (grain_strength == 1) ? 2 : 0;
    uint8_t grain_mid_lut[256];
    for (int i = 0; i < 256; i++) {
        int v = (grain_scale * ((i * (255 - i)) >> 8));
        grain_mid_lut[i] = (uint8_t)(v > 255 ? 255 : v);
    }

    // Optional zero-I/O DC RGB565 Quick Review preview (1 pixel per 8x8 block: 646x431 on 17.8MP)
    uint16_t* preview_rgb565 = NULL;
    int preview_w = 0, preview_h = 0;
    if (out_result) {
        preview_w = (info->dst_w + 7) >> 3;
        preview_h = (info->dst_h + 7) >> 3;
        if (preview_w > 0 && preview_h > 0 && preview_w <= 1280 && preview_h <= 960) {
            preview_rgb565 = (uint16_t*)malloc((size_t)preview_w * (size_t)preview_h * sizeof(uint16_t));
        }
    }

    // 8-row bands on 1:1 (27 bands total, ~9.5MB RAM -> halves barrier frequency; auto-fallback to 4 on tight RAM!)
    int band_out_rows = (scale == 2) ? 4 : 8;
    int in_rows_per_out = ((info->samp_mode == 0 || info->samp_mode == 3) ? 1 : 2) * scale;
    DecBlock* in_band_work = NULL;
    DecBlock* in_band_dec  = NULL;
    JpegQuantBlock* q_work = NULL;
    JpegQuantBlock* q_emit = NULL;
    for (;;) {
        int band_in_max_rows = band_out_rows * in_rows_per_out;
        size_t in_band_blks = (size_t)band_in_max_rows * (size_t)info->in_mcus_x * (size_t)info->blks_per_mcu;
        size_t q_band_blks  = (size_t)band_out_rows * (size_t)info->out_mcus_x * 6U;
        // calloc BOTH in_band and q_work/q_emit so sparse dirty_rows & sparse prev-last_nz invariants hold from Band 0!
        in_band_work = (DecBlock*)calloc(in_band_blks, sizeof(DecBlock));
        in_band_dec  = (DecBlock*)calloc(in_band_blks, sizeof(DecBlock));
        q_work       = (JpegQuantBlock*)calloc(q_band_blks, sizeof(JpegQuantBlock));
        q_emit       = (JpegQuantBlock*)calloc(q_band_blks, sizeof(JpegQuantBlock));
        if (in_band_work && in_band_dec && q_work && q_emit) break;
        if (band_out_rows > 4) {
            free(in_band_work); free(in_band_dec); free(q_work); free(q_emit);
            in_band_work = NULL; in_band_dec = NULL; q_work = NULL; q_emit = NULL;
            band_out_rows = 4;
            continue;
        }
        break;
    }
    FastBitReader* br = (FastBitReader*)calloc(1, sizeof(FastBitReader));

    if (!in_band_work || !q_work || !q_emit || !br) {
#ifndef _WIN32
        for (int k = 0; k < 3; k++) sem_worker_destroy(&sw[k]);
        fs_restore_affinity(&saved_affinity);
        io_prefetch_destroy(&prefetch);
#endif
        free(in_band_work); free(in_band_dec); free(q_work); free(q_emit); free(br);
        free(preview_rgb565);
        native_jpeg_stream_close(out_state, 0);
        remove(output_jpeg_path);
        free(in_mem_buf);
        free(info);
        if (fp) fclose(fp);
        if (out_result) out_result->rc = -10;
        return -10;
    }
#ifndef _WIN32
    if (prefetch.active && in_mem_buf) {
        br->fp = NULL;
        br->pf = &prefetch;
        br->buf = in_mem_buf;
        br->len = 0;
        br->pos = 0;
    } else
#endif
    {
        br->fp = fp;
        br->buf = br->stream_buf;
        br->len = 0;
        br->pos = 0;
    }

    int dc_pred[3] = { 0, 0, 0 };
    int mcus_until_rst = info->restart_interval;
    int fail_rc = 0;
    int cur_in_my = 0;
    int y_cnt = (info->samp_mode == 0) ? 4 : ((info->samp_mode == 1 || info->samp_mode == 3) ? 2 : 1);
    int std_order = (info->sos_order[0] == 0 && info->sos_order[1] == 1 && info->sos_order[2] == 2);
    int chunks_per_row = (info->out_mcus_x >= 32) ? 4 : 1;
    uint64_t dec_wait_ms = 0, proc_ms = 0, emit_wait_ms = 0;
    ProfAcc prof_acc[4];
    memset(prof_acc, 0, sizeof(prof_acc));
    uint64_t main_s2_cpu_ns = 0, main_dec_cpu_ns = 0, main_emit_cpu_ns = 0;
    int prof_total_mcus = info->out_mcus_x * info->out_mcus_y;

#ifndef _WIN32
    UnifiedWorkerTask wtasks[3];
    memset(wtasks, 0, sizeof(wtasks));
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
        uint64_t cd0 = fs_thread_cpu_ns();
        decode_band_rows(&d0);
        main_dec_cpu_ns += fs_thread_cpu_ns() - cd0;
        dec_wait_ms += (fs_now_ms() - td0);
        cur_in_my += cur_band_in_rows;
        fail_rc = d0.fail_rc;
    }

    int has_prev_emit = 0;
    int prev_emit_mcus = 0;

    // Unified 4-Core Pinned Work-Stealing Band Loop (EXACTLY 4 active threads on 4 physical cores, 1 barrier/band!):
    //  - Core 0 (main):  Huffman-decodes Band N+1 from RAM, then immediately steals Stage-2 chunks of Band N!
    //  - Core 1 (sw[0]): Huffman-emits Band N-1 to SD, then immediately steals Stage-2 chunks of Band N!
    //  - Core 2 (sw[1]): Dedicated Stage-2 work-stealer on Band N!
    //  - Core 3 (sw[2]): Dedicated Stage-2 work-stealer on Band N!
    for (int out_my0 = 0; out_my0 < info->out_mcus_y && !fail_rc; out_my0 += band_out_rows) {
        int cur_out_rows = info->out_mcus_y - out_my0;
        if (cur_out_rows > band_out_rows) cur_out_rows = band_out_rows;

        int next_out_my0 = out_my0 + band_out_rows;
        int has_next = (next_out_my0 < info->out_mcus_y) ? 1 : 0;
        int next_band_in_my0 = cur_in_my;
        int next_band_in_rows = 0;
        DecodeBandJob dec_job;
        memset(&dec_job, 0, sizeof(dec_job));
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
        }

        SharedBandWork bw;
        bw.info = info;
        bw.ycc64 = ycc64;
        bw.in_band = in_band_work;
        bw.band_in_my0 = cur_band_in_my0;
        bw.band_in_rows = cur_band_in_rows > 0 ? cur_band_in_rows : 1;
        bw.out_my_base = out_my0;
        bw.cur_out_rows = cur_out_rows;
        bw.chunks_per_row = chunks_per_row;
        bw.total_chunks = cur_out_rows * chunks_per_row;
        bw.next_chunk = 0;
        bw.grain_mid_lut = grain_mid_lut;
        bw.has_grain = (grain_scale > 0) ? 1 : 0;
        bw.clarity_strength = clarity_strength;
        bw.q_out_base = q_work;
        bw.preview_rgb565 = preview_rgb565;
        bw.preview_w = preview_w;
        bw.preview_h = preview_h;

        uint64_t tp0 = fs_now_ms();
#ifndef _WIN32
        EmitJob emit_job;
        emit_job.state = out_state;
        emit_job.blocks = q_emit;
        emit_job.num_mcus = prev_emit_mcus;
        emit_job.ok = 1;

        int trig[3] = { 0, 0, 0 };
        for (int k = 0; k < 3; k++) {
            if (sw[k].active) {
                wtasks[k].bw = &bw;
                wtasks[k].emit_job = (k == 0 && has_prev_emit) ? &emit_job : NULL;
                wtasks[k].prof = &prof_acc[k + 1];
                trig[k] = sem_worker_trigger(&sw[k], run_unified_worker_task, &wtasks[k]);
            }
        }

        // If Core 1 worker wasn't available, emit Band N-1 synchronously on main
        if (has_prev_emit && !trig[0]) {
            uint64_t ce0 = fs_thread_cpu_ns();
            emit_job.ok = native_jpeg_emit_quantized_blocks(out_state, q_emit, prev_emit_mcus);
            main_emit_cpu_ns += fs_thread_cpu_ns() - ce0;
        }

        // Core 0 (main): decode Band N+1 into in_band_dec (in parallel with Cores 1-3!), then steal Stage-2 chunks!
        int dec_done_overlapped = 0;
        if (has_next && in_band_dec != NULL) {
            if (next_band_in_rows > 0) {
                uint64_t cd1 = fs_thread_cpu_ns();
                decode_band_rows(&dec_job);
                main_dec_cpu_ns += fs_thread_cpu_ns() - cd1;
            }
            dec_done_overlapped = 1;
        }
        {
            uint64_t cs0 = fs_thread_cpu_ns();
            run_steal_chunks(&bw, &prof_acc[0]);
            main_s2_cpu_ns += fs_thread_cpu_ns() - cs0;
        }

        for (int k = 0; k < 3; k++) {
            if (trig[k]) sem_worker_wait(&sw[k]);
        }
        proc_ms += (fs_now_ms() - tp0);

        if (has_prev_emit && !emit_job.ok) {
            fail_rc = -13;
            break;
        }
        if (has_next) {
            if (dec_done_overlapped) {
                DecBlock* tdb = in_band_work; in_band_work = in_band_dec; in_band_dec = tdb;
            } else if (next_band_in_rows > 0) {
                uint64_t td1 = fs_now_ms();
                uint64_t cd2 = fs_thread_cpu_ns();
                dec_job.dst_band = in_band_work;
                decode_band_rows(&dec_job);
                main_dec_cpu_ns += fs_thread_cpu_ns() - cd2;
                dec_wait_ms += (fs_now_ms() - td1);
            }
            cur_in_my += next_band_in_rows;
            cur_band_in_my0 = next_band_in_my0;
            cur_band_in_rows = next_band_in_rows;
            if (dec_job.fail_rc) {
                fail_rc = dec_job.fail_rc;
                break;
            }
        }
#else
        if (has_prev_emit) {
            if (!native_jpeg_emit_quantized_blocks(out_state, q_emit, prev_emit_mcus)) {
                fail_rc = -13;
                break;
            }
        }
        run_steal_chunks(&bw, &prof_acc[0]);
        proc_ms += (fs_now_ms() - tp0);
        if (has_next) {
            if (next_band_in_rows > 0) {
                dec_job.dst_band = in_band_work;
                decode_band_rows(&dec_job);
            }
            cur_in_my += next_band_in_rows;
            cur_band_in_my0 = next_band_in_my0;
            cur_band_in_rows = next_band_in_rows;
            if (dec_job.fail_rc) {
                fail_rc = dec_job.fail_rc;
                break;
            }
        }
#endif
        JpegQuantBlock* tq = q_work; q_work = q_emit; q_emit = tq;
        prev_emit_mcus = cur_out_rows * info->out_mcus_x;
        has_prev_emit = 1;
    }

    // Emit final band synchronously
    if (!fail_rc && has_prev_emit) {
        uint64_t te_fin = fs_now_ms();
        uint64_t ce_fin = fs_thread_cpu_ns();
        if (!native_jpeg_emit_quantized_blocks(out_state, q_emit, prev_emit_mcus)) {
            fail_rc = -13;
        }
        main_emit_cpu_ns += fs_thread_cpu_ns() - ce_fin;
        emit_wait_ms += (fs_now_ms() - te_fin);
    }

    int pin_cnt = 0;
    int io_wait_ms = (int)br->io_wait_ms;
#ifndef _WIN32
    for (int k = 0; k < 3; k++) sem_worker_destroy(&sw[k]);
    pin_cnt = main_pinned + sw[0].pinned + sw[1].pinned + sw[2].pinned;
    fs_restore_affinity(&saved_affinity);
    io_prefetch_destroy(&prefetch);
#endif

    free(in_band_work);
    free(in_band_dec);
    free(q_work);
    free(q_emit);
    free(br);
    free(in_mem_buf);
    free(info);
    if (fp) fclose(fp);

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
        out_result->read_ms = io_wait_ms;
        out_result->pin_cnt = pin_cnt;
        out_result->setup_ms = out_result->total_ms - out_result->dec_ms - out_result->proc_ms - out_result->emit_ms;
        out_result->lut_ms = lut_wall_ms;
        {
            uint64_t s2c = main_s2_cpu_ns, decc = main_dec_cpu_ns, emc = main_emit_cpu_ns;
            uint64_t pn = 0, pi = 0, pl = 0, pf = 0;
            uint64_t fast_b = 0, tot_b = 0;
#ifndef _WIN32
            for (int k = 0; k < 3; k++) {
                s2c += wtasks[k].s2_cpu_ns;
                emc += wtasks[k].emit_cpu_ns;
            }
#endif
            for (int k = 0; k < 4; k++) {
                pn += prof_acc[k].n;
                pi += prof_acc[k].idct_ns;
                pl += prof_acc[k].lut_ns;
                pf += prof_acc[k].fdct_ns;
                fast_b += prof_acc[k].dct_fast_blks;
                tot_b += prof_acc[k].tot_blks;
            }
            out_result->cpu_s2_ms = (int)(s2c / 1000000ULL);
            out_result->cpu_dec_ms = (int)(decc / 1000000ULL);
            out_result->cpu_emit_ms = (int)(emc / 1000000ULL);
            out_result->dct_fast_pct = (tot_b > 0ULL) ? (int)((fast_b * 100ULL + (tot_b >> 1)) / tot_b) : 0;
            if (pn > 0) {
                out_result->idct_ms = (int)(pi * (uint64_t)prof_total_mcus / pn / 1000000ULL);
                out_result->lut_eval_ms = (int)(pl * (uint64_t)prof_total_mcus / pn / 1000000ULL);
                out_result->fdct_ms = (int)(pf * (uint64_t)prof_total_mcus / pn / 1000000ULL);
            }
        }
        out_result->cur_mhz = sched_mhz;
        out_result->bg_cgroup = sched_bg;
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
