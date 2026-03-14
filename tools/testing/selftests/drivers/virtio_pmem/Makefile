# SPDX-License-Identifier: GPL-2.0-only
CFLAGS += $(KHDR_INCLUDES)

TEST_GEN_PROGS := virtio_pmem_dax_test

# run_qemu_test.sh is an optional integration test requiring QEMU and ndctl.
# It is listed as TEST_PROGS so 'make run_tests' will execute it too.
TEST_PROGS := run_qemu_test.sh

top_srcdir ?= ../../../../..

include ../../lib.mk
