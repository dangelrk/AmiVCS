# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ronny Krueger
#
# Host unit tests for libcloudvcs (vcsname.c, diff.c, merge3.c; no network, no libcurl).
#   make         build and run
#   make clean

CC      ?= cc
SAN     ?= -fsanitize=address,undefined -fno-omit-frame-pointer
ROOT     = ../../..
CORE     = $(ROOT)/core
CFLAGS   = -std=c99 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -g -O1 -Wall -Wextra $(SAN) \
           -I../include/cloudvcs -I$(CORE)/libcloudcore/include/cloudcore \
           -I$(CORE)/libcloudfs/include/cloudfs
SRCS     = test_vcsname.c ../src/vcsname.c $(CORE)/libcloudcore/src/charset.c \
           $(CORE)/libcloudfs/src/localfs_pc.c $(CORE)/libcloudfs/src/localfs_util.c \
           $(CORE)/libcloudcore/src/md5.c $(CORE)/libcloudcore/src/ui_hooks.c \
           $(CORE)/libcloudcore/src/progress.c
OBJDIR   = obj
BIN      = $(OBJDIR)/test_vcsname
DIFFBIN  = $(OBJDIR)/test_diff
MERGEBIN = $(OBJDIR)/test_merge3

.PHONY: all run clean
all: run
run: $(BIN) $(DIFFBIN) $(MERGEBIN)
	$(BIN)
	$(DIFFBIN)
	$(MERGEBIN)
$(DIFFBIN): test_diff.c ../src/diff.c ../include/cloudvcs/diff.h | $(OBJDIR)
	$(CC) $(CFLAGS) -o $@ test_diff.c ../src/diff.c
$(MERGEBIN): test_merge3.c ../src/merge3.c ../src/diff.c ../include/cloudvcs/merge3.h ../include/cloudvcs/diff.h | $(OBJDIR)
	$(CC) $(CFLAGS) -o $@ test_merge3.c ../src/merge3.c ../src/diff.c
$(BIN): $(SRCS) ../include/cloudvcs/vcsname.h | $(OBJDIR)
	$(CC) $(CFLAGS) -o $@ $(SRCS)
$(OBJDIR):
	mkdir -p $@
clean:
	rm -rf $(OBJDIR)
