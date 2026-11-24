CC       ?= cc
AR       ?= ar
PREFIX   ?= /usr/local
DESTDIR  ?=
BUILD    ?= build

CFLAGS   ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS += -Iinclude -Isrc
LDLIBS   := -lz

UNAME_S  := $(shell uname -s)
FUSE_PREFIX ?= /usr/local

# Auto-detect libfuse3 / FUSE-T unless WITH_FUSE=0 is explicitly passed
WITH_FUSE ?= auto
ifeq ($(WITH_FUSE),auto)
  ifneq ($(wildcard $(FUSE_PREFIX)/include/fuse3/fuse.h),)
    ENABLE_FUSE := 1
    FUSE_CFLAGS := -I$(FUSE_PREFIX)/include/fuse3
    ifeq ($(UNAME_S),Darwin)
      FUSE_LIBS := -L$(FUSE_PREFIX)/lib -lfuse3 -Wl,-rpath,/usr/local/lib -lpthread
    else
      FUSE_LIBS := -L$(FUSE_PREFIX)/lib -lfuse3 -lpthread
    endif
  else ifneq ($(wildcard /usr/include/fuse3/fuse.h),)
    ENABLE_FUSE := 1
    FUSE_CFLAGS := -I/usr/include/fuse3
    FUSE_LIBS   := -lfuse3 -lpthread
  else
    ENABLE_FUSE := 0
  endif
else ifeq ($(WITH_FUSE),1)
  ENABLE_FUSE := 1
  ifneq ($(wildcard $(FUSE_PREFIX)/include/fuse3/fuse.h),)
    FUSE_CFLAGS ?= -I$(FUSE_PREFIX)/include/fuse3
    ifeq ($(UNAME_S),Darwin)
      FUSE_LIBS ?= -L$(FUSE_PREFIX)/lib -lfuse3 -Wl,-rpath,/usr/local/lib -lpthread
    else
      FUSE_LIBS ?= -L$(FUSE_PREFIX)/lib -lfuse3 -lpthread
    endif
  else
    FUSE_CFLAGS ?= -I/usr/include/fuse3
    FUSE_LIBS   ?= -lfuse3 -lpthread
  endif
else
  ENABLE_FUSE := 0
endif

LIB_SRCS := \
	src/core.c \
	src/probe.c \
	src/fs_reiserfs.c \
	src/fs_minix_sysv.c \
	src/fs_cramfs_romfs.c \
	src/fs_squashfs_erofs.c \
	src/fs_iso9660.c \
	src/fs_ufs_jfs.c

LIB_OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(LIB_SRCS))
STATIC_LIB := $(BUILD)/liblegacyfs.a
CLI_BIN    := $(BUILD)/legacyfs
FUSE_BIN   := $(BUILD)/legacyfs-mount
TEST_BIN   := $(BUILD)/test_legacyfs

FUSE_ALIASES := \
	fuse-reiserfs \
	fuse-minix \
	fuse-cramfs \
	fuse-romfs \
	fuse-iso9660 \
	fuse-squashfs \
	fuse-erofs \
	fuse-ufs \
	fuse-jfs \
	fuse-sysv

TARGETS := $(STATIC_LIB) $(CLI_BIN)
ifeq ($(ENABLE_FUSE),1)
  TARGETS += $(FUSE_BIN)
endif

.PHONY: all clean check check-asan install dist

all: $(TARGETS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c include/legacyfs.h src/internal.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(STATIC_LIB): $(LIB_OBJS)
	rm -f $@
	$(AR) rcs $@ $(LIB_OBJS)

$(CLI_BIN): tools/legacyfs_cli.c $(STATIC_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(STATIC_LIB) $(LDLIBS) -o $@

$(FUSE_BIN): fuse/legacyfs_fuse.c $(STATIC_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FUSE_CFLAGS) $< $(STATIC_LIB) $(LDLIBS) $(FUSE_LIBS) -o $@

$(TEST_BIN): tests/test_legacyfs.c $(STATIC_LIB)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(STATIC_LIB) $(LDLIBS) -o $@

check: all $(TEST_BIN)
	./$(TEST_BIN)
	./$(CLI_BIN) formats >/dev/null
	./$(CLI_BIN) formats --json >/dev/null

check-asan: | $(BUILD)
	$(CC) $(CPPFLAGS) -O1 -g -Wall -Wextra -std=c11 -fsanitize=address,undefined \
		$(LIB_SRCS) tests/test_legacyfs.c $(LDLIBS) -o $(BUILD)/test_legacyfs_asan
	./$(BUILD)/test_legacyfs_asan

install: all
	mkdir -p "$(DESTDIR)$(PREFIX)/include" "$(DESTDIR)$(PREFIX)/lib" "$(DESTDIR)$(PREFIX)/bin"
	cp -p include/legacyfs.h "$(DESTDIR)$(PREFIX)/include/legacyfs.h"
	cp -p $(STATIC_LIB) "$(DESTDIR)$(PREFIX)/lib/liblegacyfs.a"
	cp -p $(CLI_BIN) "$(DESTDIR)$(PREFIX)/bin/legacyfs"
	@if [ "$(ENABLE_FUSE)" = "1" ]; then \
		cp -p $(FUSE_BIN) "$(DESTDIR)$(PREFIX)/bin/legacyfs-mount"; \
		for alias in $(FUSE_ALIASES); do \
			ln -sf legacyfs-mount "$(DESTDIR)$(PREFIX)/bin/$$alias"; \
		done; \
	fi

clean:
	rm -rf $(BUILD)
