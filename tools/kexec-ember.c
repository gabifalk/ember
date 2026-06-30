/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Gabi Falk
 *
 * kexec-ember: Copy ext2 initrd into place, then exec Ember .efi binary.
 *
 * Under posix-runner, execve of a PE32+ (.efi) file triggers UEFI
 * LoadImage/StartImage. Ember's efi_main loads /boot/ember.ext2 from the
 * ESP before calling ExitBootServices.
 *
 * Usage: kexec-ember [-i initrd] [-k kernel.efi].
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUF_SIZE 4096

static int
copy_file(const char *src, const char *dst)
{
	FILE *in = fopen(src, "r");
	if (!in) {
		fprintf(stderr, "kexec-ember: cannot open %s\n", src);
		return -1;
	}
	FILE *out = fopen(dst, "w");
	if (!out) {
		fprintf(stderr, "kexec-ember: cannot create %s\n", dst);
		fclose(in);
		return -1;
	}
	char buf[BUF_SIZE];
	size_t n;
	while ((n = fread(buf, 1, BUF_SIZE, in)) > 0) {
		fwrite(buf, 1, n, out);
	}
	fclose(in);
	fclose(out);
	return 0;
}

int
main(int argc, char **argv)
{
	const char *initrd = "/boot/ember.ext2";
	const char *kernel = "/EFI/BOOT/BOOTX64.EFI";
	const char *initrd_src = NULL;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-i") && i + 1 < argc) {
			initrd_src = argv[++i];
		} else if (!strcmp(argv[i], "-k") && i + 1 < argc) {
			kernel = argv[++i];
		} else {
			fprintf(stderr,
				"Usage: kexec-ember [-i initrd] [-k kernel.efi]\n");
			return 1;
		}
	}

	if (initrd_src) {
		printf("kexec-ember: copying %s -> %s\n", initrd_src, initrd);
		if (copy_file(initrd_src, initrd) < 0) {
			return 1;
		}
	}

	printf("kexec-ember: exec %s\n", kernel);
	char *new_argv[] = { "BOOTX64.EFI", NULL };
	char *new_envp[] = { NULL };
	execve(kernel, new_argv, new_envp);

	/* Execve failed. */
	fprintf(stderr, "kexec-ember: execve failed\n");
	return 1;
}
