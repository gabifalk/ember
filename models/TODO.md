# Unmodeled kernel issues

Identified 2026-04-15. Each needs a Promela model before fixing in C.

## Fixed

- **4d. MAP_FIXED TLB race** → `mmap_fixed_tlb.pml` + fix in `syscall_mm.c`
- **5c. Exec PML4 leak** → `exec_pml4_leak.pml` + fix in `syscall_proc_exec.c`
- **1c. Clone PT page leak** → `partial_alloc_rollback.pml` + fix in `paging.c`
- **1d. mmap partial alloc leak** → `partial_alloc_rollback.pml` + fix in `syscall_mm.c`
- **1e. mremap move partial leak** → `partial_alloc_rollback.pml` + fix in `syscall_mm.c`
- **1f. mremap grow partial leak** → `partial_alloc_rollback.pml` + fix in `syscall_mm.c`
- **2c. Bucket cache fragmentation** → `bucket_drain.pml` + fix in `heap.c` (BUCKET_CAP=32)

## Lower priority — bounded by MAX_PROCS=256 or BKL

### 1g. Shebang strs buffer leaked per exec
- **File:** `kernel/syscall_proc_exec.c:211`
- **Problem:** `shebang_strs` is allocated for each shebang exec but never
  freed. Small leak (interp + opt + script path lengths) per shebang exec.
- **Existing model:** None.

### 2b. Pipe wake_chan wraps to 0, collides with wait4
- **File:** `kernel/pipe.c:10,25`
- **Problem:** `next_pipe_chan` is a static int that increments on every
  `pipe_create()`. After ~2^31 pipes, wraps to 0, colliding with wait4's
  `wait_chan = 0` → spurious wakeups.
- **Existing model:** `pipe.pml` covers pipe sleep/wakeup but not channel
  collision.

### 3b. O(n²) scan under sched_lock in do_exit
- **File:** `kernel/syscall_proc_exit.c:72-88`
- **Problem:** When reaping orphaned zombies, nested loop scans all MAX_PROCS
  to check PML4 sharing, under `sched_lock` with IRQs disabled. O(n²) worst
  case blocks all other CPUs.
- **Existing model:** `zombie_reap_race.pml` covers reap correctness but not
  the PML4 scan latency.

### 3e. Cpio getdents O(n) scan under vfs_lock
- **File:** `kernel/fs/vfs.c:969-1057`
- **Problem:** For cpio-backed directories, `vfs_getdents` walks the entire
  VFS node list under `vfs_lock`. All other VFS operations blocked for
  O(total_nodes).
- **Existing model:** None.

### 4a. vfs_create TOCTOU (safe under BKL)
- **File:** `kernel/fs/vfs.c:269-318`
- **Problem:** Existence check under `vfs_lock`, then lock dropped, then
  `ext2_create()` without lock, then lock reacquired. Two concurrent
  `open(O_CREAT|O_EXCL)` could both pass the check. Safe under BKL.
- **Existing model:** None (BKL covers this per unified_smp.pml P5).

### 4b. vfs_read/vfs_write uses node pointer after dropping vfs_lock (safe under BKL)
- **File:** `kernel/fs/vfs.c:106-124, 169-185`
- **Problem:** Node pointer obtained before dropping vfs_lock; after
  reacquiring, the node could have been evicted+freed. Safe under BKL.
- **Existing model:** `vfs_node_lifecycle.pml` covers evict+unref but not
  the read/write lock-drop pattern.
