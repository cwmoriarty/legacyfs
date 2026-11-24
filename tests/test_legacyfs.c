#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "legacyfs.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

typedef struct {
    uint8_t *data;
    size_t size;
} mem_img_t;

static int mem_read_cb(void *ctx, uint64_t offset, size_t length, void *buffer) {
    const mem_img_t *m = (const mem_img_t *)ctx;
    if (offset > m->size || m->size - (size_t)offset < length) {
        return EIO;
    }
    memcpy(buffer, m->data + offset, length);
    return 0;
}

static void put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)(v >> 8);
}
static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
}
static void put_le64(uint8_t *p, uint64_t v) {
    put_le32(p, (uint32_t)(v & 0xFFFFFFFFULL));
    put_le32(p + 4, (uint32_t)(v >> 32));
}
static void put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFU);
}
static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xFFU);
    p[1] = (uint8_t)((v >> 16) & 0xFFU);
    p[2] = (uint8_t)((v >> 8) & 0xFFU);
    p[3] = (uint8_t)(v & 0xFFU);
}

static void verify_file_and_link(
    const mem_img_t *img,
    legacyfs_kind_t expected_kind,
    const char *file_path,
    const char *expected_text,
    const char *link_path,
    const char *expected_target
) {
    legacyfs_probe_info_t pinfo;
    int prc = legacyfs_probe_io(mem_read_cb, (void *)img, img->size, &pinfo);
    assert(prc == 0);
    assert(pinfo.kind == expected_kind);
    assert(pinfo.can_mount_ro == 1);

    legacyfs_t *fs = NULL;
    int orc = legacyfs_open_io(mem_read_cb, (void *)img, img->size, LEGACYFS_KIND_UNKNOWN, &fs);
    assert(orc == 0 && fs != NULL);

    uint64_t fino = 0;
    assert(legacyfs_lookup_path(fs, file_path, &fino) == 0);

    legacyfs_attr_t fattr;
    assert(legacyfs_getattr(fs, fino, &fattr) == 0);
    assert(fattr.ftype == LEGACYFS_FT_REG);
    assert(fattr.size == strlen(expected_text));

    char buf[16384];
    size_t got = 0;
    assert(legacyfs_read(fs, fino, 0, sizeof(buf) - 1, buf, &got) == 0);
    assert(got == strlen(expected_text));
    buf[got] = '\0';
    assert(memcmp(buf, expected_text, got) == 0);

    /* Verify non-zero offset read */
    if (got > 5) {
        char sub[16384];
        size_t sgot = 0;
        assert(legacyfs_read(fs, fino, 3, got - 3, sub, &sgot) == 0);
        assert(sgot == got - 3);
        assert(memcmp(sub, expected_text + 3, sgot) == 0);
    }

    if (link_path && expected_target) {
        uint64_t lino = 0;
        assert(legacyfs_lookup_path(fs, link_path, &lino) == 0);
        char lbuf[256];
        size_t lgot = 0;
        assert(legacyfs_readlink(fs, lino, lbuf, sizeof(lbuf), &lgot) == 0);
        assert(strcmp(lbuf, expected_target) == 0);
    }

    legacyfs_close(fs);
}

/* 1. Synthetic ReiserFS 3.6 test (2-level S+tree: internal root -> 2 leaves, tail-packed + indirect file + symlink) */
static void rfs_put_v2_key(uint8_t *k, uint32_t dir_id, uint32_t obj_id, uint64_t off, uint64_t type) {
    put_le32(k + 0, dir_id);
    put_le32(k + 4, obj_id);
    put_le64(k + 8, (off & 0x0FFFFFFFFFFFFFFFULL) | (type << 60));
}

static void rfs_put_sd_v2(uint8_t *b, uint16_t mode, uint64_t size) {
    memset(b, 0, 44);
    put_le16(b + 0, mode);
    put_le32(b + 4, 1);
    put_le64(b + 8, size);
    put_le32(b + 16, 1000);
    put_le32(b + 20, 1000);
    put_le32(b + 28, 1700000000U);
}

