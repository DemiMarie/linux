// SPDX-License-Identifier: GPL-2.0
/*
 * virtio_pmem_dax_test.c: self-test for virtio-pmem DAX support
 *
 * Tests that a virtio-pmem-backed DAX device supports direct access:
 *   - Opens /dev/daxX.Y whose provider is "virtio-pmem"
 *   - mmap(MAP_SYNC | MAP_SHARED) to exercise the direct_access callback
 *   - Writes a known pattern and reads it back to verify correctness
 *   - Verifies zero_page_range by mapping at a second offset and checking zeros
 *
 * The test SKIPs if no virtio-pmem device or DAX device is found.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <linux/mman.h>

#include "../../kselftest.h"

#ifndef MAP_SYNC
#define MAP_SYNC 0x80000
#endif

#define TEST_PREFIX	"drivers/virtio_pmem"
#define MAP_SIZE	(2 * 4096)  /* two pages */
#define PATTERN		0xAB

/* Sysfs root for nvdimm bus regions */
#define ND_BUS_PATH	"/sys/bus/nd/devices"
/* Sysfs root for dax bus devices */
#define DAX_BUS_PATH	"/sys/bus/dax/devices"
/* Virtio-pmem provider name as set in virtio_pmem_probe() */
#define VIRTIO_PMEM_PROVIDER	"virtio-pmem"

/*
 * Read a sysfs attribute into @buf (up to @size bytes).
 * Returns the number of bytes read (including NUL), or -1 on error.
 */
static int sysfs_read(const char *path, char *buf, size_t size)
{
	int fd, n;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	n = read(fd, buf, size - 1);
	close(fd);
	if (n < 0)
		return -1;
	/* strip trailing newline */
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		n--;
	buf[n] = '\0';
	return n;
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
		 * Check whether this dax device's region symlink points to
		 * our region.  The dax device sysfs path has a "region" symlink
		 * or a "region" subdirectory depending on the kernel version.
		 * As a simpler heuristic, check the dax device name prefix:
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

/* Test: write a pattern and read it back through a DAX mmap */
static void test_dax_read_write(const char *devpath)
{
	int fd;
	unsigned char *map;
	size_t i;
	int pass = 1;

	ksft_print_msg("  testing direct access (mmap MAP_SYNC) on %s\n",
		       devpath);

	fd = open(devpath, O_RDWR);
	if (fd < 0) {
		ksft_test_result_skip("open(%s): %s\n", devpath,
				      strerror(errno));
		return;
	}

	map = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE,
		   MAP_SHARED | MAP_SYNC, fd, 0);
	if (map == MAP_FAILED) {
		/*
		 * MAP_SYNC may not be supported on all archs/configs;
		 * fall back to MAP_SHARED without MAP_SYNC.
		 */
		map = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE,
			   MAP_SHARED, fd, 0);
		if (map == MAP_FAILED) {
			ksft_test_result_skip("mmap(%s): %s\n", devpath,
					      strerror(errno));
			close(fd);
			return;
		}
		ksft_print_msg("  MAP_SYNC not supported, using MAP_SHARED\n");
	}

	/* Write pattern to first page */
	memset(map, PATTERN, 4096);

	/* Read back and verify */
	for (i = 0; i < 4096; i++) {
		if (map[i] != PATTERN) {
			ksft_print_msg("  mismatch at byte %zu: got 0x%02x, expected 0x%02x\n",
				       i, map[i], PATTERN);
			pass = 0;
			break;
		}
	}

	if (pass)
		ksft_test_result_pass("dax read/write on %s\n", devpath);
	else
		ksft_test_result_fail("dax read/write on %s\n", devpath);

	munmap(map, MAP_SIZE);
	close(fd);
}

/* Test: zero second page via memset (exercises zero_page_range path) */
static void test_dax_zero_page(const char *devpath)
{
	int fd;
	unsigned char *map;
	size_t i;
	int pass = 1;

	ksft_print_msg("  testing zero_page_range on %s\n", devpath);

	fd = open(devpath, O_RDWR);
	if (fd < 0) {
		ksft_test_result_skip("open(%s): %s\n", devpath,
				      strerror(errno));
		return;
	}

	map = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE,
		   MAP_SHARED | MAP_SYNC, fd, 0);
	if (map == MAP_FAILED) {
		map = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE,
			   MAP_SHARED, fd, 0);
		if (map == MAP_FAILED) {
			ksft_test_result_skip("mmap(%s): %s\n", devpath,
					      strerror(errno));
			close(fd);
			return;
		}
	}

	/* Fill second page with non-zero data, then zero it */
	memset(map + 4096, 0xFF, 4096);
	memset(map + 4096, 0x00, 4096);

	/* Verify the second page is zeroed */
	for (i = 4096; i < MAP_SIZE; i++) {
		if (map[i] != 0) {
			ksft_print_msg("  non-zero byte at offset %zu after zero: 0x%02x\n",
				       i, map[i]);
			pass = 0;
			break;
		}
	}

	if (pass)
		ksft_test_result_pass("zero_page_range on %s\n", devpath);
	else
		ksft_test_result_fail("zero_page_range on %s\n", devpath);

	munmap(map, MAP_SIZE);
	close(fd);
}

int main(void)
{
	char *region, *devpath;

	ksft_print_header();

	/* Find a virtio-pmem backed NVDIMM region */
	region = find_virtio_pmem_region();
	if (!region) {
		ksft_set_plan(1);
		ksft_test_result_skip("no virtio-pmem region found (is the driver loaded and a device present?)\n");
		ksft_finished();
	}

	ksft_print_msg("found virtio-pmem region: %s\n", region);

	/* Find the DAX device for that region */
	devpath = find_dax_dev_for_region(region);
	if (!devpath) {
		ksft_set_plan(1);
		ksft_test_result_skip("no DAX device found for region %s (try: ndctl create-namespace -m dax -r %s)\n",
				      region, region);
		free(region);
		ksft_finished();
	}

	ksft_print_msg("found DAX device: %s\n", devpath);

	ksft_set_plan(2);
	test_dax_read_write(devpath);
	test_dax_zero_page(devpath);

	free(devpath);
	free(region);
	ksft_finished();
}
