#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "legacyfs.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Dual-mode corrupted-image fuzzer for legacyfs:
 *   1. Clang LibFuzzer entrypoint when compiled with -DLEGACYFS_LIBFUZZER -fsanitize=fuzzer,address,undefined
 *   2. Deterministic standalone mutation & corrupted-image stress runner (`make check-fuzz`)
 */

typedef struct {
    const uint8_t *data;
    size_t size;
} fuzz_slice_t;

static int fuzz_read_cb(void *ctx, uint64_t offset, size_t length, void *buffer) {
    const fuzz_slice_t *s = (const fuzz_slice_t *)ctx;
    if (offset > s->size || s->size - (size_t)offset < length) {
        return 5; /* EIO */
    }
    memcpy(buffer, s->data + (size_t)offset, length);
    return 0;
}

typedef struct {
    legacyfs_t *fs;
    int depth;
    int budget;
} fuzz_walk_t;

static int fuzz_dir_cb(void *user_ctx, const char *name, uint64_t ino, legacyfs_ftype_t ftype);

static void fuzz_exercise_ino(fuzz_walk_t *w, uint64_t ino) {
    if (w->budget <= 0) return;
    w->budget--;

    legacyfs_attr_t attr;
    if (legacyfs_getattr(w->fs, ino, &attr) != 0) return;

    if (attr.ftype == LEGACYFS_FT_REG) {
        uint8_t buf[512];
        size_t got = 0;
        (void)legacyfs_read(w->fs, ino, 0, sizeof(buf), buf, &got);
        if (attr.size > 256ULL) {
            (void)legacyfs_read(w->fs, ino, attr.size - 128ULL, 128U, buf, &got);
        }
    } else if (attr.ftype == LEGACYFS_FT_SYMLINK) {
        char target[256];
        size_t len = 0;
        (void)legacyfs_readlink(w->fs, ino, target, sizeof(target), &len);
    } else if (attr.ftype == LEGACYFS_FT_DIR && w->depth < 4) {
        w->depth++;
        (void)legacyfs_readdir(w->fs, ino, fuzz_dir_cb, w);
        w->depth--;
    }
}