static void test_reiserfs_v36(void) {
    const size_t bsize = 4096;
    const uint32_t nblocks = 64;
    mem_img_t img = { (uint8_t *)calloc(nblocks, bsize), nblocks * bsize };

    /* Superblock at block 16 (65536) */
    uint8_t *sb = img.data + 65536;
    put_le32(sb + 0, nblocks);
    put_le32(sb + 4, 40);
    put_le32(sb + 8, 20);      /* root internal node at block 20 */
    put_le16(sb + 40, 3);
    put_le16(sb + 44, bsize);
    put_le16(sb + 50, 1);
    memcpy(sb + 52, "ReIsEr2Fs", 9);
    memcpy(sb + 100, "RFS_ARCHIVE", 11);

    /* Internal node at block 20: 1 delimiter key (2, 10, 0, 0), 2 children: leaf 21 and leaf 22 */
    uint8_t *inode = img.data + 20 * bsize;
    put_le16(inode + 0, 2); /* level 2 = internal */
    put_le16(inode + 2, 1); /* 1 key -> 2 disk_children */
    rfs_put_v2_key(inode + 24, 2, 10, 0, 0);
    put_le32(inode + 24 + 16 + 0, 21); /* child 0 = block 21 */
    put_le32(inode + 24 + 16 + 8, 22); /* child 1 = block 22 */

    /* Leaf 21: root dir (1, 2) stat_data + direntry ("readme.txt"->(2,10), "big.bin"->(2,11), "rel.lnk"->(2,12)) */
    uint8_t *l1 = img.data + 21 * bsize;
    put_le16(l1 + 0, 1);
    put_le16(l1 + 2, 2);

    uint16_t loc0 = 3800;
    rfs_put_sd_v2(l1 + loc0, S_IFDIR | 0755, 96);
    uint8_t *ih0 = l1 + 24;
    rfs_put_v2_key(ih0, 1, 2, 0, 0);
    put_le16(ih0 + 18, 44);
    put_le16(ih0 + 20, loc0);
    put_le16(ih0 + 22, 1);

    /* Build direntry item: 3 entries (3 * 16 = 48B headers + names packed from end) */
    uint16_t dlen = 112;
    uint16_t loc1 = 3600;
    uint8_t *dbody = l1 + loc1;
    /* Entry 0 name at [96..112): "readme.txt\0" */
    /* Entry 1 name at [80..96):  "big.bin\0" */
    /* Entry 2 name at [64..80):  "rel.lnk\0" */
    strcpy((char *)dbody + 96, "readme.txt");
    strcpy((char *)dbody + 80, "big.bin");
    strcpy((char *)dbody + 64, "rel.lnk");

    put_le32(dbody + 0 + 4, 2); put_le32(dbody + 0 + 8, 10); put_le16(dbody + 0 + 12, 96); put_le16(dbody + 0 + 14, 4);
    put_le32(dbody + 16 + 4, 2); put_le32(dbody + 16 + 8, 11); put_le16(dbody + 16 + 12, 80); put_le16(dbody + 16 + 14, 4);
    put_le32(dbody + 32 + 4, 2); put_le32(dbody + 32 + 8, 12); put_le16(dbody + 32 + 12, 64); put_le16(dbody + 32 + 14, 4);

    uint8_t *ih1 = l1 + 48;
    rfs_put_v2_key(ih1, 1, 2, 1, 3);
    put_le16(ih1 + 16, 3);
    put_le16(ih1 + 18, dlen);
    put_le16(ih1 + 20, loc1);
    put_le16(ih1 + 22, 1);

    /* Leaf 22: (2,10) tail-packed file, (2,11) 2-block indirect file, (2,12) symlink */
    const char *txt = "ReiserFS v3.6 tail-packed direct item payload";
    size_t txt_len = strlen(txt);
    uint8_t *l2 = img.data + 22 * bsize;
    put_le16(l2 + 0, 1);
    put_le16(l2 + 2, 6);

    /* Item 0: (2,10) stat_data */
    rfs_put_sd_v2(l2 + 3900, S_IFREG | 0644, txt_len);
    rfs_put_v2_key(l2 + 24 + 0 * 24, 2, 10, 0, 0);
    put_le16(l2 + 24 + 0 * 24 + 18, 44); put_le16(l2 + 24 + 0 * 24 + 20, 3900); put_le16(l2 + 24 + 0 * 24 + 22, 1);

    /* Item 1: (2,10) direct item at offset 1 */
    memcpy(l2 + 3800, txt, txt_len);
    rfs_put_v2_key(l2 + 24 + 1 * 24, 2, 10, 1, 2);
    put_le16(l2 + 24 + 1 * 24 + 18, (uint16_t)txt_len); put_le16(l2 + 24 + 1 * 24 + 20, 3800); put_le16(l2 + 24 + 1 * 24 + 22, 1);

    /* Item 2 & 3: (2,11) indirect file spanning blocks 30 & 31 (5000 bytes) */
    char big_expected[5001];
    for (int i = 0; i < 5000; i++) big_expected[i] = (char)('A' + (i % 26));
    big_expected[5000] = '\0';
    memcpy(img.data + 30 * bsize, big_expected, 4096);
    memcpy(img.data + 31 * bsize, big_expected + 4096, 5000 - 4096);

    rfs_put_sd_v2(l2 + 3700, S_IFREG | 0644, 5000);
    rfs_put_v2_key(l2 + 24 + 2 * 24, 2, 11, 0, 0);
    put_le16(l2 + 24 + 2 * 24 + 18, 44); put_le16(l2 + 24 + 2 * 24 + 20, 3700); put_le16(l2 + 24 + 2 * 24 + 22, 1);

    put_le32(l2 + 3680, 30);
    put_le32(l2 + 3684, 31);
    rfs_put_v2_key(l2 + 24 + 3 * 24, 2, 11, 1, 1);
    put_le16(l2 + 24 + 3 * 24 + 18, 8); put_le16(l2 + 24 + 3 * 24 + 20, 3680); put_le16(l2 + 24 + 3 * 24 + 22, 1);

    /* Item 4 & 5: (2,12) symlink -> "readme.txt" */
    const char *lnk = "readme.txt";
    rfs_put_sd_v2(l2 + 3600, S_IFLNK | 0777, strlen(lnk));
    rfs_put_v2_key(l2 + 24 + 4 * 24, 2, 12, 0, 0);
    put_le16(l2 + 24 + 4 * 24 + 18, 44); put_le16(l2 + 24 + 4 * 24 + 20, 3600); put_le16(l2 + 24 + 4 * 24 + 22, 1);

    memcpy(l2 + 3560, lnk, strlen(lnk));
    rfs_put_v2_key(l2 + 24 + 5 * 24, 2, 12, 1, 2);
    put_le16(l2 + 24 + 5 * 24 + 18, (uint16_t)strlen(lnk)); put_le16(l2 + 24 + 5 * 24 + 20, 3560); put_le16(l2 + 24 + 5 * 24 + 22, 1);

    verify_file_and_link(&img, LEGACYFS_KIND_REISERFS, "/readme.txt", txt, "/rel.lnk", lnk);
    verify_file_and_link(&img, LEGACYFS_KIND_REISERFS, "/big.bin", big_expected, NULL, NULL);
    free(img.data);
}

