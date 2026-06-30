/* SPDX-License-Identifier: MIT */
/* kexec-linux: load kernel + initramfs via kexec_file_load, then reboot */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/reboot.h>

/* memfd_create may not be in musl headers for older configs */
static int ember_memfd_create(const char *name, unsigned int flags) {
    return (int)syscall(319, name, flags);
}

/* kexec_file_load(kernel_fd, initrd_fd, cmdline_len, cmdline, flags) */
static int ember_kexec_file_load(int kernel_fd, int initrd_fd) {
    return (int)syscall(320, kernel_fd, initrd_fd, 0, NULL, 0);
}

static int stream_to_memfd(FILE *src, const char *label) {
    int fd = ember_memfd_create(label, 0);
    if (fd < 0) {
        perror("memfd_create");
        return -1;
    }

    char buf[BUFSIZ];
    size_t n;
    while ((n = fread(buf, 1, BUFSIZ, src)) > 0) {
        char *p = buf;
        size_t remaining = n;
        while (remaining > 0) {
            ssize_t nw = write(fd, p, remaining);
            if (nw <= 0) {
                perror("write memfd");
                close(fd);
                return -1;
            }
            p += nw;
            remaining -= (size_t)nw;
        }
    }

    if (lseek(fd, 0, SEEK_SET) < 0) {
        perror("lseek memfd");
        close(fd);
        return -1;
    }

    return fd;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: kexec-linux <kernel> <initramfs>\n"
                        "  initramfs can be a file path or !command\n");
        return 1;
    }

    const char *kernel_path = argv[1];
    const char *initramfs_arg = argv[2];

    /* Open kernel */
    int kernel_fd = open(kernel_path, O_RDONLY);
    if (kernel_fd < 0) {
        perror(kernel_path);
        return 1;
    }

    /* Open/generate initramfs */
    FILE *src;
    int is_popen = 0;
    if (initramfs_arg[0] == '!') {
        src = popen(initramfs_arg + 1, "r");
        is_popen = 1;
    } else {
        src = fopen(initramfs_arg, "rb");
    }
    if (!src) {
        fprintf(stderr, "Cannot open initramfs '%s'\n", initramfs_arg);
        close(kernel_fd);
        return 1;
    }

    int initrd_fd = stream_to_memfd(src, "initrd");
    if (is_popen)
        pclose(src);
    else
        fclose(src);

    if (initrd_fd < 0) {
        close(kernel_fd);
        return 1;
    }

    /* Load */
    if (ember_kexec_file_load(kernel_fd, initrd_fd) < 0) {
        perror("kexec_file_load");
        close(kernel_fd);
        close(initrd_fd);
        return 1;
    }

    close(kernel_fd);
    close(initrd_fd);

    /* Execute */
    sync();
    syscall(SYS_reboot, 0xfee1dead, 672274793, 0x45584543, NULL);

    /* Should not reach here */
    perror("reboot");
    return 1;
}
