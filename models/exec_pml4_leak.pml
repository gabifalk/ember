/*
 * Exec PML4 leak model for ember.
 *
 * Models do_execve_inner: after paging_create_user_pml4() allocates a
 * new PML4, two subsequent steps can fail:
 *   1. elf_load_user() — ELF parsing/mapping failure
 *   2. setup_user_stack_argv() — stack allocation failure
 *
 * Current code (BUGGY): both error paths return -ENOMEM/-ENOEXEC
 * without calling paging_free_user_pml4(), leaking the PML4 and all
 * user pages already mapped into it.
 *
 * Proposed fix: call paging_free_user_pml4(new_pml4) on both failure
 * paths.
 *
 * Properties:
 *   P1: no PML4 leak (pml4_allocated implies pml4_freed on all paths)
 *   P2: no double-free of PML4
 *   P3: no use-after-free (old PML4 not freed if exec fails)
 *
 * Verify:
 *   spin -a models/exec_pml4_leak.pml && \
 *   gcc -O2 -o pan pan.c && ./pan -m200000
 *
 * Bug injection (current code — missing free):
 *   spin -a -DBUGGY_LEAK models/exec_pml4_leak.pml && \
 *   gcc -O2 -DBUGGY_LEAK -o pan pan.c && ./pan -m200000
 */

bool old_pml4_alive = true;   /* Process's current PML4. */
bool new_pml4_alive = false;  /* PML4 created for exec. */
bool new_pml4_freed = false;
bool old_pml4_freed = false;
bool double_free = false;

bool exec_succeeded = false;
byte done = 0;

/*
 * do_execve_inner: try to exec.
 * Non-deterministically models elf_load and stack setup success/failure.
 */
proctype do_execve() {
    /* Step 1: allocate new PML4. */
    new_pml4_alive = true;

    /* Step 2: elf_load_user — can fail. */
    if
    :: true ->
        /* elf_load succeeded. */

        /* Step 3: setup_user_stack_argv — can fail. */
        if
        :: true ->
            /* Stack setup succeeded — exec succeeds. */
            /* Switch: free old PML4, adopt new one. */
            if
            :: old_pml4_freed -> double_free = true
            :: else -> old_pml4_freed = true; old_pml4_alive = false
            fi;
            exec_succeeded = true

        :: true ->
            /* Stack setup failed — must free new PML4. */
#ifndef BUGGY_LEAK
            if
            :: new_pml4_freed -> double_free = true
            :: else -> new_pml4_freed = true; new_pml4_alive = false
            fi
#endif
        fi

    :: true ->
        /* elf_load failed — must free new PML4. */
#ifndef BUGGY_LEAK
        if
        :: new_pml4_freed -> double_free = true
        :: else -> new_pml4_freed = true; new_pml4_alive = false
        fi
#endif
    fi;

    done = 1
}

/*
 * Monitor: after exec completes, check invariants.
 */
proctype monitor() {
    (done == 1);

    /* P1: no leak — exactly one of the two PML4s must be alive. */
    assert(old_pml4_alive || new_pml4_alive);
    assert(!(old_pml4_alive && new_pml4_alive));

    /* If exec failed, old PML4 must still be alive, new must be freed. */
    if
    :: !exec_succeeded ->
        assert(old_pml4_alive);
        assert(!new_pml4_alive);
        assert(new_pml4_freed)
    :: exec_succeeded ->
        assert(!old_pml4_alive);
        assert(new_pml4_alive);
        assert(old_pml4_freed)
    fi;

    /* P2: no double-free. */
    assert(!double_free)
}

init {
    old_pml4_alive = true;
    new_pml4_alive = false;
    new_pml4_freed = false;
    old_pml4_freed = false;

    run do_execve();
    run monitor()
}