/* 2. Synthetic Minix v3 test */
static void test_minix_v3(void) {
    const size_t bsize = 1024;
    mem_img_t img = { (uint8_t *)calloc(32, bsize), 32 * bsize };
    uint8_t *sb = img.data + 1024;
    put_le32(sb + 0, 16);     /* ninodes */
    put_le16(sb + 6, 1);      /* imap = 1 blk */
    put_le16(sb + 8, 1);      /* zmap = 1 blk */
    put_le16(sb + 12, 0);     /* log_zone_size = 0 */
    put_le32(sb + 20, 32);    /* zones */
    put_le16(sb + 24, 0x4D5AU);
    put_le16(sb + 28, bsize);

    /* Inode table starts at block (2 + 1 + 1) = 4 */
    uint8_t *itab = img.data + 4 * bsize;
    /* Inode 1: root dir (size = 4 * 64 = 256, zone[0] = 8) */
    put_le16(itab + 0 * 64 + 0, S_IFDIR | 0755);
    put_le16(itab + 0 * 64 + 2, 2);
    put_le32(itab + 0 * 64 + 8, 4 * 64);
    put_le32(itab + 0 * 64 + 24, 8);

    const char *msg = "Hello from Minix v3 filesystem!";
    /* Inode 2: regular file at zone 9 */
    put_le16(itab + 1 * 64 + 0, S_IFREG | 0644);
    put_le16(itab + 1 * 64 + 2, 1);
    put_le32(itab + 1 * 64 + 8, (uint32_t)strlen(msg));
    put_le32(itab + 1 * 64 + 24, 9);
    memcpy(img.data + 9 * bsize, msg, strlen(msg));

    /* Inode 3: symlink at zone 10 */
    const char *lnk = "note.txt";
    put_le16(itab + 2 * 64 + 0, S_IFLNK | 0777);
    put_le16(itab + 2 * 64 + 2, 1);
    put_le32(itab + 2 * 64 + 8, (uint32_t)strlen(lnk));
    put_le32(itab + 2 * 64 + 24, 10);
    memcpy(img.data + 10 * bsize, lnk, strlen(lnk));

    uint8_t *dir = img.data + 8 * bsize;
    put_le32(dir + 0 * 64, 1); strcpy((char *)dir + 0 * 64 + 4, ".");
    put_le32(dir + 1 * 64, 1); strcpy((char *)dir + 1 * 64 + 4, "..");
    put_le32(dir + 2 * 64, 2); strcpy((char *)dir + 2 * 64 + 4, "note.txt");
    put_le32(dir + 3 * 64, 3); strcpy((char *)dir + 3 * 64 + 4, "ptr.lnk");

    verify_file_and_link(&img, LEGACYFS_KIND_MINIX, "/note.txt", msg, "/ptr.lnk", lnk);
    free(img.data);
}

/* 3. Synthetic SysV test */
static void test_sysv(void) {
    const size_t bsize = 1024;
    mem_img_t img = { (uint8_t *)calloc(16, bsize), 16 * bsize };
    uint8_t *sb = img.data + 512;
    put_le16(sb + 0, 4); /* isize = 4 -> inodes in blocks 2..3 */
    put_le32(sb + 4, 16);
    memcpy(sb + 492, "SYSV1", 5);
    put_le32(sb + 504, 0xFD187E20U);
    put_le32(sb + 508, 2); /* 1024B blocks */

    uint8_t *itab = img.data + 2 * bsize;
    /* Root is inode 2 (index 1) -> block 5 */
    uint8_t *ri = itab + 1 * 64;
    put_le16(ri + 0, S_IFDIR | 0755);
    put_le16(ri + 2, 2);
    put_le32(ri + 8, 64);
    ri[12] = 5;

    const char *msg = "System V UNIX file payload";
    /* File inode 3 (index 2) -> block 6 */
    uint8_t *fi = itab + 2 * 64;
    put_le16(fi + 0, S_IFREG | 0644);
    put_le16(fi + 2, 1);
    put_le32(fi + 8, (uint32_t)strlen(msg));
    fi[12] = 6;
    memcpy(img.data + 6 * bsize, msg, strlen(msg));

    /* Symlink inode 4 (index 3) -> block 7 */
    const char *lnk = "unix.txt";
    uint8_t *li = itab + 3 * 64;
    put_le16(li + 0, S_IFLNK | 0777);
    put_le16(li + 2, 1);
    put_le32(li + 8, (uint32_t)strlen(lnk));
    li[12] = 7;
    memcpy(img.data + 7 * bsize, lnk, strlen(lnk));

    uint8_t *dir = img.data + 5 * bsize;
    put_le16(dir + 0, 2); strcpy((char *)dir + 2, ".");
    put_le16(dir + 16, 2); strcpy((char *)dir + 18, "..");
    put_le16(dir + 32, 3); strcpy((char *)dir + 34, "unix.txt");
    put_le16(dir + 48, 4); strcpy((char *)dir + 50, "sym");

    verify_file_and_link(&img, LEGACYFS_KIND_SYSV, "/unix.txt", msg, "/sym", lnk);
    free(img.data);
}

/* 4. Synthetic CramFS test (with real zlib compression) */
static void cramfs_encode_inode(uint8_t out[12], uint16_t mode, uint32_t size, uint32_t namelen_bytes, uint32_t data_off_bytes) {
    put_le32(out + 0, (uint32_t)mode | (1000U << 16));
    put_le32(out + 4, (size & 0x00FFFFFFU) | (100U << 24));
    put_le32(out + 8, ((namelen_bytes >> 2) & 0x3FU) | ((data_off_bytes >> 2) << 6));
}

static void test_cramfs(void) {
    mem_img_t img = { (uint8_t *)calloc(1, 8192), 8192 };
    const char *msg = "Compressed ROMFS payload decompressed via zlib!";
    const char *lnk = "fw.txt";

    uLongf comp_file_len = 512;
    uint8_t comp_file[512];
    assert(compress2(comp_file, &comp_file_len, (const Bytef *)msg, (uLong)strlen(msg), Z_BEST_COMPRESSION) == Z_OK);

    uLongf comp_lnk_len = 512;
    uint8_t comp_lnk[512];
    assert(compress2(comp_lnk, &comp_lnk_len, (const Bytef *)lnk, (uLong)strlen(lnk), Z_BEST_COMPRESSION) == Z_OK);

    /* File data at 512: 4B end-ptr + comp_file */
    uint32_t file_off = 512;
    put_le32(img.data + file_off, file_off + 4U + (uint32_t)comp_file_len);
    memcpy(img.data + file_off + 4U, comp_file, comp_file_len);

    /* Symlink data at 1024: 4B end-ptr + comp_lnk */
    uint32_t lnk_off = 1024;
    put_le32(img.data + lnk_off, lnk_off + 4U + (uint32_t)comp_lnk_len);
    memcpy(img.data + lnk_off + 4U, comp_lnk, comp_lnk_len);

    /* Directory entries at 256: child 0 ("fw.txt", namelen_bytes=8 -> 12+8=20), child 1 ("s.lnk", namelen_bytes=8 -> 20) */
    uint32_t dir_off = 256;
    cramfs_encode_inode(img.data + dir_off, S_IFREG | 0644, (uint32_t)strlen(msg), 8, file_off);
    strcpy((char *)img.data + dir_off + 12, "fw.txt");

    cramfs_encode_inode(img.data + dir_off + 20, S_IFLNK | 0777, (uint32_t)strlen(lnk), 8, lnk_off);
    strcpy((char *)img.data + dir_off + 20 + 12, "s.lnk");

    /* Superblock at 0 */
    put_le32(img.data + 0, 0x28cd3d45U);
    put_le32(img.data + 4, 4096);
    memcpy(img.data + 16, "Compressed ROMFS", 16);
    strcpy((char *)img.data + 48, "CRAM_TEST");
    cramfs_encode_inode(img.data + 64, S_IFDIR | 0755, 40, 0, dir_off);

    verify_file_and_link(&img, LEGACYFS_KIND_CRAMFS, "/fw.txt", msg, "/s.lnk", lnk);
    free(img.data);
}

