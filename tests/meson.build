# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Gabi Falk

# --- Shared test helpers ---
test_common_o = custom_target('test_common.o',
  output: 'test_common.o',
  input: 'test_common.c',
  command: [user_cc_prog, '-static', '-I', meson.current_source_dir(),
            '-c', '-o', '@OUTPUT@', '@INPUT@'],
)

# --- Build init binaries (each linked with test_common.o) ---
init_programs = {
  'init_cpio': 'init_cpio.c',
  'init_pie': 'init_pie.c',
  'init_ext2': 'init_ext2.c',
  'init_ext2_tcc': 'init_ext2_tcc.c',
  'init_syscall_stress': 'init_syscall_stress.c',
  'init_signal': 'init_signal.c',
  'init_cow': 'init_cow.c',
  'init_mmap_file': 'init_mmap_file.c',
  'init_syscall_fuzz': 'init_syscall_fuzz.c',
  'init_fuzz_expanded': 'init_fuzz_expanded.c',
  'init_contention': 'init_contention.c',
  'init_ext2_stress': 'init_ext2_stress.c',
  'init_exec_edge': 'init_exec_edge.c',
  'init_symlink': 'init_symlink.c',
  'init_iovec': 'init_iovec.c',
  'init_lseek': 'init_lseek.c',
  'init_ext2_smp': 'init_ext2_smp.c',
  'init_mprotect': 'init_mprotect.c',
  'init_getdents': 'init_getdents.c',
  'init_rename': 'init_rename.c',
  'init_brk': 'init_brk.c',
  'init_fork_stress': 'init_fork_stress.c',
  'init_chdir': 'init_chdir.c',
  'init_hardlink': 'init_hardlink.c',
  'init_ftruncate': 'init_ftruncate.c',
  'init_stat': 'init_stat.c',
  'init_sigchld': 'init_sigchld.c',
  'init_large_file': 'init_large_file.c',
  'init_rlimit': 'init_rlimit.c',
  'init_sigrestart': 'init_sigrestart.c',
  'init_umask': 'init_umask.c',
  'init_poll': 'init_poll.c',
  'init_cowfork': 'init_cowfork.c',
  'init_select': 'init_select.c',
  'init_vfork': 'init_vfork.c',
  'init_sigmask': 'init_sigmask.c',
  'init_mmap_fork': 'init_mmap_fork.c',
  'init_exec_env': 'init_exec_env.c',
  'init_mp_ext2': 'init_mp_ext2.c',
  'init_sigfork': 'init_sigfork.c',
  'init_deep_exec': 'init_deep_exec.c',
  'init_tty_ioctl': 'init_tty_ioctl.c',
  'init_pread_pwrite': 'init_pread_pwrite.c',
  'init_sigstop': 'init_sigstop.c',
  'init_append': 'init_append.c',
  'init_mkdir_rmdir': 'init_mkdir_rmdir.c',
  'init_boot': 'init_boot.c',
  'init_cow_signal': 'init_cow_signal.c',
  'init_vfs_edge': 'init_vfs_edge.c',
  'init_deep_path': 'init_deep_path.c',
  'init_ext2_concurrent': 'init_ext2_concurrent.c',
  'init_sched': 'init_sched.c',
  'init_mm_stress': 'init_mm_stress.c',
  'init_workload': 'init_workload.c',
  'init_signal_adv': 'init_signal_adv.c',
  'init_identity': 'init_identity.c',
  'init_exec_adv': 'init_exec_adv.c',
  'init_io_misc': 'init_io_misc.c',
  'init_mm_adv': 'init_mm_adv.c',
  'init_stress_cache': 'init_stress_cache.c',
  'init_stub_canary': 'init_stub_canary.c',
  'init_signal_cross': 'init_signal_cross.c',
  'init_sendfile': 'init_sendfile.c',
  'init_mmap_cross': 'init_mmap_cross.c',
  'init_negative_args': 'init_negative_args.c',
  'init_readlink_proc': 'init_readlink_proc.c',
  'init_fd_exec': 'init_fd_exec.c',
  'init_fd_ops': 'init_fd_ops.c',
  'init_pipe_all': 'init_pipe_all.c',
  'init_mmap_all': 'init_mmap_all.c',
  'init_proc_ops': 'init_proc_ops.c',
  'init_kexec': 'init_kexec.c',
  'init_memfd': 'init_memfd.c',
  'init_kexec_memfd': 'init_kexec_memfd.c',
  'init_mprotect_cow': 'init_mprotect_cow.c',
  'init_signal_preempt': 'init_signal_preempt.c',
  'init_smp_fork': 'init_smp_fork.c',
  'init_smp_cow': 'init_smp_cow.c',
}

