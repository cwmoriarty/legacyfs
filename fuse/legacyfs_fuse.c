#define FUSE_USE_VERSION 31
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "legacyfs.h"
#include <errno.h>
#include <fuse.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    legacyfs_t *fs;
    pthread_mutex_t lock;
    int override_uid;
    uid_t uid;
    int override_gid;
    gid_t gid;
} lfs_fuse_state_t;

static lfs_fuse_state_t g_state;

static void attr_to_stat(const legacyfs_attr_t *a, struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_ino = (ino_t)a->ino;
    st->st_mode = (mode_t)(a->mode & ~(mode_t)0222); /* strictly read-only permissions */
    st->st_nlink = (nlink_t)(a->nlink ? a->nlink : 1);
    st->st_uid = g_state.override_uid ? g_state.uid : (uid_t)a->uid;
    st->st_gid = g_state.override_gid ? g_state.gid : (gid_t)a->gid;
    st->st_size = (off_t)a->size;
    st->st_blocks = (blkcnt_t)a->blocks_512;
    st->st_atime = (time_t)a->atime_sec;
    st->st_mtime = (time_t)a->mtime_sec;
    st->st_ctime = (time_t)a->ctime_sec;
}

static int lfs_fuse_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    (void)fi;
    pthread_mutex_lock(&g_state.lock);
    uint64_t ino = 0;
    int rc = legacyfs_lookup_path(g_state.fs, path, &ino);
    if (rc == 0) {
        legacyfs_attr_t attr;
        rc = legacyfs_getattr(g_state.fs, ino, &attr);
        if (rc == 0) {
            attr_to_stat(&attr, stbuf);
        }
    }
    pthread_mutex_unlock(&g_state.lock);
    return rc;
}

typedef struct {
    void *buf;
    fuse_fill_dir_t filler;
} fuse_dir_pack_t;

static int fuse_readdir_cb(void *user_ctx, const char *name, uint64_t ino, legacyfs_ftype_t ftype) {
    fuse_dir_pack_t *pk = (fuse_dir_pack_t *)user_ctx;
    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_ino = (ino_t)ino;
    if (ftype == LEGACYFS_FT_DIR) st.st_mode = S_IFDIR | 0555;
    else if (ftype == LEGACYFS_FT_SYMLINK) st.st_mode = S_IFLNK | 0777;
    else st.st_mode = S_IFREG | 0444;
    return pk->filler(pk->buf, name, &st, 0, (enum fuse_fill_dir_flags)0);
}

static int lfs_fuse_readdir(
    const char *path,
    void *buf,
    fuse_fill_dir_t filler,
    off_t offset,
    struct fuse_file_info *fi,
    enum fuse_readdir_flags flags
) {
    (void)offset;
    (void)fi;
    (void)flags;

    pthread_mutex_lock(&g_state.lock);
    uint64_t ino = 0;
    int rc = legacyfs_lookup_path(g_state.fs, path, &ino);
    if (rc == 0) {
        filler(buf, ".", NULL, 0, (enum fuse_fill_dir_flags)0);
        filler(buf, "..", NULL, 0, (enum fuse_fill_dir_flags)0);
        fuse_dir_pack_t pk = { buf, filler };
        rc = legacyfs_readdir(g_state.fs, ino, fuse_readdir_cb, &pk);
    }
    pthread_mutex_unlock(&g_state.lock);
    return rc;
}

static int lfs_fuse_open(const char *path, struct fuse_file_info *fi) {
    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        return -EROFS;
    }
    pthread_mutex_lock(&g_state.lock);
    uint64_t ino = 0;
    int rc = legacyfs_lookup_path(g_state.fs, path, &ino);
    if (rc == 0) {
        fi->fh = ino;
    }
    pthread_mutex_unlock(&g_state.lock);
    return rc;
}

static int lfs_fuse_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    if (offset < 0) return -EINVAL;
    pthread_mutex_lock(&g_state.lock);
    uint64_t ino = (fi && fi->fh != 0) ? fi->fh : 0;
    int rc = 0;
    if (ino == 0) {
        rc = legacyfs_lookup_path(g_state.fs, path, &ino);
    }
    size_t got = 0;
    if (rc == 0) {
        rc = legacyfs_read(g_state.fs, ino, (uint64_t)offset, size, buf, &got);
    }
    pthread_mutex_unlock(&g_state.lock);
    return rc < 0 ? rc : (int)got;
}

static int lfs_fuse_readlink(const char *path, char *buf, size_t size) {
    pthread_mutex_lock(&g_state.lock);
    uint64_t ino = 0;
    int rc = legacyfs_lookup_path(g_state.fs, path, &ino);
    if (rc == 0) {
        size_t got = 0;
        rc = legacyfs_readlink(g_state.fs, ino, buf, size, &got);
    }
    pthread_mutex_unlock(&g_state.lock);
    return rc;
}

static int lfs_fuse_statfs(const char *path, struct statvfs *stbuf) {
    (void)path;
    memset(stbuf, 0, sizeof(*stbuf));
    pthread_mutex_lock(&g_state.lock);
    legacyfs_statfs_t s;
    int rc = legacyfs_statfs(g_state.fs, &s);
    if (rc == 0) {
        stbuf->f_bsize = s.block_size;
        stbuf->f_frsize = s.block_size;
        stbuf->f_blocks = (fsblkcnt_t)s.total_blocks;
        stbuf->f_bfree = 0;
        stbuf->f_bavail = 0;
        stbuf->f_namemax = s.namemax;
    }
    pthread_mutex_unlock(&g_state.lock);
    return rc;
}