/* 5. Synthetic ROMFS test (including hardlink resolution) */
static void test_romfs(void) {
    mem_img_t img = { (uint8_t *)calloc(1, 4096), 4096 };
    memcpy(img.data, "-rom1fs-", 8);
    put_be32(img.data + 8, 2048);
    strcpy((char *)img.data + 16, "ROMVOL"); /* 6 chars -> padded to 16 -> root hdr at 32 */

    /* 32: root dir (type 1, spec_info = first child at 64) */
    put_be32(img.data + 32, 0U | 1U);
    put_be32(img.data + 36, 64U);
    strcpy((char *)img.data + 48, ".");

    const char *msg = "ROMFS big-endian embedded payload";
    /* 64: regular file "boot.cfg" (name 8B -> padded 16B -> data at 96..143), next hdr at 160 */
    put_be32(img.data + 64, 160U | 2U);
    put_be32(img.data + 72, (uint32_t)strlen(msg));
    strcpy((char *)img.data + 80, "boot.cfg");
    memcpy(img.data + 96, msg, strlen(msg));

    /* 160: hardlink "alias.cfg" (type 0, spec_info = 64), next hdr at 192 */
    put_be32(img.data + 160, 192U | 0U);
    put_be32(img.data + 164, 64U);
    strcpy((char *)img.data + 176, "alias.cfg");

    /* 192: symlink "cur.lnk" -> "boot.cfg" (name 7B -> padded 16B -> data at 224) */
    const char *lnk = "boot.cfg";
    put_be32(img.data + 192, 0U | 3U);
    put_be32(img.data + 200, (uint32_t)strlen(lnk));
    strcpy((char *)img.data + 208, "cur.lnk");
    memcpy(img.data + 224, lnk, strlen(lnk));

    verify_file_and_link(&img, LEGACYFS_KIND_ROMFS, "/boot.cfg", msg, "/cur.lnk", lnk);
    verify_file_and_link(&img, LEGACYFS_KIND_ROMFS, "/alias.cfg", msg, NULL, NULL);
    free(img.data);
}

/* 6. Synthetic SquashFS 4.0 test (zlib metadata + zlib data block + symlink) */
static size_t sqfs_write_meta_zlib(uint8_t *dst, const uint8_t *raw, size_t raw_len) {
    uLongf clen = 8192;
    uint8_t cbuf[8192];
    assert(compress2(cbuf, &clen, raw, (uLong)raw_len, Z_BEST_COMPRESSION) == Z_OK);
    put_le16(dst, (uint16_t)clen); /* bit 15 = 0 -> zlib compressed */
    memcpy(dst + 2, cbuf, clen);
    return 2U + (size_t)clen;
}

static void test_squashfs_v4(void) {
    mem_img_t img = { (uint8_t *)calloc(1, 16384), 16384 };
    const char *msg = "SquashFS 4.0 zlib-compressed block content!";
    const char *lnk = "app.conf";

    /* Compress file data block placed at offset 256 */
    uLongf cdata_len = 1024;
    uint8_t cdata[1024];
    assert(compress2(cdata, &cdata_len, (const Bytef *)msg, (uLong)strlen(msg), Z_BEST_COMPRESSION) == Z_OK);
    memcpy(img.data + 256, cdata, cdata_len);

    /* Build uncompressed inode table buffer:
     * inner 0:  BASIC_DIR root (32B)
     * inner 32: BASIC_FILE "app.conf" (32B + 4B block_list = 36B)
     * inner 68: BASIC_SYMLINK "run.lnk" (24B + 8B target = 32B) */
    uint8_t itab_raw[256];
    memset(itab_raw, 0, sizeof(itab_raw));

    /* Directory table raw: 12B dir_header + entry0 (8 + 8 = 16) + entry1 (8 + 7 = 15) = 43B -> file_size = 43 + 3 = 46 */
    uint8_t dtab_raw[128];
    memset(dtab_raw, 0, sizeof(dtab_raw));
    put_le32(dtab_raw + 0, 1); /* count - 1 = 1 -> 2 entries */
    put_le32(dtab_raw + 4, 0); /* start_block = 0 */
    put_le32(dtab_raw + 8, 1);
    /* Entry 0: offset 32, type 2 (file), size = 8 - 1 = 7, "app.conf" */
    put_le16(dtab_raw + 12 + 0, 32);
    put_le16(dtab_raw + 12 + 4, 2);
    put_le16(dtab_raw + 12 + 6, 7);
    memcpy(dtab_raw + 12 + 8, "app.conf", 8);
    /* Entry 1: offset 68, type 3 (symlink), size = 7 - 1 = 6, "run.lnk" */
    put_le16(dtab_raw + 28 + 0, 68);
    put_le16(dtab_raw + 28 + 4, 3);
    put_le16(dtab_raw + 28 + 6, 6);
    memcpy(dtab_raw + 28 + 8, "run.lnk", 7);

    /* Root BASIC_DIR at inner 0 */
    put_le16(itab_raw + 0, 1);
    put_le16(itab_raw + 2, 0755);
    put_le32(itab_raw + 16, 0);
    put_le32(itab_raw + 20, 2);
    put_le16(itab_raw + 24, 43 + 3);
    put_le16(itab_raw + 26, 0);

    /* BASIC_FILE at inner 32 */
    put_le16(itab_raw + 32 + 0, 2);
    put_le16(itab_raw + 32 + 2, 0644);
    put_le32(itab_raw + 32 + 16, 256); /* blocks_start */
    put_le32(itab_raw + 32 + 20, 0xFFFFFFFFU); /* no fragment */
    put_le32(itab_raw + 32 + 28, (uint32_t)strlen(msg));
    put_le32(itab_raw + 32 + 32, (uint32_t)cdata_len); /* compressed block size */

    /* BASIC_SYMLINK at inner 68 */
    put_le16(itab_raw + 68 + 0, 3);
    put_le16(itab_raw + 68 + 2, 0777);
    put_le32(itab_raw + 68 + 16, 1);
    put_le32(itab_raw + 68 + 20, (uint32_t)strlen(lnk));
    memcpy(itab_raw + 68 + 24, lnk, strlen(lnk));

    uint64_t itab_pos = 2048;
    sqfs_write_meta_zlib(img.data + itab_pos, itab_raw, 100);
    uint64_t dtab_pos = 4096;
    sqfs_write_meta_zlib(img.data + dtab_pos, dtab_raw, 43);

    /* Superblock */
    put_le32(img.data + 0, 0x73717368U);
    put_le32(img.data + 4, 3);
    put_le32(img.data + 12, 4096);
    put_le16(img.data + 20, 1); /* ZLIB */
    put_le16(img.data + 28, 4);
    put_le16(img.data + 30, 0);
    put_le64(img.data + 32, 0); /* root_ref = (0 << 16) | 0 */
    put_le64(img.data + 40, 8192);
    put_le64(img.data + 48, UINT64_MAX);
    put_le64(img.data + 64, itab_pos);
    put_le64(img.data + 72, dtab_pos);
    put_le64(img.data + 80, UINT64_MAX);

    verify_file_and_link(&img, LEGACYFS_KIND_SQUASHFS, "/app.conf", msg, "/run.lnk", lnk);
    free(img.data);
}

