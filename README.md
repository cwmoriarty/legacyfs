# legacyfs

`legacyfs` is a portable, bounds-checked C11 library (`liblegacyfs.a`), command-line inspector (`legacyfs`), and read-only FUSE3 / FUSE-T driver (`legacyfs-mount`) for mounting and reading archival, embedded, and legacy filesystems in user space on macOS and Linux.

## Capabilities

### Read-Only Mount and Extraction (10 Filesystem Families)
Full superblock validation, directory listing, path lookup, symlink resolution, and file reading:
- **ReiserFS** (`3.5` and `3.6`, balanced S+tree traversal, tail-packed direct items, and indirect blocks — removed from mainline Linux in 6.13)
- **UFS / FFS** (`UFS1` and `UFS2` BSD Fast File System, direct and indirect blocks, fast inline symlinks)
- **JFS** (`JFS1` IBM Journaled File System, fileset inode allocation maps, inline and B+tree directories, `xtree` file extents)
- **SquashFS** (`4.0`, uncompressed and `zlib`-compressed metadata, data blocks, and tail fragments)
- **EROFS** (`1.0`, compact 32B and extended 64B inodes, flat-plain and flat-inline tail-packed layouts)
- **ISO9660** (ECMA-119 with POSIX Rock Ridge `PX`, `NM`, and `SL` extensions)
- **CramFS** (little-endian Compressed ROMFS with `zlib` page decompression)
- **ROMFS** (`-rom1fs-` big-endian header chain including hardlink resolution)
- **Minix** (`v1`, `v2`, and `v3` with 14/30/60-character directory entries and indirect zones)
- **SysV** (System V filesystem with 24-bit packed block pointers and indirect blocks)

### Superblock and Container Identification (`legacyfs_probe`, 37 Formats Total)
In addition to the 10 mountable families above, `legacyfs_probe` identifies and extracts metadata (version, label, UUID, block size, and capacity) for:
`Reiser4`, `Btrfs`, `XFS`, `F2FS`, `NILFS2`, `ZFS`, `GFS2`, `OCFS2`, `BCacheFS`, `BeFS`, `HPFS`, `Amiga AFFS`, `Classic Mac HFS`, `UDF`, `VMware VMFS`, `Microsoft ReFS`, `Wii WBFS`, `Xbox FATX`, `QNX4`, `QNX6`, `JFFS2`, `UBIFS`, `LUKS1`, `LUKS2`, `LVM2`, `Linux MD RAID`, and `Linux Swap`.

## Build and Test

Build the static library, CLI, and FUSE3 driver (auto-detects Linux `libfuse3` and macOS `FUSE-T`):

```bash
make
```

Run the synthetic filesystem image test suite and sanitizer checks:

```bash
make check
make check-asan
```

Install headers, `liblegacyfs.a`, `legacyfs`, `legacyfs-mount`, and per-format aliases (`fuse-reiserfs`, `fuse-ufs`, `fuse-jfs`, `fuse-squashfs`, `fuse-erofs`, `fuse-iso9660`, `fuse-cramfs`, `fuse-romfs`, `fuse-minix`, `fuse-sysv`):

```bash
sudo make install PREFIX=/usr/local
```

## Usage

Inspect an image or block device:

```bash
legacyfs probe disk.img
legacyfs probe --json disk.img
```

Browse or extract files without mounting:

```bash
legacyfs ls disk.img /
legacyfs cat disk.img /etc/hostname
legacyfs extract disk.img ./recovered_files
```

Mount read-only via FUSE3 (Linux) or FUSE-T (macOS):

```bash
fuse-reiserfs disk.img /mnt/reiser
```

## Downstream Integrations

- **[Montserrat](https://github.com/cwmoriarty/montserrat):** Uses `CLegacyFS` and `legacyfs-mount` for superblock identification across 37 formats and read-only macOS FUSE-T / FSKit mounting.
- **[TestDisk-NG](https://github.com/cwmoriarty/TestDisk-NG):** Links `liblegacyfs.a` so `[P] List Files` and `[C] Copy` can browse and recover files directly from ReiserFS, UFS/UFS2, JFS, SquashFS, EROFS, ISO9660, CramFS, ROMFS, Minix, and SysV partitions.

## License

MIT License.