static int fuzz_dir_cb(void *user_ctx, const char *name, uint64_t ino, legacyfs_ftype_t ftype) {
    (void)ftype;
    fuzz_walk_t *w = (fuzz_walk_t *)user_ctx;
    if (w->budget <= 0) return 1;
    if (!name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    fuzz_exercise_ino(w, ino);
    return (w->budget <= 0) ? 1 : 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    fuzz_slice_t slice = { data, size };
    legacyfs_probe_info_t pinfo;
    int prc = legacyfs_probe_io(fuzz_read_cb, &slice, size, &pinfo);

    /* 1. Exercise auto-detected open + bounded tree walk */
    if (prc == 0 && pinfo.can_mount_ro) {
        legacyfs_t *fs = NULL;
        if (legacyfs_open_io(fuzz_read_cb, &slice, size, LEGACYFS_KIND_UNKNOWN, &fs) == 0 && fs) {
            legacyfs_statfs_t st;
            (void)legacyfs_statfs(fs, &st);
            fuzz_walk_t w = { fs, 0, 32 };
            fuzz_exercise_ino(&w, legacyfs_root_ino(fs));
            uint64_t dummy = 0;
            (void)legacyfs_lookup_path(fs, "/etc/passwd", &dummy);
            legacyfs_close(fs);
        }
    }

    /* 2. Force-mount against one driver chosen by first input byte to stress parser rejection paths */
    if (size >= 4) {
        static const legacyfs_kind_t k_mountable[] = {
            LEGACYFS_KIND_REISERFS, LEGACYFS_KIND_MINIX, LEGACYFS_KIND_CRAMFS,
            LEGACYFS_KIND_ROMFS, LEGACYFS_KIND_ISO9660, LEGACYFS_KIND_SQUASHFS,
            LEGACYFS_KIND_EROFS, LEGACYFS_KIND_UFS, LEGACYFS_KIND_JFS,
            LEGACYFS_KIND_SYSV, LEGACYFS_KIND_BTRFS, LEGACYFS_KIND_XFS,
            LEGACYFS_KIND_UDF
        };
        legacyfs_kind_t forced = k_mountable[data[0] % (sizeof(k_mountable) / sizeof(k_mountable[0]))];
        legacyfs_t *fs = NULL;
        if (legacyfs_open_io(fuzz_read_cb, &slice, size, forced, &fs) == 0 && fs) {
            fuzz_walk_t w = { fs, 0, 16 };
            fuzz_exercise_ino(&w, legacyfs_root_ino(fs));
            legacyfs_close(fs);
        }
    }

    return 0;
}

#ifndef LEGACYFS_LIBFUZZER
static uint64_t xorshift64(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

static void seed_magic_template(uint8_t *buf, size_t cap, uint32_t variant, uint64_t *rng) {
    memset(buf, 0, cap);
    switch (variant % 13U) {
    case 0: /* ReiserFS v3.6 */
        if (cap >= 70000) {
            memcpy(buf + 65536 + 52, "ReIsEr2Fs", 9);
            buf[65536 + 44] = 0x00; buf[65536 + 45] = 0x10;
            buf[65536 + 8] = 17;
        }
        break;
    case 1: /* Minix v3 */
        if (cap >= 4096) {
            buf[1024 + 24] = 0x5A; buf[1024 + 25] = 0x4D;
            buf[1024 + 28] = 0x00; buf[1024 + 29] = 0x04;
        }
        break;
    case 2: /* CramFS */
        if (cap >= 1024) {
            buf[0] = 0x45; buf[1] = 0x3D; buf[2] = 0xCD; buf[3] = 0x28;
            memcpy(buf + 16, "Compressed ROMFS", 16);
        }
        break;
    case 3: /* ROMFS */
        if (cap >= 1024) {
            memcpy(buf, "-rom1fs-", 8);
            buf[10] = 0x04;
        }
        break;
    case 4: /* ISO9660 */
        if (cap >= 40960) {
            buf[32768] = 1;
            memcpy(buf + 32769, "CD001", 5);
            buf[32774] = 1;
        }
        break;
    case 5: /* SquashFS */
        if (cap >= 1024) {
            memcpy(buf, "hsqs", 4);
            buf[13] = 0x10; buf[28] = 4; buf[41] = 0x04;
        }
        break;
    case 6: /* EROFS */
        if (cap >= 4096) {
            buf[1024] = 0xE2; buf[1025] = 0xE1; buf[1026] = 0xF5; buf[1027] = 0xE0;
            buf[1024 + 12] = 12;
        }
        break;
    case 7: /* UFS2 */
        if (cap >= 70000) {
            buf[65536 + 1372] = 0x19; buf[65536 + 1373] = 0x01;
            buf[65536 + 1374] = 0x54; buf[65536 + 1375] = 0x19;
            buf[65536 + 49] = 0x10; buf[65536 + 53] = 0x10;
        }
        break;
    case 8: /* JFS */
        if (cap >= 40960) {
            memcpy(buf + 32768, "JFS1", 4);
            buf[32768 + 4] = 1;
            buf[32768 + 17] = 0x10;
        }
        break;
    case 9: /* SysV */
        if (cap >= 2048) {
            buf[512 + 504] = 0x20; buf[512 + 505] = 0x7E;
            buf[512 + 506] = 0x18; buf[512 + 507] = 0xFD;
        }
        break;
    case 10: /* Btrfs */
        if (cap >= 75000) {
            memcpy(buf + 65536 + 64, "_BHRfS_M", 8);
            buf[65536 + 145] = 0x10; /* sectorsize 4096 */
            buf[65536 + 149] = 0x10; /* nodesize 4096 */
            buf[65536 + 81] = 0x20;  /* root logical */
        }
        break;
    case 11: /* XFS */
        if (cap >= 8192) {
            memcpy(buf, "XFSB", 4);
            buf[6] = 0x10; /* blocksize 4096 */
            buf[104] = 0x02; /* inodesize 512 */
            buf[120] = 12; buf[123] = 3; buf[124] = 8;
            buf[87] = 64; buf[91] = 1; buf[63] = 64;
        }
        break;
    case 12: /* UDF */
        if (cap >= 140000) {
            memcpy(buf + 32769, "NSR03", 5);
            buf[256 * 512] = 2; /* AVDP tag 2 at 512B sector 256 */
            buf[256 * 512 + 12] = 0; buf[256 * 512 + 13] = 1;
            buf[256 * 512 + 17] = 0x10; buf[256 * 512 + 20] = 32;
        }
        break;
    }

    /* Inject random corruptions, extreme lengths, and cyclic pointers */
    uint32_t mutations = 8U + (uint32_t)(xorshift64(rng) % 64U);
    for (uint32_t m = 0; m < mutations; m++) {
        size_t idx = (size_t)(xorshift64(rng) % cap);
        uint8_t mode = (uint8_t)(xorshift64(rng) & 3U);
        if (mode == 0) {
            buf[idx] ^= (uint8_t)(1U << (xorshift64(rng) & 7U));
        } else if (mode == 1) {
            buf[idx] = 0xFFU;
        } else if (mode == 2) {
            buf[idx] = 0x00U;
        } else {
            buf[idx] = (uint8_t)xorshift64(rng);
        }
    }
}

int main(int argc, char **argv) {
    uint32_t iterations = 2500U;
    if (argc > 1) {
        unsigned long parsed = strtoul(argv[1], NULL, 10);
        if (parsed > 0 && parsed <= 1000000UL) iterations = (uint32_t)parsed;
    }

    size_t max_cap = 160 * 1024;
    uint8_t *buf = (uint8_t *)malloc(max_cap);
    if (!buf) return 1;

    uint64_t rng = 0xC0FFEE123456789BULL;
    printf("[legacyfs-fuzz] Running %u corrupted-image fuzz iterations across all 13 drivers...\n", iterations);

    /* Edge-case tiny buffers (0B, 1B, 7B, 31B, 511B, 512B, 1023B) */
    static const size_t tiny_sizes[] = { 0, 1, 7, 31, 64, 511, 512, 1023, 2048, 4096 };
    for (size_t t = 0; t < sizeof(tiny_sizes) / sizeof(tiny_sizes[0]); t++) {
        for (size_t k = 0; k < tiny_sizes[t]; k++) buf[k] = (uint8_t)xorshift64(&rng);
        (void)LLVMFuzzerTestOneInput(buf, tiny_sizes[t]);
    }

    for (uint32_t i = 0; i < iterations; i++) {
        size_t active_sz = (i % 5U == 0U)
            ? (size_t)(xorshift64(&rng) % 8192U)
            : max_cap;
        if (active_sz > 0) {
            seed_magic_template(buf, active_sz, i, &rng);
        }
        (void)LLVMFuzzerTestOneInput(buf, active_sz);
    }

    free(buf);
    printf("[legacyfs-fuzz] Completed %u iterations with 0 crashes or sanitizer errors.\n", iterations);
    return 0;
}
#endif