init_bins = {}
foreach name, src : init_programs
  obj = custom_target(name + '.o',
    output: name + '.o',
    input: src,
    command: [user_cc_prog, '-static', '-I', meson.current_source_dir(),
              '-c', '-o', '@OUTPUT@', '@INPUT@'],
  )
  init_bins += {name: custom_target(name,
    output: name,
    input: [obj, test_common_o],
    command: [user_cc_prog, '-static', '-o', '@OUTPUT@', '@INPUT@'],
  )}
endforeach

# --- Standalone user programs ---
hello = custom_target('hello',
  output: 'hello',
  input: 'hello.c',
  command: [user_cc_prog, '-static', '-o', '@OUTPUT@', '@INPUT@'],
)

fd_checker = custom_target('fd_checker',
  output: 'fd_checker',
  input: 'fd_checker.c',
  command: [user_cc_prog, '-static', '-o', '@OUTPUT@', '@INPUT@'],
)

hello_pie = custom_target('hello_pie',
  output: 'hello_pie',
  input: 'hello_pie.c',
  command: [user_cc_prog, '-static-pie', '-o', '@OUTPUT@', '@INPUT@'],
)

# --- Assembly test programs ---
foreach name : ['test_ud', 'test_pf', 'test_int3']
  asm_obj = custom_target(name + '_obj',
    output: name + '.o',
    input: name + '.S',
    command: [cc_prog, '-m64', '-ffreestanding', '-nostdlib', '-fno-builtin',
              '-fno-stack-protector', '-fno-pic', '-c', '@INPUT@', '-o', '@OUTPUT@'],
  )
  custom_target(name,
    output: name,
    input: asm_obj,
    depends: [ember_ld],
    command: [ember_ld, '-e', '_start', '--base=0x400000',
              '-o', '@OUTPUT@', '@INPUT@'],
  )
endforeach

# --- Freestanding W^X / mprotect test (native cc, own _start, no libc) ---
# Real read-execute .text and raw syscalls -- the W^X-negative and mprotect
# paths the monolithic-RWX M2 binaries and libc inits cannot express.
init_wx_mprotect = custom_target('init_wx_mprotect',
  output: 'init_wx_mprotect',
  input: 'init_wx_mprotect.c',
  command: [cc_prog, '-static', '-no-pie', '-nostdlib', '-ffreestanding',
            '-fno-stack-protector', '-march=x86-64', '-mno-avx',
            '-Wl,-z,separate-code', '-o', '@OUTPUT@', '@INPUT@'],
)

# --- Freestanding IRQ self-test init (own _start, no libc) ---
init_irqtest = custom_target('init_irqtest',
  output: 'init_irqtest',
  input: 'init_irqtest.c',
  command: [cc_prog, '-static', '-no-pie', '-nostdlib', '-ffreestanding',
            '-fno-stack-protector', '-march=x86-64', '-mno-avx',
            '-Wl,-z,separate-code', '-o', '@OUTPUT@', '@INPUT@'],
)

# --- Ext2 images ---
mkext2 = find_program('../scripts/mkext2img.sh')
ext2_img = custom_target('ext2.img',
  output: 'ext2.img',
  command: [mkext2, '@OUTPUT@'],
)

static_tcc_path = get_option('static_tcc')
if static_tcc_path != ''
  mkext2_tcc = find_program('../scripts/mkext2img-tcc.sh')
  ext2_tcc_img = custom_target('ext2-tcc.img',
    output: 'ext2-tcc.img',
    input: 'hello_tcc.c',
    command: [mkext2_tcc, '@OUTPUT@', static_tcc_path, '@INPUT@'],
  )
endif


# --- Per-test ESP image builder and QEMU runner ---
build_test = find_program('../scripts/build_test.sh')
build_test_ext2efi = find_program('../scripts/build_test_ext2efi.sh')
run_test = find_program('../scripts/run_test.sh')

# Common depends/args for build_test
_bt_deps = [kernel_archives, elf2efi, ember_ld]
_bt_base = [build_test, cc_prog, ember_ld, srcdir,
            kernel_archives[0], kernel_archives[1],
            elf2efi]

# --- Core tests ---
_cpio_extra_args = [hello[0].full_path() + ':hello']
_cpio_extras = [hello]

# -- cpio --
_name = 'esp-cpio'
_esp = custom_target(_name,
  output: _name + '.img',
  depends: _bt_deps + [init_bins['init_cpio']] + _cpio_extras,
  command: _bt_base + ['@OUTPUT@', init_bins['init_cpio']] + _cpio_extra_args,
)
test('cpio', run_test,
  args: [_esp],
  depends: [_esp],
  timeout: 60,
)

