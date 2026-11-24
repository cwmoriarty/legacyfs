#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "legacyfs.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void print_json_escaped(const char *s) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
        if (*p == '"' || *p == '\\') {
            putchar('\\');
            putchar((char)*p);
        } else if (*p >= 0x20) {
            putchar((char)*p);
        }
    }
    putchar('"');
}

static void usage(void) {
    fprintf(stderr,
        "legacyfs %s - Read-only archival and legacy filesystem toolkit\n\n"
        "Usage:\n"
        "  legacyfs probe [--json] <image|device>\n"
        "  legacyfs ls <image|device> [path]\n"
        "  legacyfs cat <image|device> <path>\n"
        "  legacyfs extract <image|device> <dest_dir>\n"
        "  legacyfs formats [--json]\n"
        "  legacyfs version\n",
        legacyfs_version());
}

static int cmd_formats(int as_json) {
    const legacyfs_format_desc_t *table = NULL;
    size_t count = legacyfs_supported_formats(&table);
    if (as_json) {
        printf("[\n");
        for (size_t i = 0; i < count; i++) {
            printf("  {\"name\": ");
            print_json_escaped(table[i].short_name);
            printf(", \"displayName\": ");
            print_json_escaped(table[i].display_name);
            printf(", \"canMountReadOnly\": %s, \"isContainer\": %s}%s\n",
                   table[i].can_mount_ro ? "true" : "false",
                   table[i].is_container ? "true" : "false",
                   (i + 1 < count) ? "," : "");
        }
        printf("]\n");
        return 0;
    }

    printf("%-14s %-22s %-12s %s\n", "FORMAT", "DISPLAY NAME", "TIER", "FUSE DRIVER");
    for (size_t i = 0; i < count; i++) {
        const char *tier = table[i].can_mount_ro ? "mount-ro" : (table[i].is_container ? "container" : "probe-only");
        printf("%-14s %-22s %-12s %s\n",
               table[i].short_name,
               table[i].display_name,
               tier,
               table[i].fuse_binary ? table[i].fuse_binary : "-");
    }
    return 0;
}

static int cmd_probe(const char *target, int as_json) {
    legacyfs_probe_info_t info;
    int rc = legacyfs_probe_path(target, &info);
    if (rc != 0) {
        if (as_json) {
            printf("{\"recognized\": false, \"error\": %d}\n", -rc);
        } else {
            fprintf(stderr, "No recognized legacy filesystem or container on %s (%s)\n", target, strerror(-rc));
        }
        return 1;
    }

    const legacyfs_format_desc_t *desc = legacyfs_describe_kind(info.kind);
    if (as_json) {
        printf("{\n  \"recognized\": true,\n  \"format\": ");
        print_json_escaped(desc->short_name);
        printf(",\n  \"displayName\": ");
        print_json_escaped(info.name);
        printf(",\n  \"version\": ");
        print_json_escaped(info.version_detail);
        printf(",\n  \"label\": ");
        print_json_escaped(info.label);
        printf(",\n  \"uuid\": ");
        print_json_escaped(info.uuid);
        printf(",\n  \"blockSize\": %u,\n  \"totalBytes\": %llu,\n  \"canMountReadOnly\": %s,\n  \"isContainer\": %s,\n  \"needsRecovery\": %s\n}\n",
               info.block_size,
               (unsigned long long)info.total_bytes,
               info.can_mount_ro ? "true" : "false",
               info.is_container ? "true" : "false",
               info.needs_recovery ? "true" : "false");
        return 0;
    }

    printf("Target:      %s\n", target);
    printf("Format:      %s (%s)\n", info.name, desc->short_name);
    if (info.version_detail[0]) printf("Version:     %s\n", info.version_detail);
    if (info.label[0])          printf("Label:       %s\n", info.label);
    if (info.uuid[0])           printf("UUID:        %s\n", info.uuid);
    if (info.block_size)        printf("Block Size:  %u B\n", info.block_size);
    if (info.total_bytes)       printf("Total Size:  %llu B\n", (unsigned long long)info.total_bytes);
    printf("Capability:  %s\n", info.can_mount_ro ? "Read-only mount and extraction" : (info.is_container ? "Container identification" : "Superblock identification"));
    return 0;
}

