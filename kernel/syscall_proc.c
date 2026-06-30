/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#include "syscall_helpers.h"

uint64_t
syscall_handle_proc(syscall_frame_t * f)
{
	switch (f->rax) {

	case SYS_FORK:
		return do_fork(f);

	case SYS_VFORK:
		return do_vfork(f);

	case SYS_CLONE:{
			uint64_t clone_flags = f->rdi;
			if (!(clone_flags & CLONE_VM))
				return do_fork(f);
			return do_clone_thread(f, clone_flags);
		}

	case SYS_EXECVE:
		return do_execve(f);

	case SYS_EXECVEAT:
		return do_execveat(f);

	case SYS_EXIT:
	case SYS_EXIT_GROUP:{
			int code = (int)f->rdi;
			do_exit(f, code);
			for (;;)
				__asm__ __volatile__("hlt");
			break;
		}

	case SYS_WAIT4:
		return do_wait4(f);

	case SYS_WAITID:
		return do_waitid(f);

	case SYS_PIPE:
		f->rsi = 0;
		return do_pipe2(f);

	case SYS_PIPE2:
		return do_pipe2(f);

	case SYS_DUP2:
		return do_dup2(f);

	case SYS_DUP3:
		return do_dup3(f);

	case SYS_REBOOT:{
			uint32_t magic = (uint32_t) f->rdi;
			uint32_t magic2 = (uint32_t) f->rsi;
			uint32_t cmd = (uint32_t) f->rdx;
			if (magic != LINUX_REBOOT_MAGIC1
			    || magic2 != LINUX_REBOOT_MAGIC2) {
				f->rax = SYSCALL_ERR(EINVAL);
				return f->rax;
			}
			if (cmd == LINUX_REBOOT_CMD_POWER_OFF) {
				console_write("poweroff\n");
				serial_flush();
				qemu_poweroff();
			}
			if (cmd == LINUX_REBOOT_CMD_KEXEC) {
				if (!kexec_is_loaded()) {
					f->rax = SYSCALL_ERR(EINVAL);
					return f->rax;
				}
				console_write("kexec: executing...\n");
				serial_flush();
				kexec_execute();	/* Does not return. */
			}
			f->rax = SYSCALL_ERR(EINVAL);
			return f->rax;
		}

	case SYS_KEXEC_FILE_LOAD:{
			int kernel_fd = (int)f->rdi;
			int initrd_fd = (int)f->rsi;
			uint64_t cmdline_len = f->rdx;
			const char *user_cmdline = (const char *)f->r10;
			uint64_t flags = f->r8;

			/* Only support KEXEC_FILE_NO_INITRD flag. */
			if (flags & ~(uint64_t) KEXEC_FILE_NO_INITRD) {
				f->rax = SYSCALL_ERR(EINVAL);
				return f->rax;
			}

			if (flags & KEXEC_FILE_NO_INITRD)
				initrd_fd = -1;

			/* Copy cmdline from user space. */
			char kcmdline[4096];
			kcmdline[0] = '\0';
			uint64_t kcmdline_len = 0;
			if (user_cmdline && cmdline_len > 0) {
				uint64_t clen =
				    (cmdline_len < 4095) ? cmdline_len : 4095;
				USER_ACCESS_BEGIN();
				uint64_t ci;
				for (ci = 0; ci < clen; ci++)
					kcmdline[ci] = user_cmdline[ci];
				USER_ACCESS_END();
				kcmdline[clen] = '\0';
				kcmdline_len = clen;
			}

			int ret = kexec_load_from_fds(kernel_fd, initrd_fd,
						      kcmdline, kcmdline_len);
			f->rax = (ret == 0) ? 0 : SYSCALL_ERR(EINVAL);
			return f->rax;
		}

	default:
		f->rax = SYSCALL_ERR(ENOSYS);
		return f->rax;
	}
}
