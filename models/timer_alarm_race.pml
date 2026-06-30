/*
 * timer_eoi_kernel alarm safety model for ember SMP.
 *
 * Models the actual ember code:
 *   - timer_eoi_kernel (timer.c:116-135): on SMP, only does LAPIC EOI
 *     and kernel_ticks++.  Does NOT access alarm, signal, or process
 *     state (guarded by cpu_count <= 1 check at line 129).
 *   - alarm cancellation (alarm(0)): runs under BKL on syscall path.
 *   - alarm expiry check: runs under BKL in signal_deliver/timer_handler.
 *
 * Verifies: no stale alarm fire — if alarm(0) cancels the alarm,
 * the alarm check (under BKL) sees alarm_tick=0 and doesn't fire.
 *
 * Verify:
 *   spin -a models/timer_alarm_race.pml && \
 *   gcc -O2 -o pan pan.c && ./pan -E -m100000
 */

byte kernel_ticks = 3;
byte alarm_tick = 5;
bool sig_pending = false;

bool bkl = false;
byte bkl_cpu = 255;

bool alarm_cancelled = false;
/* stale_fire: alarm check fires AFTER cancellation */
bool stale_fire = false;

inline BKL_ACQ(c) {
    atomic { !bkl -> bkl = true; bkl_cpu = c }
}
inline BKL_REL(c) {
    assert(bkl && bkl_cpu == c);
    bkl_cpu = 255; bkl = false
}

/* CPU0 (BKL held): alarm(0) cancels alarm */
active proctype cpu0_alarm_cancel() {
    BKL_ACQ(0);
    alarm_tick = 0;
    alarm_cancelled = true;
    BKL_REL(0)
}

/* CPU1: timer_eoi_kernel (NO BKL).
 * Matches timer.c:116-135: on SMP only does EOI + kernel_ticks++. */
active proctype cpu1_timer_eoi() {
    byte tmp;
    tmp = kernel_ticks;
    tmp = tmp + 1;
    kernel_ticks = tmp;

    tmp = kernel_ticks;
    tmp = tmp + 1;
    kernel_ticks = tmp
}

/* BKL-protected alarm check (in timer_handler or signal_deliver). */
active proctype bkl_alarm_check() {
    BKL_ACQ(1);

    if
    :: (alarm_tick != 0) && (kernel_ticks >= alarm_tick) ->
        sig_pending = true;
        alarm_tick = 0;
        /* Check if this is a stale fire */
        if
        :: alarm_cancelled -> stale_fire = true
        :: else -> skip
        fi
    :: else -> skip
    fi;

    BKL_REL(1)
}

/* Safety: alarm check never fires after cancellation */
ltl no_stale_alarm { [] !stale_fire }
