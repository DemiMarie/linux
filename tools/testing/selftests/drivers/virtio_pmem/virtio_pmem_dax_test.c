// SPDX-License-Identifier: GPL-2.0
/*
 * virtio_pmem_dax_test.c: self-test for virtio-pmem DAX support
 *
 * Tests performed (all require a virtio-pmem-backed /dev/daxN.M device):
 *
 *  1. map_sync_rejected: mmap(MAP_SHARED_VALIDATE|MAP_SYNC) must fail with
 *     EOPNOTSUPP, because virtio-pmem requires an explicit async virtqueue
 *     flush for persistence and cannot honour MAP_SYNC semantics.
 *
 *  2. dax_read_write: mmap(MAP_SHARED) write + read-back via the DAX mapping.
 *
 *  3. zero_page_range: fill a page, then zero it; verify it reads back as 0.
 *
 *  4. phys_addr_matches: read the physical base address from the ND region's
 *     "resource" sysfs attribute, then verify that /proc/self/pagemap reports
 *     the same physical address for the mmap'd page.
 *
 * The test SKIPs gracefully if no virtio-pmem region or DAX device is found.
 * It must be run as root (pagemap access and DAX device open require root).
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include "../../kselftest.h"

#ifndef MAP_SYNC
#define MAP_SYNC 0x80000
#endif
#ifndef MAP_SHARED_VALIDATE
#define MAP_SHARED_VALIDATE 0x03
#endif

#define PATTERN		0xAB
#define PAGE_SIZE_DEFAULT	4096UL

/* Sysfs root for nvdimm bus regions */
#define ND_BUS_PATH	"/sys/bus/nd/devices"
/* Sysfs root for dax bus devices */
#define DAX_BUS_PATH	"/sys/bus/dax/devices"
/* Virtio-pmem provider name as set in virtio_pmem_probe() */
#define VIRTIO_PMEM_PROVIDER	"virtio-pmem"

static long page_size;

/*
 * Read a sysfs attribute into @buf (up to @size bytes), stripping trailing
 * newlines.  Returns the number of bytes stored (>= 0) or -1 on error.
 */
static int sysfs_read(const char *path, char *buf, size_t size)
{
	int fd;
	ssize_t n;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, size - 1);
	close(fd);
	if (n < 0)
		return -1;
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		n--;
	buf[n] = '\0';
	return (int)n;
}

/*
 * Find the first ND region backed by virtio-pmem.
 * Returns a heap-allocated region name (e.g. "region0"), or NULL.
 */
static char *find_virtio_pmem_region(void)
{
	DIR *d;
	struct dirent *ent;
	char path[512], val[128];

	d = opendir(ND_BUS_PATH);
	if (!d)
		return NULL;

	while ((ent = readdir(d)) != NULL) {
		if (strncmp(ent->d_name, "region", 6) != 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s/provider",
			 ND_BUS_PATH, ent->d_name);
		if (sysfs_read(path, val, sizeof(val)) < 0)
			continue;
		if (strcmp(val, VIRTIO_PMEM_PROVIDER) == 0) {
			closedir(d);
			return strdup(ent->d_name);
		}
	}
	closedir(d);
	return NULL;
}

/*
 * Given a region name (e.g. "region0"), find the first dax device
 * that lives under it (e.g. "dax0.0") and return its /dev path.
 * Returns a heap-allocated string, or NULL if not found.
 */
static char *find_dax_dev_for_region(const char *region)
{
	DIR *d;
	struct dirent *ent;
	char path[512];
	struct stat st;

	d = opendir(DAX_BUS_PATH);
	if (!d)
		return NULL;

	while ((ent = readdir(d)) != NULL) {
		int dax_n;
		char region_n_str[32];

		if (strncmp(ent->d_name, "dax", 3) != 0)
			continue;

		/*
		 * dax<N>.<M> belongs to region<N>.
		 */
		if (sscanf(ent->d_name, "dax%d.", &dax_n) != 1)
			continue;

		snprintf(region_n_str, sizeof(region_n_str), "region%d", dax_n);
		if (strcmp(region_n_str, region) != 0)
			continue;

		snprintf(path, sizeof(path), "/dev/%s", ent->d_name);
		if (stat(path, &st) == 0 && S_ISCHR(st.st_mode)) {
			closedir(d);
			return strdup(path);
		}
	}
	closedir(d);
	return NULL;
}

