/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright (C) 2026 Gabi Falk */
#ifndef EMBER_SIGNAL_H
#define EMBER_SIGNAL_H

#include <stdint.h>

#define NSIG          32

#define SIGHUP         1
#define SIGINT         2
#define SIGQUIT        3
#define SIGILL         4
#define SIGABRT        6
#define SIGBUS         7
#define SIGFPE         8
#define SIGKILL        9
#define SIGUSR1       10
#define SIGUSR2       12
#define SIGPIPE       13
#define SIGALRM       14
#define SIGTERM       15
#define SIGCHLD       17
#define SIGCONT       18
#define SIGSTOP       19
#define SIGTSTP       20
#define SIGTTIN       21
#define SIGTTOU       22
#define SIGSEGV       11

#define SIG_DFL  ((uint64_t)0)
#define SIG_IGN  ((uint64_t)1)

/* sa_flags. */
#define SA_SIGINFO    0x4
#define SA_RESTORER   0x04000000
#define SA_RESTART    0x10000000

/* Sigprocmask how. */
#define SIG_BLOCK     0
#define SIG_UNBLOCK   1
#define SIG_SETMASK   2

#endif