# -- PIE --
_pie_extra_args = [hello[0].full_path() + ':hello',
                   hello_pie[0].full_path() + ':hello_pie']
_pie_extras = [hello, hello_pie]

_name = 'esp-pie'
_esp = custom_target(_name,
  output: _name + '.img',
  depends: _bt_deps + [init_bins['init_pie']] + _pie_extras,
  command: _bt_base + ['@OUTPUT@', init_bins['init_pie']] + _pie_extra_args,
)
test('pie', run_test,
  args: [_esp],
  depends: [_esp],
  timeout: 60,
)

# -- ext2 (AHCI, q35) --
_name = 'esp-ext2'
_esp = custom_target(_name,
  output: _name + '.img',
  depends: _bt_deps + [init_bins['init_ext2']],
  command: _bt_base + ['@OUTPUT@', init_bins['init_ext2']],
)
test('ext2', run_test,
  args: [_esp, '--ext2', ext2_img],
  depends: [_esp, ext2_img],
  timeout: 60,
)

# -- ext2-ide (legacy IDE, pc) --
_name = 'esp-ext2-ide'
_esp = custom_target(_name,
  output: _name + '.img',
  depends: _bt_deps + [init_bins['init_ext2']],
  command: _bt_base + ['@OUTPUT@', init_bins['init_ext2']],
)
test('ext2-ide', run_test,
  args: [_esp, '--ext2', ext2_img, '--machine', 'pc'],
  depends: [_esp, ext2_img],
  timeout: 60,
)

# -- ext2-smp (concurrent ext2 operations) --
_name = 'esp-ext2-smp'
_esp = custom_target(_name,
  output: _name + '.img',
  depends: _bt_deps + [init_bins['init_ext2_smp']],
  command: _bt_base + ['@OUTPUT@', init_bins['init_ext2_smp']],
)
test('ext2-smp', run_test,
  args: [_esp, '--ext2', ext2_img],
  depends: [_esp, ext2_img],
  timeout: 60,
)

# -- ext2-tcc --
if static_tcc_path != ''
  _name = 'esp-ext2-tcc'
  _esp = custom_target(_name,
    output: _name + '.img',
    depends: _bt_deps + [init_bins['init_ext2_tcc']],
    command: _bt_base + ['@OUTPUT@', init_bins['init_ext2_tcc']],
  )
  test('ext2-tcc', run_test,
    args: [_esp, '--ext2', ext2_tcc_img],
    depends: [_esp, ext2_tcc_img],
    timeout: 60,
  )
endif

# --- ext2-efi test (ext2 image loaded by EFI loader from ESP) ---
_bt_ext2efi_base = [build_test_ext2efi, cc_prog, ember_ld, srcdir,
                    kernel_archives[0], kernel_archives[1],
                    elf2efi]

_name = 'esp-ext2-efi'
_esp = custom_target(_name,
  output: _name + '.img',
  depends: _bt_deps + [init_bins['init_ext2']],
  command: _bt_ext2efi_base + ['@OUTPUT@', init_bins['init_ext2']],
)
test('ext2-efi', run_test,
  args: [_esp],
  depends: [_esp],
  timeout: 60,
)

# --- Cpio tests that previously ran at SMP 1/2/4 ---
_smp_cpio_tests = {
  'signal': 'init_signal',
  'cow': 'init_cow',
  'contention': 'init_contention',
  'fork-stress': 'init_fork_stress',
  'cow-signal': 'init_cow_signal',
  'sched': 'init_sched',
}

foreach tname, init_key : _smp_cpio_tests
  _name = 'esp-' + tname
  _esp = custom_target(_name,
    output: _name + '.img',
    depends: _bt_deps + [init_bins[init_key]] + _cpio_extras,
    command: _bt_base + ['@OUTPUT@', init_bins[init_key]] + _cpio_extra_args,
  )
  test(tname, run_test,
    args: [_esp],
    depends: [_esp],
    timeout: 60,
  )
endforeach

# --- Ext2 tests that previously ran at SMP 1/2/4 ---
_smp_ext2_tests = {
  'ext2-concurrent': 'init_ext2_concurrent',
  'stress-cache': 'init_stress_cache',
}

foreach tname, init_key : _smp_ext2_tests
  _name = 'esp-' + tname
  _esp = custom_target(_name,
    output: _name + '.img',
    depends: _bt_deps + [init_bins[init_key]],
    command: _bt_base + ['@OUTPUT@', init_bins[init_key]],
  )
  test(tname, run_test,
    args: [_esp, '--ext2', ext2_img],
    depends: [_esp, ext2_img],
    timeout: 60,
  )