/*
 * Read the physical base address of the ND region from its "resource" sysfs
 * attribute (admin-readable hex value, e.g. "0x140000000").
 * Returns the address or 0 on error.
 */
static uint64_t read_region_phys_base(const char *region)
{
	char path[512], val[64];
	uint64_t addr;

	snprintf(path, sizeof(path), "%s/%s/resource", ND_BUS_PATH, region);
	if (sysfs_read(path, val, sizeof(val)) < 0)
		return 0;
	if (sscanf(val, "%" SCNx64, &addr) != 1 &&
	    sscanf(val, "0x%" SCNx64, &addr) != 1)
		return 0;
	return addr;
}

/*
 * Read the physical frame number for the virtual page containing @vaddr
 * from /proc/self/pagemap.  The caller must ensure the page is present
 * (e.g. by touching it) before calling this function.
 *
 * Returns the PFN (>0) or 0 on error.
 */
static uint64_t vaddr_to_pfn(uintptr_t vaddr)
{
	int fd;
	off_t off;
	uint64_t entry = 0;
	ssize_t n;

	fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd < 0)
		return 0;

	off = (off_t)((vaddr / (uintptr_t)page_size) * sizeof(uint64_t));
	if (lseek(fd, off, SEEK_SET) != off) {
		close(fd);
		return 0;
	}
	n = read(fd, &entry, sizeof(entry));
	close(fd);
	if (n != (ssize_t)sizeof(entry))
		return 0;

	/* bit 63: page present; bits 54:0: PFN */
	if (!(entry & (UINT64_C(1) << 63)))
		return 0;

	return entry & ((UINT64_C(1) << 55) - 1);
}

/* Test 1: mmap(MAP_SHARED_VALIDATE|MAP_SYNC) must fail with EOPNOTSUPP */
static void test_map_sync_rejected(const char *devpath)
{
	int fd;
	void *map;

	fd = open(devpath, O_RDWR);
	if (fd < 0) {
		ksft_test_result_skip("open(%s): %s\n", devpath,
				      strerror(errno));
		return;
	}

	/*
	 * MAP_SYNC is only honoured (and its validation enforced) with
	 * MAP_SHARED_VALIDATE — using plain MAP_SHARED silently strips it.
	 * For virtio-pmem, which requires asynchronous (virtqueue) flushes
	 * for persistence, MAP_SYNC must be rejected.
	 */
	map = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
		   MAP_SHARED_VALIDATE | MAP_SYNC, fd, 0);
	close(fd);

	if (map == MAP_FAILED) {
		if (errno == EOPNOTSUPP)
			ksft_test_result_pass(
				"MAP_SYNC correctly rejected (EOPNOTSUPP) on %s\n",
				devpath);
		else
			ksft_test_result_fail(
				"MAP_SYNC mmap failed with unexpected error %d (%s) on %s\n",
				errno, strerror(errno), devpath);
	} else {
		munmap(map, (size_t)page_size);
		ksft_test_result_fail(
			"MAP_SYNC mmap unexpectedly succeeded on %s\n",
			devpath);
	}
}

/* Test 2: write a pattern through a DAX mmap and read it back */
static void test_dax_read_write(const char *devpath)
{
	int fd;
	unsigned char *map;
	size_t i;
	int pass = 1;
	size_t map_size = (size_t)page_size;

	fd = open(devpath, O_RDWR);
	if (fd < 0) {
		ksft_test_result_skip("open(%s): %s\n", devpath,
				      strerror(errno));
		return;
	}

	map = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		ksft_test_result_skip("mmap(%s): %s\n", devpath,
				      strerror(errno));
		close(fd);
		return;
	}

	memset(map, PATTERN, map_size);

	for (i = 0; i < map_size; i++) {
		if (map[i] != PATTERN) {
			ksft_print_msg(
				"  mismatch at byte %zu: got 0x%02x, expected 0x%02x\n",
				i, map[i], PATTERN);
			pass = 0;
			break;
		}
	}

	if (pass)
		ksft_test_result_pass("dax read/write on %s\n", devpath);
	else
		ksft_test_result_fail("dax read/write on %s\n", devpath);

	munmap(map, map_size);
	close(fd);
}