/* 7. Synthetic EROFS test (flat-inline tail-packed file + symlink) */
static void test_erofs(void) {
    const size_t bsize = 4096;
    mem_img_t img = { (uint8_t *)calloc(8, bsize), 8 * bsize };
    uint8_t *sb = img.data + 1024;
    put_le32(sb + 0, 0xE0F5E1E2U);
    sb[12] = 12;               /* 2^12 = 4096 */
    put_le16(sb + 14, 0);      /* root_nid = 0 */
    put_le32(sb + 36, 8);
    put_le32(sb + 40, 1);      /* meta_blkaddr = 1 (byte 4096) */
    strcpy((char *)sb + 64, "EROFS_IMG");

    /* nid 0 at 4096 + 0: root compact dir (FLAT_PLAIN, blk 2) */
    uint8_t *n0 = img.data + 4096 + 0 * 32;
    put_le16(n0 + 0, 0);       /* compact, FLAT_PLAIN */
    put_le16(n0 + 4, S_IFDIR | 0755);
    put_le16(n0 + 6, 2);
    put_le32(n0 + 8, 96);
    put_le32(n0 + 16, 2);      /* dir data at block 2 */

    /* Dir block at 2 * 4096: 4 entries (".", "..", "hello.txt"->nid 2, "l.lnk"->nid 6) */
    uint8_t *db = img.data + 2 * bsize;
    /* 4 * 12 = 48 -> first nameoff = 48 */
    put_le64(db + 0 * 12, 0); put_le16(db + 0 * 12 + 8, 48); db[0 * 12 + 10] = 2;
    put_le64(db + 1 * 12, 0); put_le16(db + 1 * 12 + 8, 50); db[1 * 12 + 10] = 2;
    put_le64(db + 2 * 12, 2); put_le16(db + 2 * 12 + 8, 53); db[2 * 12 + 10] = 1;
    put_le64(db + 3 * 12, 6); put_le16(db + 3 * 12 + 8, 63); db[3 * 12 + 10] = 7;
    memcpy(db + 48, ".\0..\0hello.txt\0l.lnk\0", 21);

    /* nid 2 at 4096 + 2 * 32: compact FLAT_INLINE (datalayout=2 -> ifmt = 2 << 1 = 4) */
    const char *msg = "EROFS inline tail-packed data!";
    uint8_t *n2 = img.data + 4096 + 2 * 32;
    put_le16(n2 + 0, 2U << 1);
    put_le16(n2 + 4, S_IFREG | 0644);
    put_le16(n2 + 6, 1);
    put_le32(n2 + 8, (uint32_t)strlen(msg));
    memcpy(n2 + 32, msg, strlen(msg));

    /* nid 6 at 4096 + 6 * 32: symlink FLAT_INLINE */
    const char *lnk = "hello.txt";
    uint8_t *n6 = img.data + 4096 + 6 * 32;
    put_le16(n6 + 0, 2U << 1);
    put_le16(n6 + 4, S_IFLNK | 0777);
    put_le16(n6 + 6, 1);
    put_le32(n6 + 8, (uint32_t)strlen(lnk));
    memcpy(n6 + 32, lnk, strlen(lnk));

    verify_file_and_link(&img, LEGACYFS_KIND_EROFS, "/hello.txt", msg, "/l.lnk", lnk);
    free(img.data);
}

/* 8. Synthetic ISO9660 + Rock Ridge test */
static size_t iso_write_rec(uint8_t *dst, uint32_t lba, uint32_t len, uint8_t flags, const char *iso_id, size_t id_len, const char *rr_nm, uint32_t rr_mode, const char *rr_sl) {
    size_t su_off = 33U + id_len + ((id_len & 1U) == 0 ? 1U : 0U);
    memset(dst, 0, 256);
    put_le32(dst + 2, lba);
    put_be32(dst + 6, lba);
    put_le32(dst + 10, len);
    put_be32(dst + 14, len);
    dst[25] = flags;
    dst[32] = (uint8_t)id_len;
    memcpy(dst + 33, iso_id, id_len);

    size_t su = su_off;
    if (rr_mode != 0) {
        dst[su + 0] = 'P'; dst[su + 1] = 'X'; dst[su + 2] = 36; dst[su + 3] = 1;
        put_le32(dst + su + 4, rr_mode);
        put_le32(dst + su + 12, 1);
        su += 36;
    }
    if (rr_nm) {
        size_t nl = strlen(rr_nm);
        dst[su + 0] = 'N'; dst[su + 1] = 'M'; dst[su + 2] = (uint8_t)(5 + nl); dst[su + 3] = 1;
        dst[su + 4] = 0;
        memcpy(dst + su + 5, rr_nm, nl);
        su += 5 + nl;
    }
    if (rr_sl) {
        size_t sl = strlen(rr_sl);
        dst[su + 0] = 'S'; dst[su + 1] = 'L'; dst[su + 2] = (uint8_t)(7 + sl); dst[su + 3] = 1;
        dst[su + 4] = 0;
        dst[su + 5] = 0;
        dst[su + 6] = (uint8_t)sl;
        memcpy(dst + su + 7, rr_sl, sl);
        su += 7 + sl;
    }
    if (su & 1U) su++;
    dst[0] = (uint8_t)su;
    return su;
}