endforeach

# --- SMP-specific cpio tests (4 CPUs) ---
_name = 'esp-smp-fork'
_esp = custom_target(_name,
  output: _name + '.img',
  depends: _bt_deps + [init_bins['init_smp_fork']],
  command: _bt_base + ['@OUTPUT@', init_bins['init_smp_fork']],
)
test('smp-fork', run_test,
  args: [_esp, '--smp', '4'],
  depends: [_esp],
  timeout: 60,
)

_name = 'esp-smp-cow'
_esp = custom_target(_name,
  output: _name + '.img',
  depends: _bt_deps + [init_bins['init_smp_cow']],
  command: _bt_base + ['@OUTPUT@', init_bins['init_smp_cow']],
)
test('smp-cow', run_test,
  args: [_esp, '--smp', '4'],
  depends: [_esp],
  timeout: 60,
)

# --- Simple cpio tests (with cpio extras, SMP 1 only) ---
# test-name : init binary key
_cpio_tests = {
  'syscall-stress': 'init_syscall_stress',
  'mmap-file': 'init_mmap_file',
  'syscall-fuzz': 'init_syscall_fuzz',
  'fuzz-expanded': 'init_fuzz_expanded',
  'exec-edge': 'init_exec_edge',
  'mprotect': 'init_mprotect',
  'sigchld': 'init_sigchld',
  'rlimit': 'init_rlimit',
  'sigrestart': 'init_sigrestart',
  'poll': 'init_poll',
  'cowfork': 'init_cowfork',
  'select': 'init_select',
  'vfork': 'init_vfork',
  'sigmask': 'init_sigmask',
  'exec-env': 'init_exec_env',
  'sigfork': 'init_sigfork',
  'deep-exec': 'init_deep_exec',
  'tty-ioctl': 'init_tty_ioctl',
  'sigstop': 'init_sigstop',
  'workload': 'init_workload',
  'signal-adv': 'init_signal_adv',
  'exec-adv': 'init_exec_adv',
  'io-misc': 'init_io_misc',
  'signal-cross': 'init_signal_cross',
  'pipe-all': 'init_pipe_all',
  'mmap-all': 'init_mmap_all',
  'proc-ops': 'init_proc_ops',
}

foreach tname, init_key : _cpio_tests
  _name = 'esp-' + tname
  _esp = custom_target(_name,
    output: _name + '.img',
    depends: _bt_deps + [init_bins[init_key]] + _cpio_extras,
    command: _bt_base + ['@OUTPUT@', init_bins[init_key]] + _cpio_extra_args,
  )
  test(tname, run_test,
    args: [_esp],
    depends: [_esp],
    timeout: 60,
  )
endforeach

# --- Simple bare cpio tests (no cpio extras, SMP 1 only) ---
_bare_cpio_tests = {
  'brk': 'init_brk',
  'boot': 'init_boot',
  'mm-stress': 'init_mm_stress',
  'mm-adv': 'init_mm_adv',
  'stub-canary': 'init_stub_canary',
  'mmap-cross': 'init_mmap_cross',
  'negative-args': 'init_negative_args',
  'readlink-proc': 'init_readlink_proc',
  'fd-ops': 'init_fd_ops',
  'kexec': 'init_kexec',
  'memfd': 'init_memfd',
  'kexec-memfd': 'init_kexec_memfd',
  'mprotect-cow': 'init_mprotect_cow',
  'signal-preempt': 'init_signal_preempt',
}

foreach tname, init_key : _bare_cpio_tests
  _name = 'esp-' + tname
  _esp = custom_target(_name,
    output: _name + '.img',
    depends: _bt_deps + [init_bins[init_key]],
    command: _bt_base + ['@OUTPUT@', init_bins[init_key]],
  )
  test(tname, run_test,
    args: [_esp],
    depends: [_esp],
    timeout: 60,
  )
endforeach

# --- fd-exec test (needs fd_checker in cpio) ---
_fd_exec_extra_args = [hello[0].full_path() + ':hello',
                       fd_checker[0].full_path() + ':fd_checker']
_fd_exec_extras = [hello, fd_checker]

_name = 'esp-fd-exec'
_esp = custom_target(_name,
  output: _name + '.img',
  depends: _bt_deps + [init_bins['init_fd_exec']] + _fd_exec_extras,
  command: _bt_base + ['@OUTPUT@', init_bins['init_fd_exec']] + _fd_exec_extra_args,
)
test('fd-exec', run_test,
  args: [_esp],
  depends: [_esp],
  timeout: 60,
)

