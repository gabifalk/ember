/*
 * SPDX-FileCopyrightText: 2026 Gabi Falk
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * make_initrd: Build ext2 initrd in memory and exec Ember kernel.
 *
 * Allocates a memory buffer, formats it as ext2 using lwext4
 * (via memory-backed block device), copies directory trees into it,
 * writes a 24-byte descriptor file, and exec's the Ember kernel .efi.
 *
 * Under posix-runner on UEFI, virtual addresses = physical addresses.
 * The buffer persists across exec because posix-runner doesn't free it.
 *
 * Usage: make_initrd [-o desc] [-k kernel.efi] [-s blocks] path1 [path2 ...].
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <inttypes.h>

#ifndef DT_DIR
#define DT_DIR 4
#define DT_REG 8
#endif

#include <ext4.h>
#include <ext4_mkfs.h>
#include "mem_dev.h"

#define EXT2_BLOCK_SIZE 1024
#define PATH_MAX 4096
#define COPY_BUF_SIZE 4096

#define INITRD_DESC_MAGIC 0x44524E4954494E49ULL	/* "INITINRD". */

typedef struct {
	uint64_t magic;
	uint64_t address;
	uint64_t size;
} initrd_desc_t;

static struct ext4_blockdev *bd;
static struct ext4_fs fs;

static struct ext4_mkfs_info info = {
	.block_size = EXT2_BLOCK_SIZE,
	.journal = 0,
	.inode_size = 128,
};

static int
copy_file(const char *src_path)
{
	ext4_file dest_file;
	char dest_path[PATH_MAX];
	char buf[COPY_BUF_SIZE];
	int err;
	size_t n;
	size_t wcnt;
	FILE *src;

	strcpy(dest_path, "/mp");
	strcat(dest_path, src_path);

	src = fopen(src_path, "r");
	if (!src) {
		printf("make_initrd: cannot open: %s\n", src_path);
		return -1;
	}

	err = ext4_fopen(&dest_file, dest_path, "wb");
	if (err != EOK) {
		printf("make_initrd: ext4_fopen error %d: %s\n", err,
		       dest_path);
		fclose(src);
		return -1;
	}

	while ((n = fread(buf, 1, COPY_BUF_SIZE, src)) > 0) {
		err = ext4_fwrite(&dest_file, buf, n, &wcnt);
		if (err != EOK) {
			printf("make_initrd: ext4_fwrite error %d: %s\n", err,
			       dest_path);
			ext4_fclose(&dest_file);
			fclose(src);
			return -1;
		}
	}

	ext4_fclose(&dest_file);
	fclose(src);
	return 0;
}

static void
copy_recursive(const char *dir_path)
{
	DIR *d;
	struct dirent *ent;
	char child_path[PATH_MAX];
	char ext4_dir[PATH_MAX];

	/* Create the directory in ext2. */
	strcpy(ext4_dir, "/mp");
	strcat(ext4_dir, dir_path);
	ext4_dir_mk(ext4_dir);

	d = opendir(dir_path);
	if (!d) {
		printf("make_initrd: cannot opendir: %s\n", dir_path);
		return;
	}

	while ((ent = readdir(d)) != NULL) {
		if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) {
			continue;
		}

		strcpy(child_path, dir_path);
		if (dir_path[strlen(dir_path) - 1] != '/') {
			strcat(child_path, "/");
		}
		strcat(child_path, ent->d_name);

		if (ent->d_type == DT_DIR) {
			copy_recursive(child_path);
		} else {
			copy_file(child_path);
		}
	}

	closedir(d);
}

static void
copy_file_to_root(const char *host_path)
{
	struct stat st;
	if (stat(host_path, &st) < 0) {
		printf("make_initrd: stat failed (skipping): %s\n", host_path);
		return;
	}

	if (S_ISDIR(st.st_mode)) {
		printf("make_initrd: %s/ (dir)\n", host_path);
		copy_recursive(host_path);
	} else if (S_ISREG(st.st_mode)) {
		printf("make_initrd: %s (%ld bytes)\n", host_path,
		       (long)st.st_size);
		copy_file(host_path);
	}
}