typedef struct {
    legacyfs_t *fs;
} cli_ls_ctx_t;

static int cli_ls_cb(void *user_ctx, const char *name, uint64_t ino, legacyfs_ftype_t ftype) {
    cli_ls_ctx_t *ctx = (cli_ls_ctx_t *)user_ctx;
    legacyfs_attr_t attr;
    memset(&attr, 0, sizeof(attr));
    legacyfs_getattr(ctx->fs, ino, &attr);
    char tc = '?';
    if (ftype == LEGACYFS_FT_DIR) tc = 'd';
    else if (ftype == LEGACYFS_FT_REG) tc = '-';
    else if (ftype == LEGACYFS_FT_SYMLINK) tc = 'l';
    printf("%c%04o %10llu %s\n", tc, attr.mode & 07777U, (unsigned long long)attr.size, name);
    return 0;
}

static int cmd_ls(const char *target, const char *path) {
    legacyfs_t *fs = NULL;
    int rc = legacyfs_open_path(target, LEGACYFS_KIND_UNKNOWN, &fs);
    if (rc != 0) {
        fprintf(stderr, "open failed: %s\n", strerror(-rc));
        return 1;
    }
    uint64_t ino = 0;
    rc = legacyfs_lookup_path(fs, path ? path : "/", &ino);
    if (rc != 0) {
        fprintf(stderr, "lookup '%s' failed: %s\n", path ? path : "/", strerror(-rc));
        legacyfs_close(fs);
        return 1;
    }
    cli_ls_ctx_t ctx = { fs };
    rc = legacyfs_readdir(fs, ino, cli_ls_cb, &ctx);
    legacyfs_close(fs);
    if (rc != 0) {
        fprintf(stderr, "readdir failed: %s\n", strerror(-rc));
        return 1;
    }
    return 0;
}

static int cmd_cat(const char *target, const char *path) {
    legacyfs_t *fs = NULL;
    int rc = legacyfs_open_path(target, LEGACYFS_KIND_UNKNOWN, &fs);
    if (rc != 0) {
        fprintf(stderr, "open failed: %s\n", strerror(-rc));
        return 1;
    }
    uint64_t ino = 0;
    rc = legacyfs_lookup_path(fs, path, &ino);
    if (rc != 0) {
        fprintf(stderr, "lookup '%s' failed: %s\n", path, strerror(-rc));
        legacyfs_close(fs);
        return 1;
    }
    legacyfs_attr_t attr;
    rc = legacyfs_getattr(fs, ino, &attr);
    if (rc != 0) {
        legacyfs_close(fs);
        return 1;
    }

    uint8_t buf[65536];
    uint64_t off = 0;
    while (off < attr.size) {
        size_t got = 0;
        rc = legacyfs_read(fs, ino, off, sizeof(buf), buf, &got);
        if (rc != 0 || got == 0) break;
        if (fwrite(buf, 1, got, stdout) != got) {
            rc = -EIO;
            break;
        }
        off += got;
    }
    legacyfs_close(fs);
    return rc == 0 ? 0 : 1;
}

static int is_safe_component(const char *name) {
    if (!name || !name[0]) return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    if (strchr(name, '/') != NULL) return 0;
    return 1;
}

typedef struct {
    legacyfs_t *fs;
    const char *dest_dir;
    int depth;
    int err;
} extract_ctx_t;

static int extract_dir(legacyfs_t *fs, uint64_t dir_ino, const char *dest_dir, int depth);