# --- W^X / mprotect freestanding test (bare cpio) ---
_name = 'esp-wx-mprotect'
_esp = custom_target(_name,
  output: _name + '.img',
  depends: _bt_deps + [init_wx_mprotect],
  command: _bt_base + ['@OUTPUT@', init_wx_mprotect],
)
test('wx-mprotect', run_test,
  args: [_esp],
  depends: [_esp],
  timeout: 60,
)

# --- IRQ self-test (boot-time self-IPI exercises dispatch path) ---
_name = 'esp-irq-selftest'
_esp_irq = custom_target(_name,
  output: _name + '.img',
  depends: _bt_deps + [init_irqtest],
  command: _bt_base + ['@OUTPUT@', init_irqtest],
)
test('irq-selftest', run_test,
  args: [_esp_irq],
  depends: [_esp_irq],
  timeout: 60,
)
test('irq-selftest-smp', run_test,
  args: [_esp_irq, '--smp', '2'],
  depends: [_esp_irq],
  timeout: 60,
)

# --- Simple ext2 tests (AHCI, q35, SMP 1 only) ---
# test-name : init binary key
_ext2_tests = {
  'ext2-stress': 'init_ext2_stress',
  'iovec': 'init_iovec',
  'symlink': 'init_symlink',
  'lseek': 'init_lseek',
  'getdents': 'init_getdents',
  'rename': 'init_rename',
  'chdir': 'init_chdir',
  'hardlink': 'init_hardlink',
  'ftruncate': 'init_ftruncate',
  'stat': 'init_stat',
  'large-file': 'init_large_file',
  'umask': 'init_umask',
  'mmap-fork': 'init_mmap_fork',
  'mp-ext2': 'init_mp_ext2',
  'pread-pwrite': 'init_pread_pwrite',
  'append': 'init_append',
  'mkdir-rmdir': 'init_mkdir_rmdir',
  'vfs-edge': 'init_vfs_edge',
  'deep-path': 'init_deep_path',
  'identity': 'init_identity',
  'sendfile': 'init_sendfile',
}

foreach tname, init_key : _ext2_tests
  _name = 'esp-' + tname
  _esp = custom_target(_name,
    output: _name + '.img',
    depends: _bt_deps + [init_bins[init_key]],
    command: _bt_base + ['@OUTPUT@', init_bins[init_key]],
  )
  test(tname, run_test,
    args: [_esp, '--ext2', ext2_img],
    depends: [_esp, ext2_img],
    timeout: 60,
  )
endforeach

# --- M2libc test (optional, requires M2-Planet toolchain) ---
m2_planet_prefix = get_option('m2_planet_prefix')
if m2_planet_prefix != ''
  compile_m2 = find_program('../scripts/compile_m2.sh')

  m2_hello = custom_target('m2_hello',
    output: 'm2_hello',
    input: 'm2_hello.c',
    command: [compile_m2, m2_planet_prefix, '@INPUT@', '@OUTPUT@'],
  )

  init_m2libc_o = custom_target('init_m2libc.o',
    output: 'init_m2libc.o',
    input: 'init_m2libc.c',
    command: [user_cc_prog, '-static', '-I', meson.current_source_dir(),
              '-c', '-o', '@OUTPUT@', '@INPUT@'],
  )
  init_m2libc = custom_target('init_m2libc',
    output: 'init_m2libc',
    input: [init_m2libc_o, test_common_o],
    command: [user_cc_prog, '-static', '-o', '@OUTPUT@', '@INPUT@'],
  )

  _name = 'esp-m2libc'
  _esp = custom_target(_name,
    output: _name + '.img',
    depends: _bt_deps + [init_m2libc, m2_hello],
    command: _bt_base + ['@OUTPUT@', init_m2libc,
              m2_hello[0].full_path() + ':m2_hello'],
  )
  test('m2libc', run_test,
    args: [_esp],
    depends: [_esp],
    timeout: 60,
  )
endif

# --- Stack frame size check ---
check_stack = find_program('../scripts/check_stack_usage.sh')
test('stack-usage', check_stack,
  args: [kernel_archives[1]],
  depends: [kernel_archives],
)

# --- Interactive run targets ---
run_qemu = find_program('../scripts/run_qemu.sh')
run_qemu_fb = find_program('../scripts/run_qemu_fb.sh')

run_target('run',
  command: [run_qemu],
  depends: [bootx64],
  env: {'BUILD_DIR': meson.project_build_root()},
)

run_target('run-fb',
  command: [run_qemu_fb],
  depends: [bootx64],
  env: {'BUILD_DIR': meson.project_build_root()},
)