int
main(int argc, char **argv)
{
	const char *desc_path = "/boot/initrd.desc";
	const char *kernel_path = "/EFI/BOOT/BOOTX64.EFI";
	int image_blocks = 1381376;	/* Default ~1.3 GiB. */
	int path_count = 0;
	char *paths[64];
	int err;
	int i;
	uint64_t total_size;
	uint8_t *buf;
	FILE *desc;
	initrd_desc_t d;
	char *new_argv[2];
	char *new_envp[1];

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-o") && i + 1 < argc) {
			desc_path = argv[++i];
		} else if (!strcmp(argv[i], "-k") && i + 1 < argc) {
			kernel_path = argv[++i];
		} else if (!strcmp(argv[i], "-s") && i + 1 < argc) {
			image_blocks = atoi(argv[++i]);
		} else {
			paths[path_count] = argv[i];
			path_count++;
		}
	}

	if (path_count == 0) {
		printf
		    ("Usage: make_initrd [-o desc] [-k kernel.efi] [-s blocks] "
		     "path1 [path2 ...]\n");
		return 1;
	}

	total_size = (uint64_t) image_blocks *EXT2_BLOCK_SIZE;
	printf("make_initrd: %d blocks of %d bytes (%lu MiB)\n",
	       image_blocks, EXT2_BLOCK_SIZE,
	       (unsigned long)(total_size / (1024 * 1024)));

	/* Allocate memory buffer for ext2 image. */
	buf = malloc(total_size);
	if (!buf) {
		printf("make_initrd: cannot allocate %lu bytes\n",
		       (unsigned long)total_size);
		return 1;
	}
	memset(buf, 0, total_size);

	/* Set up memory block device. */
	mem_dev_setup(buf, total_size);
	bd = mem_dev_get();

	/* Format as ext2. */
	err = ext4_mkfs(&fs, bd, &info, F_SET_EXT2_V0);
	if (err != EOK) {
		printf("make_initrd: ext4_mkfs error: %d\n", err);
		return 1;
	}

	/* Mount. */
	err = ext4_device_register(bd, "ext4_fs");
	if (err != EOK) {
		printf("make_initrd: ext4_device_register: %d\n", err);
		return 1;
	}

	err = ext4_mount("ext4_fs", "/mp/", 0);
	if (err != EOK) {
		printf("make_initrd: ext4_mount: %d\n", err);
		return 1;
	}

	err = ext4_recover("/mp/");
	if (err != EOK && err != ENOTSUP) {
		printf("make_initrd: ext4_recover: %d\n", err);
		return 1;
	}

	printf("make_initrd: ext2 mounted, copying files\n");

	/* Create basic directories. */
	ext4_dir_mk("/mp/dev");
	ext4_dir_mk("/mp/tmp");

	/* Copy each specified path. */
	for (i = 0; i < path_count; i++) {
		copy_file_to_root(paths[i]);
	}

	/* Unmount (flushes all metadata) */
	err = ext4_umount("/mp/");
	if (err != EOK) {
		printf("make_initrd: ext4_umount: %d\n", err);
		return 1;
	}

	printf("make_initrd: ext2 image complete\n");

	/* Write descriptor. */
	printf("make_initrd: descriptor -> %s\n", desc_path);
	printf("make_initrd: buffer at 0x%lx, size %lu\n",
	       (unsigned long)(uintptr_t) buf, (unsigned long)total_size);

	desc = fopen(desc_path, "w");
	if (!desc) {
		printf("make_initrd: cannot create %s\n", desc_path);
		return 1;
	}
	d.magic = INITRD_DESC_MAGIC;
	d.address = (uint64_t) (uintptr_t) buf;
	d.size = total_size;
	fwrite(&d, sizeof(d), 1, desc);
	fclose(desc);

	fflush(stdout);

	/*
	 * Exec kernel - the ext2 buffer persists because execve of PE32+
	 * never returns (Ember calls ExitBootServices).
	 */
	printf("make_initrd: exec %s\n", kernel_path);
	fflush(stdout);
	new_argv[0] = "BOOTX64.EFI";
	new_argv[1] = NULL;
	new_envp[0] = NULL;
	execve(kernel_path, new_argv, new_envp);

	printf("make_initrd: execve failed\n");
	return 1;
}
