/*
 * iretq_stack.pml -- Verify that the iretq return path in
 * syscall_entry.S never writes outside the kernel stack.
 *
 * The syscall frame (152 bytes = 19 * 8) sits at the top of
 * the kernel stack: frame starts at kstack_top - 152.
 * The iretq path must build a 40-byte iretq frame and a
 * 120-byte GPR save area, all within the stack bounds.
 *
 * BUGGY: writes scratch data at 152(%rsp) = kstack_top,
 *        corrupting the next proc struct.
 * FIXED: uses space BELOW the frame (sub $120, %rsp),
 *        all writes stay within [kstack_base, kstack_top).
 */

/* Model the stack as abstract offsets from kstack_base.
 * kstack_top = kstack_base + KSTACK_SIZE. */
#define KSTACK_SIZE 65536
#define FRAME_SIZE  152    /* sizeof(syscall_frame_t) = 19 * 8 */

/* Track the lowest and highest write offsets */
int write_lo = KSTACK_SIZE;
int write_hi = 0;
bool overflow = false;

inline check_write(offset) {
    if
    :: (offset < 0 || offset >= KSTACK_SIZE) -> overflow = true
    :: else -> skip
    fi;
    if
    :: (offset < write_lo) -> write_lo = offset
    :: else -> skip
    fi;
    if
    :: (offset > write_hi) -> write_hi = offset
    :: else -> skip
    fi
}

active proctype IretqPath() {
    /* RSP starts at kstack_top - FRAME_SIZE (frame occupies top of stack) */
    int rsp = KSTACK_SIZE - FRAME_SIZE;

    /* The frame occupies [rsp, rsp + FRAME_SIZE) = [kstack_top-152, kstack_top) */

#ifdef FIX
    /* FIXED path: sub $120, %rsp — grow downward */
    rsp = rsp - 120;

    /* Step 2: Copy GPRs from frame (at rsp+120) to save area (at rsp+0..112) */
    /* Reads from [rsp+120, rsp+120+152) = old frame — within bounds */
    /* Writes to [rsp, rsp+120) = save area */
    int i = 0;
    do
    :: (i < 120) ->
        check_write(rsp + i);
        i = i + 8
    :: (i >= 120) -> break
    od;

    /* Step 3: Build iretq frame at rsp+120 (overwrites old frame) */
    /* Writes: rsp+120, rsp+128, rsp+136, rsp+144, rsp+152 (5 * 8 = 40 bytes) */
    check_write(rsp + 120);   /* RIP */
    check_write(rsp + 128);   /* CS */
    check_write(rsp + 136);   /* RFLAGS */
    check_write(rsp + 144);   /* RSP */
    check_write(rsp + 152);   /* SS */

    /* Step 4: Restore GPRs (reads from save area — no writes) */
    /* Step 5: lea 120(%rsp), %rsp — move up to iretq frame */
    /* iretq reads from [rsp+120, rsp+120+40) — within bounds */

#else
    /* BUGGY path: writes scratch ABOVE the frame */
    /* mov %rax, 152(%rsp) etc. */
    check_write(rsp + 152);   /* kstack_top + 0: OVERFLOW! */
    check_write(rsp + 160);   /* kstack_top + 8: OVERFLOW! */
    check_write(rsp + 168);   /* kstack_top + 16: OVERFLOW! */

    /* Then builds iretq frame using sub $48 etc. — all below */
    rsp = rsp + 152;          /* lea 152(%rsp), %rsp */
    rsp = rsp - 48;           /* sub $48, %rsp */
    int i = 0;
    do
    :: (i < 48) ->
        check_write(rsp + i);
        i = i + 8
    :: (i >= 48) -> break
    od;
#endif
}

/* Safety: no write at or above kstack_top */
ltl no_overflow { [] !overflow }