static void test_iso9660_rock_ridge(void) {
    const size_t lb = 2048;
    mem_img_t img = { (uint8_t *)calloc(24, lb), 24 * lb };
    uint8_t *pvd = img.data + 16 * lb;
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    memcpy(pvd + 40, "ARCHIVAL_DISC   ", 16);
    put_le32(pvd + 80, 24);
    put_le16(pvd + 128, lb);

    /* Root dir at LBA 18 */
    iso_write_rec(pvd + 156, 18, lb, 0x02, "\0", 1, NULL, 0, NULL);

    const char *msg = "ISO9660 with Rock Ridge long filename content!";
    memcpy(img.data + 19 * lb, msg, strlen(msg));

    uint8_t *dir = img.data + 18 * lb;
    size_t pos = 0;
    pos += iso_write_rec(dir + pos, 18, lb, 0x02, "\0", 1, NULL, 0, NULL);
    pos += iso_write_rec(dir + pos, 18, lb, 0x02, "\1", 1, NULL, 0, NULL);
    pos += iso_write_rec(dir + pos, 19, (uint32_t)strlen(msg), 0x00, "LONGFILE.TXT;1", 14, "LongRockRidgeName.txt", S_IFREG | 0644, NULL);
    pos += iso_write_rec(dir + pos, 0, 0, 0x00, "LINK.;1", 7, "shortcut.lnk", S_IFLNK | 0777, "LongRockRidgeName.txt");

    verify_file_and_link(&img, LEGACYFS_KIND_ISO9660, "/LongRockRidgeName.txt", msg, "/shortcut.lnk", "LongRockRidgeName.txt");
    free(img.data);
}

/* 9. Synthetic UFS2 test */
static void test_ufs2(void) {
    const size_t fsize = 1024;
    const size_t bsize = 4096;
    mem_img_t img = { (uint8_t *)calloc(128, fsize), 128 * fsize };

    /* Superblock at 64 KiB (frag 64) */
    uint8_t *sb = img.data + 65536;
    put_le32(sb + 16, 80);    /* fs_iblkno = frag 80 */
    put_le32(sb + 32, 1);     /* fs_ncg = 1 */
    put_le32(sb + 48, bsize);
    put_le32(sb + 52, fsize);
    put_le32(sb + 184, 32);   /* fs_ipg = 32 */
    put_le32(sb + 188, 128);  /* fs_fpg = 128 */
    strcpy((char *)sb + 680, "BSD_UFS2");
    put_le64(sb + 1088, 128);
    put_le32(sb + 1372, 0x19540119U);

    uint8_t *itab = img.data + 80 * fsize;
    /* Inode 2 (root dir) -> frag 96 */
    uint8_t *ri = itab + 2 * 256;
    put_le16(ri + 0, S_IFDIR | 0755);
    put_le16(ri + 2, 2);
    put_le64(ri + 16, 64);
    put_le64(ri + 24, 2);
    put_le64(ri + 112, 96);

    const char *msg = "BSD UFS2 Fast File System verified!";
    /* Inode 3 (file) -> frag 100 */
    uint8_t *fi = itab + 3 * 256;
    put_le16(fi + 0, S_IFREG | 0644);
    put_le16(fi + 2, 1);
    put_le64(fi + 16, strlen(msg));
    put_le64(fi + 24, 2);
    put_le64(fi + 112, 100);
    memcpy(img.data + 100 * fsize, msg, strlen(msg));

    /* Inode 4 (fast inline symlink: blocks_512 == 0) */
    const char *lnk = "bsd.txt";
    uint8_t *li = itab + 4 * 256;
    put_le16(li + 0, S_IFLNK | 0777);
    put_le16(li + 2, 1);
    put_le64(li + 16, strlen(lnk));
    put_le64(li + 24, 0);
    memcpy(li + 112, lnk, strlen(lnk));

    /* BSD directory entries at frag 96 (each 16B) */
    uint8_t *dir = img.data + 96 * fsize;
    put_le32(dir + 0, 2);  put_le16(dir + 4, 16); dir[6] = 4;  dir[7] = 1; strcpy((char *)dir + 8, ".");
    put_le32(dir + 16, 2); put_le16(dir + 20, 16); dir[22] = 4; dir[23] = 2; strcpy((char *)dir + 24, "..");
    put_le32(dir + 32, 3); put_le16(dir + 36, 16); dir[38] = 8; dir[39] = 7; strcpy((char *)dir + 40, "bsd.txt");
    put_le32(dir + 48, 4); put_le16(dir + 52, 16); dir[54] = 10; dir[55] = 5; strcpy((char *)dir + 56, "u.lnk");

    verify_file_and_link(&img, LEGACYFS_KIND_UFS, "/bsd.txt", msg, "/u.lnk", lnk);
    free(img.data);
}

/* 10. Synthetic IBM JFS test */
static void jfs_put_pxd(uint8_t raw[8], uint32_t len, uint64_t addr) {
    raw[0] = (uint8_t)(len & 0xFFU);
    raw[1] = (uint8_t)((len >> 8) & 0xFFU);
    raw[2] = (uint8_t)((len >> 16) & 0xFFU);
    raw[3] = (uint8_t)((addr >> 32) & 0xFFU);
    put_le32(raw + 4, (uint32_t)(addr & 0xFFFFFFFFULL));
}