static int extract_cb(void *user_ctx, const char *name, uint64_t ino, legacyfs_ftype_t ftype) {
    extract_ctx_t *ectx = (extract_ctx_t *)user_ctx;
    if (!is_safe_component(name)) return 0;

    char child_path[PATH_MAX];
    int n = snprintf(child_path, sizeof(child_path), "%s/%s", ectx->dest_dir, name);
    if (n <= 0 || (size_t)n >= sizeof(child_path)) {
        ectx->err = -ENAMETOOLONG;
        return 1;
    }

    if (ftype == LEGACYFS_FT_DIR) {
        if (mkdir(child_path, 0755) != 0 && errno != EEXIST) {
            ectx->err = -errno;
            return 1;
        }
        int rc = extract_dir(ectx->fs, ino, child_path, ectx->depth + 1);
        if (rc != 0) {
            ectx->err = rc;
            return 1;
        }
    } else if (ftype == LEGACYFS_FT_REG) {
        legacyfs_attr_t attr;
        if (legacyfs_getattr(ectx->fs, ino, &attr) != 0) return 0;
        FILE *fp = fopen(child_path, "wb");
        if (!fp) {
            ectx->err = -errno;
            return 1;
        }
        uint8_t buf[32768];
        uint64_t off = 0;
        while (off < attr.size) {
            size_t got = 0;
            int rc = legacyfs_read(ectx->fs, ino, off, sizeof(buf), buf, &got);
            if (rc != 0 || got == 0) {
                ectx->err = rc ? rc : -EIO;
                break;
            }
            if (fwrite(buf, 1, got, fp) != got) {
                ectx->err = -EIO;
                break;
            }
            off += got;
        }
        fclose(fp);
        chmod(child_path, (attr.mode & 0777U) | 0400U);
    } else if (ftype == LEGACYFS_FT_SYMLINK) {
        char link_target[PATH_MAX];
        size_t llen = 0;
        if (legacyfs_readlink(ectx->fs, ino, link_target, sizeof(link_target), &llen) == 0) {
            unlink(child_path);
            if (symlink(link_target, child_path) != 0) {
                /* Ignore symlink permission errors on restricted destinations */
            }
        }
    }
    return ectx->err != 0 ? 1 : 0;
}

static int extract_dir(legacyfs_t *fs, uint64_t dir_ino, const char *dest_dir, int depth) {
    if (depth > 32) return -ELOOP;
    if (mkdir(dest_dir, 0755) != 0 && errno != EEXIST) {
        return -errno;
    }
    extract_ctx_t ectx = { fs, dest_dir, depth, 0 };
    int rc = legacyfs_readdir(fs, dir_ino, extract_cb, &ectx);
    return ectx.err != 0 ? ectx.err : rc;
}

static int cmd_extract(const char *target, const char *dest_dir) {
    legacyfs_t *fs = NULL;
    int rc = legacyfs_open_path(target, LEGACYFS_KIND_UNKNOWN, &fs);
    if (rc != 0) {
        fprintf(stderr, "open failed: %s\n", strerror(-rc));
        return 1;
    }
    rc = extract_dir(fs, legacyfs_root_ino(fs), dest_dir, 0);
    legacyfs_close(fs);
    if (rc != 0) {
        fprintf(stderr, "extract failed: %s\n", strerror(-rc));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage();
        return 1;
    }

    const char *cmd = argv[1];
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
        printf("legacyfs %s\n", legacyfs_version());
        return 0;
    }
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        usage();
        return 0;
    }
    if (strcmp(cmd, "formats") == 0) {
        int as_json = (argc >= 3 && strcmp(argv[2], "--json") == 0);
        return cmd_formats(as_json);
    }
    if (strcmp(cmd, "probe") == 0) {
        int as_json = 0;
        const char *target = NULL;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--json") == 0) as_json = 1;
            else if (argv[i][0] != '-') target = argv[i];
        }
        if (!target) {
            usage();
            return 1;
        }
        return cmd_probe(target, as_json);
    }
    if (strcmp(cmd, "ls") == 0) {
        if (argc < 3) { usage(); return 1; }
        return cmd_ls(argv[2], argc >= 4 ? argv[3] : "/");
    }
    if (strcmp(cmd, "cat") == 0) {
        if (argc < 4) { usage(); return 1; }
        return cmd_cat(argv[2], argv[3]);
    }
    if (strcmp(cmd, "extract") == 0) {
        if (argc < 4) { usage(); return 1; }
        return cmd_extract(argv[2], argv[3]);
    }

    usage();
    return 1;
}
