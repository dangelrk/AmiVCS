# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ronny Krueger
#
# Host unit tests for libcloudvcs (vcsname.c; no network, no libcurl).
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

.PHONY: all run clean
all: run
run: $(BIN)
	$(BIN)
$(BIN): $(SRCS) ../include/cloudvcs/vcsname.h | $(OBJDIR)
	$(CC) $(CFLAGS) -o $@ $(SRCS)
$(OBJDIR):
	mkdir -p $@
clean:
	rm -rf $(OBJDIR)