static void jfs_put_xad(uint8_t raw[16], uint64_t lblk_off, uint32_t len, uint64_t disk_blk) {
    memset(raw, 0, 16);
    raw[3] = (uint8_t)((lblk_off >> 32) & 0xFFU);
    put_le32(raw + 4, (uint32_t)(lblk_off & 0xFFFFFFFFULL));
    raw[8] = (uint8_t)(len & 0xFFU);
    raw[9] = (uint8_t)((len >> 8) & 0xFFU);
    raw[10] = (uint8_t)((len >> 16) & 0xFFU);
    raw[11] = (uint8_t)((disk_blk >> 32) & 0xFFU);
    put_le32(raw + 12, (uint32_t)(disk_blk & 0xFFFFFFFFULL));
}

static void test_jfs(void) {
    const size_t bsize = 4096;
    mem_img_t img = { (uint8_t *)calloc(32, bsize), 32 * bsize };

    /* Superblock at 32768 (block 8) */
    uint8_t *sb = img.data + 32768;
    memcpy(sb, "JFS1", 4);
    put_le32(sb + 4, 2);
    put_le64(sb + 8, 32 * 8);
    put_le32(sb + 16, bsize);
    put_le32(sb + 24, 512);
    strcpy((char *)sb + 144, "JFS_VOL");

    /* Aggregate Inode 16 at 65536 + 16 * 512: points to 2 blocks (20..21) for fileset imap */
    uint8_t *ait16 = img.data + 65536 + 16 * 512;
    put_le32(ait16 + 8, 16);
    put_le64(ait16 + 24, 8192);
    put_le32(ait16 + 40, 1);
    put_le32(ait16 + 52, S_IFREG | 0600);
    ait16[160 + 16] = 0x02; /* BT_LEAF */
    put_le16(ait16 + 160 + 18, 3); /* nextindex = 3 -> entry 2 valid */
    jfs_put_xad(ait16 + 160 + 2 * 16, 0, 2, 20);

    /* IAG 0 sits at block 21 (byte offset 4096 of ait16); inoext[0] points to block 22 */
    uint8_t *iag0 = img.data + 21 * bsize;
    jfs_put_pxd(iag0 + 3072, 4, 22);

    /* Fileset inodes 0..31 sit starting at block 22 */
    uint8_t *f_inodes = img.data + 22 * bsize;

    /* Inode 2: root directory with inline dtroot_t (2 entries: "aix.txt" -> 3, "j.lnk" -> 4) */
    uint8_t *di2 = f_inodes + 2 * 512;
    put_le32(di2 + 8, 2);
    put_le64(di2 + 24, 256);
    put_le32(di2 + 40, 2);
    put_le32(di2 + 52, S_IFDIR | 0755);
    uint8_t *dtroot = di2 + 128;
    dtroot[16] = 0x02; /* BT_LEAF */
    dtroot[17] = 2;    /* 2 entries */
    dtroot[24 + 0] = 1; /* stbl[0] = slot 1 */
    dtroot[24 + 1] = 2; /* stbl[1] = slot 2 */

    /* Slot 1: inum=3, next=-1, namlen=7, UTF-16LE "aix.txt" */
    uint8_t *s1 = dtroot + 1 * 32;
    put_le32(s1 + 0, 3); s1[4] = 0xFF; s1[5] = 7;
    const char *n1 = "aix.txt";
    for (int i = 0; i < 7; i++) put_le16(s1 + 6 + i * 2, (uint8_t)n1[i]);

    /* Slot 2: inum=4, next=-1, namlen=5, UTF-16LE "j.lnk" */
    uint8_t *s2 = dtroot + 2 * 32;
    put_le32(s2 + 0, 4); s2[4] = 0xFF; s2[5] = 5;
    const char *n2 = "j.lnk";
    for (int i = 0; i < 5; i++) put_le16(s2 + 6 + i * 2, (uint8_t)n2[i]);

    /* Inode 3: regular file at block 26 */
    const char *msg = "IBM JFS extent-based file content verified!";
    uint8_t *di3 = f_inodes + 3 * 512;
    put_le32(di3 + 8, 3);
    put_le64(di3 + 24, strlen(msg));
    put_le64(di3 + 32, 1);
    put_le32(di3 + 40, 1);
    put_le32(di3 + 52, S_IFREG | 0644);
    di3[160 + 16] = 0x02;
    put_le16(di3 + 160 + 18, 3);
    jfs_put_xad(di3 + 160 + 2 * 16, 0, 1, 26);
    memcpy(img.data + 26 * bsize, msg, strlen(msg));

    /* Inode 4: fast inline symlink at di4 + 256 (di_nblocks == 0) */
    const char *lnk = "aix.txt";
    uint8_t *di4 = f_inodes + 4 * 512;
    put_le32(di4 + 8, 4);
    put_le64(di4 + 24, strlen(lnk));
    put_le64(di4 + 32, 0);
    put_le32(di4 + 40, 1);
    put_le32(di4 + 52, S_IFLNK | 0777);
    memcpy(di4 + 256, lnk, strlen(lnk));

    verify_file_and_link(&img, LEGACYFS_KIND_JFS, "/aix.txt", msg, "/j.lnk", lnk);
    free(img.data);
}

/* 11. Test all 27 Tier-2 Superblock & Container Probes + Malformed Input Rejection */
static void assert_probe_kind(uint8_t *buf, size_t sz, legacyfs_kind_t expected) {
    mem_img_t m = { buf, sz };
    legacyfs_probe_info_t info;
    assert(legacyfs_probe_io(mem_read_cb, &m, sz, &info) == 0);
    assert(info.kind == expected);
}