/* Test 3: zero a page via memset and verify it reads back as zero */
static void test_dax_zero_page(const char *devpath)
{
	int fd;
	unsigned char *map;
	size_t i;
	int pass = 1;
	size_t map_size = 2 * (size_t)page_size;

	fd = open(devpath, O_RDWR);
	if (fd < 0) {
		ksft_test_result_skip("open(%s): %s\n", devpath,
				      strerror(errno));
		return;
	}

	map = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		ksft_test_result_skip("mmap(%s): %s\n", devpath,
				      strerror(errno));
		close(fd);
		return;
	}

	/* Fill second page with non-zero data, then zero it */
	memset(map + page_size, 0xFF, (size_t)page_size);
	memset(map + page_size, 0x00, (size_t)page_size);

	for (i = (size_t)page_size; i < map_size; i++) {
		if (map[i] != 0) {
			ksft_print_msg(
				"  non-zero at offset %zu after zero: 0x%02x\n",
				i, map[i]);
			pass = 0;
			break;
		}
	}

	if (pass)
		ksft_test_result_pass("zero_page_range on %s\n", devpath);
	else
		ksft_test_result_fail("zero_page_range on %s\n", devpath);

	munmap(map, map_size);
	close(fd);
}

/*
 * Test 4: verify that the physical address visible in /proc/self/pagemap
 * matches the physical base address reported in the ND region's "resource"
 * sysfs attribute.
 */
static void test_phys_addr_matches(const char *devpath, const char *region)
{
	int fd;
	unsigned char *map;
	uint64_t phys_base, pfn, phys_from_pagemap;

	phys_base = read_region_phys_base(region);
	if (!phys_base) {
		ksft_test_result_skip(
			"could not read %s/%s/resource\n", ND_BUS_PATH, region);
		return;
	}

	fd = open(devpath, O_RDWR);
	if (fd < 0) {
		ksft_test_result_skip("open(%s): %s\n", devpath,
				      strerror(errno));
		return;
	}

	map = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
		   MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		ksft_test_result_skip("mmap(%s): %s\n", devpath,
				      strerror(errno));
		close(fd);
		return;
	}

	/* Touch the page to ensure it is present in the page table */
	map[0] = 0;

	pfn = vaddr_to_pfn((uintptr_t)map);
	if (!pfn) {
		ksft_test_result_skip(
			"pagemap read failed for %s (running as root?)\n",
			devpath);
		munmap(map, (size_t)page_size);
		close(fd);
		return;
	}

	phys_from_pagemap = pfn * (uint64_t)page_size;

	ksft_print_msg(
		"  region phys_base=0x%016" PRIx64
		" pagemap phys=0x%016" PRIx64 "\n",
		phys_base, phys_from_pagemap);

	/*
	 * The first page of the DAX mapping must start at the physical
	 * base address of the region.
	 */
	if (phys_from_pagemap == phys_base)
		ksft_test_result_pass(
			"physical address from pagemap matches region resource on %s\n",
			devpath);
	else
		ksft_test_result_fail(
			"physical address mismatch on %s: pagemap=0x%016" PRIx64
			" region=0x%016" PRIx64 "\n",
			devpath, phys_from_pagemap, phys_base);

	munmap(map, (size_t)page_size);
	close(fd);
}

int main(void)
{
	char *region, *devpath;

	ksft_print_header();

	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0)
		page_size = PAGE_SIZE_DEFAULT;

	/* Find a virtio-pmem backed NVDIMM region */
	region = find_virtio_pmem_region();
	if (!region) {
		ksft_set_plan(1);
		ksft_test_result_skip(
			"no virtio-pmem region found "
			"(is the driver loaded and a device present?)\n");
		ksft_finished();
	}

	ksft_print_msg("found virtio-pmem region: %s\n", region);

	/* Find the DAX device for that region */
	devpath = find_dax_dev_for_region(region);
	if (!devpath) {
		ksft_set_plan(1);
		ksft_test_result_skip(
			"no DAX device found for region %s "
			"(try: ndctl create-namespace -m dax -r %s)\n",
			region, region);
		free(region);
		ksft_finished();
	}

	ksft_print_msg("found DAX device: %s\n", devpath);

	ksft_set_plan(4);
	test_map_sync_rejected(devpath);
	test_dax_read_write(devpath);
	test_dax_zero_page(devpath);
	test_phys_addr_matches(devpath, region);

	free(devpath);
	free(region);
	ksft_finished();
}