static void lfs_fuse_destroy(void *priv) {
    (void)priv;
    if (g_state.fs) {
        legacyfs_close(g_state.fs);
        g_state.fs = NULL;
    }
    pthread_mutex_destroy(&g_state.lock);
}

static const struct fuse_operations k_fuse_ops = {
    .getattr  = lfs_fuse_getattr,
    .readlink = lfs_fuse_readlink,
    .open     = lfs_fuse_open,
    .read     = lfs_fuse_read,
    .statfs   = lfs_fuse_statfs,
    .readdir  = lfs_fuse_readdir,
    .destroy  = lfs_fuse_destroy,
};

/* Parse -o options list, extracting uid=/gid= and filtering options unknown to FUSE3/FUSE-T */
static void filter_mount_options(const char *raw_opts, char *out_opts, size_t out_cap) {
    snprintf(out_opts, out_cap, "ro");
    if (!raw_opts || !raw_opts[0]) return;

    char *copy = strdup(raw_opts);
    if (!copy) return;

    char *saveptr = NULL;
    for (char *tok = strtok_r(copy, ",", &saveptr); tok != NULL; tok = strtok_r(NULL, ",", &saveptr)) {
        if (strncmp(tok, "uid=", 4) == 0) {
            g_state.override_uid = 1;
            g_state.uid = (uid_t)strtoul(tok + 4, NULL, 10);
        } else if (strncmp(tok, "gid=", 4) == 0) {
            g_state.override_gid = 1;
            g_state.gid = (gid_t)strtoul(tok + 4, NULL, 10);
        } else if (strcmp(tok, "allow_other") == 0 ||
                   strcmp(tok, "default_permissions") == 0 ||
                   strncmp(tok, "fsname=", 7) == 0 ||
                   strncmp(tok, "subtype=", 8) == 0 ||
                   strncmp(tok, "volname=", 8) == 0) {
#ifndef __APPLE__
            if (strncmp(tok, "volname=", 8) == 0) continue;
#endif
            size_t cur = strlen(out_opts);
            snprintf(out_opts + cur, out_cap - cur, ",%s", tok);
        }
    }
    free(copy);
}

int main(int argc, char **argv) {
    memset(&g_state, 0, sizeof(g_state));
    pthread_mutex_init(&g_state.lock, NULL);

    legacyfs_kind_t forced_kind = legacyfs_kind_from_name(argv[0]);
    const char *device_path = NULL;
    const char *mountpoint = NULL;
    int foreground = 0;
    int debug = 0;
    char filtered_opts[512];
    snprintf(filtered_opts, sizeof(filtered_opts), "ro");

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-f") == 0) {
            foreground = 1;
        } else if (strcmp(a, "-d") == 0) {
            debug = 1;
            foreground = 1;
        } else if (strcmp(a, "-s") == 0) {
            /* single-threaded accepted */
        } else if (strcmp(a, "-r") == 0) {
            /* read-only is always enforced */
        } else if (strcmp(a, "-t") == 0 && i + 1 < argc) {
            forced_kind = legacyfs_kind_from_name(argv[++i]);
        } else if (strcmp(a, "-o") == 0 && i + 1 < argc) {
            filter_mount_options(argv[++i], filtered_opts, sizeof(filtered_opts));
        } else if (strncmp(a, "-o", 2) == 0 && a[2] != '\0') {
            filter_mount_options(a + 2, filtered_opts, sizeof(filtered_opts));
        } else if (strcmp(a, "--version") == 0 || strcmp(a, "-V") == 0) {
            printf("legacyfs-mount %s\n", legacyfs_version());
            return 0;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            printf("Usage: %s [-f] [-t <format>] [-o options] <device|image> <mountpoint>\n", argv[0]);
            return 0;
        } else if (a[0] != '-') {
            if (!device_path) device_path = a;
            else if (!mountpoint) mountpoint = a;
        }
    }

    if (!device_path || !mountpoint) {
        fprintf(stderr, "Usage: %s [-f] [-t <format>] [-o options] <device|image> <mountpoint>\n", argv[0]);
        return 1;
    }

    int rc = legacyfs_open_path(device_path, forced_kind, &g_state.fs);
    if (rc != 0) {
        fprintf(stderr, "%s: failed to open %s: %s\n", argv[0], device_path, strerror(-rc));
        return 1;
    }

    char *fuse_argv[8];
    int fuse_argc = 0;
    fuse_argv[fuse_argc++] = argv[0];
    if (foreground) fuse_argv[fuse_argc++] = "-f";
    if (debug)      fuse_argv[fuse_argc++] = "-d";
    fuse_argv[fuse_argc++] = "-o";
    fuse_argv[fuse_argc++] = filtered_opts;
    fuse_argv[fuse_argc++] = (char *)mountpoint;
    fuse_argv[fuse_argc] = NULL;

    return fuse_main(fuse_argc, fuse_argv, &k_fuse_ops, NULL);
}