static void test_tier2_probes_and_fuzz_bounds(void) {
    size_t sz = 2 * 1024 * 1024;
    uint8_t *buf = (uint8_t *)calloc(1, sz);

    /* Blank image must return -ENOENT */
    mem_img_t empty = { buf, sz };
    legacyfs_probe_info_t info;
    assert(legacyfs_probe_io(mem_read_cb, &empty, sz, &info) == -ENOENT);

    memset(buf, 0, sz); memcpy(buf + 65536, "ReIsEr4", 7); assert_probe_kind(buf, sz, LEGACYFS_KIND_REISER4);
    memset(buf, 0, sz); memcpy(buf + 65536 + 64, "_BHRfS_M", 8); assert_probe_kind(buf, sz, LEGACYFS_KIND_BTRFS);
    memset(buf, 0, sz); memcpy(buf, "XFSB", 4); put_be32(buf + 4, 4096); assert_probe_kind(buf, sz, LEGACYFS_KIND_XFS);
    memset(buf, 0, sz); put_le32(buf + 1024, 0xF2F52010U); assert_probe_kind(buf, sz, LEGACYFS_KIND_F2FS);
    memset(buf, 0, sz); put_le32(buf + 1024, 2); put_le16(buf + 1024 + 6, 0x3434U); assert_probe_kind(buf, sz, LEGACYFS_KIND_NILFS2);
    memset(buf, 0, sz); put_le64(buf + 131072, 0x00bab10cULL); assert_probe_kind(buf, sz, LEGACYFS_KIND_ZFS);
    memset(buf, 0, sz); put_be32(buf + 65536, 0x01161970U); put_be32(buf + 65536 + 4, 1); assert_probe_kind(buf, sz, LEGACYFS_KIND_GFS2);
    memset(buf, 0, sz); memcpy(buf + 4096, "OCFSV2", 6); assert_probe_kind(buf, sz, LEGACYFS_KIND_OCFS2);
    static const uint8_t bcache_magic[16] = {0xc6,0x85,0x73,0xf6,0x66,0xce,0x90,0xa9,0xd9,0x6a,0x60,0xcf,0x80,0x3d,0xf7,0xef};
    memset(buf, 0, sz); memcpy(buf + 4096 + 24, bcache_magic, 16); assert_probe_kind(buf, sz, LEGACYFS_KIND_BCACHEFS);
    memset(buf, 0, sz); put_le32(buf + 512 + 32, 0x42465331U); assert_probe_kind(buf, sz, LEGACYFS_KIND_BEFS);
    memset(buf, 0, sz); put_le32(buf + 8192, 0xF995E849U); put_le32(buf + 8192 + 4, 0xFA53E9C5U); assert_probe_kind(buf, sz, LEGACYFS_KIND_HPFS);
    memset(buf, 0, sz); memcpy(buf, "DOS\1", 4); put_be32(buf + 8, 880); assert_probe_kind(buf, sz, LEGACYFS_KIND_AFFS);
    memset(buf, 0, sz); put_be16(buf + 1024, 0x4244U); put_be32(buf + 1024 + 20, 512); assert_probe_kind(buf, sz, LEGACYFS_KIND_HFS);
    memset(buf, 0, sz); memcpy(buf + 32769, "NSR03", 5); assert_probe_kind(buf, sz, LEGACYFS_KIND_UDF);
    memset(buf, 0, sz); put_le32(buf + 1048576, 0x2fabf15eU); assert_probe_kind(buf, sz, LEGACYFS_KIND_VMFS);
    memset(buf, 0, sz); memcpy(buf + 3, "ReFS\0\0\0\0", 8); assert_probe_kind(buf, sz, LEGACYFS_KIND_REFS);
    memset(buf, 0, sz); memcpy(buf, "WBFS", 4); assert_probe_kind(buf, sz, LEGACYFS_KIND_WBFS);
    memset(buf, 0, sz); memcpy(buf, "FATX", 4); assert_probe_kind(buf, sz, LEGACYFS_KIND_FATX);
    memset(buf, 0, sz); put_le16(buf + 512, 0x002FU); memcpy(buf + 512 + 4, "QNX4", 4); assert_probe_kind(buf, sz, LEGACYFS_KIND_QNX4);
    memset(buf, 0, sz); put_le32(buf + 4096, 0x68191122U); assert_probe_kind(buf, sz, LEGACYFS_KIND_QNX6);
    memset(buf, 0, sz); put_le16(buf, 0x1985U); put_le16(buf + 2, 0xE001U); assert_probe_kind(buf, sz, LEGACYFS_KIND_JFFS2);
    memset(buf, 0, sz); put_le32(buf, 0x06101831U); buf[16] = 6; assert_probe_kind(buf, sz, LEGACYFS_KIND_UBIFS);
    memset(buf, 0, sz); memcpy(buf, "LUKS\xba\xbe", 6); put_be16(buf + 6, 1); assert_probe_kind(buf, sz, LEGACYFS_KIND_LUKS1);
    memset(buf, 0, sz); memcpy(buf, "LUKS\xba\xbe", 6); put_be16(buf + 6, 2); assert_probe_kind(buf, sz, LEGACYFS_KIND_LUKS2);
    memset(buf, 0, sz); memcpy(buf + 512, "LABELONE", 8); memcpy(buf + 512 + 24, "LVM2 001", 8); assert_probe_kind(buf, sz, LEGACYFS_KIND_LVM2);
    memset(buf, 0, sz); put_le32(buf + 4096, 0xa92b4efcU); assert_probe_kind(buf, sz, LEGACYFS_KIND_LINUX_RAID);
    memset(buf, 0, sz); memcpy(buf + 4086, "SWAPSPACE2", 10); assert_probe_kind(buf, sz, LEGACYFS_KIND_LINUX_SWAP);

    free(buf);
}

int main(void) {
    printf("[legacyfs] Testing ReiserFS v3.6 (S+tree, tail-packed + indirect + symlink)...\n");
    test_reiserfs_v36();
    printf("[legacyfs] Testing Minix v3...\n");
    test_minix_v3();
    printf("[legacyfs] Testing System V...\n");
    test_sysv();
    printf("[legacyfs] Testing CramFS (zlib)...\n");
    test_cramfs();
    printf("[legacyfs] Testing ROMFS (with hardlinks)...\n");
    test_romfs();
    printf("[legacyfs] Testing SquashFS 4.0 (zlib)...\n");
    test_squashfs_v4();
    printf("[legacyfs] Testing EROFS (inline tail-packing)...\n");
    test_erofs();
    printf("[legacyfs] Testing ISO9660 + Rock Ridge...\n");
    test_iso9660_rock_ridge();
    printf("[legacyfs] Testing BSD UFS2...\n");
    test_ufs2();
    printf("[legacyfs] Testing IBM JFS...\n");
    test_jfs();
    printf("[legacyfs] Testing all 27 Tier-2 filesystem and container probes...\n");
    test_tier2_probes_and_fuzz_bounds();
    printf("[legacyfs] All 37 filesystem and container tests passed.\n");
    return 0;
}
