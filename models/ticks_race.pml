/*
 * ticks_race.pml - SPIN model for kernel_ticks increment race under SMP
 *
 * ember has two timer ISR paths that both increment kernel_ticks:
 *   - timer_handler()    : user-mode ISR, runs under BKL
 *   - timer_eoi_kernel() : kernel-mode ISR, runs WITHOUT BKL
 *
 * On x86_64, a plain C "kernel_ticks++" compiles to a non-atomic
 * read-modify-write (load, add 1, store).  Two concurrent increments
 * on different CPUs can lose ticks: one CPU reads a stale value, adds 1,
 * and writes back a value that is LOWER than what the other CPU already
 * stored -- effectively rolling the counter backwards within a race
 * window.
 *
 * This model verifies:
 *   (1) Safety:  kernel_ticks is always >= 1 at completion
 *                (the counter makes forward progress despite races)
 *   (2) Liveness: a nanosleep waiter always eventually completes
 *                 (lost ticks cause timing imprecision, not deadlock)
 *
 * The key finding: with N=3 rounds per CPU (6 total increments),
 * kernel_ticks can end as low as 2.  A stale write can regress the
 * counter, but the overall trend is still forward.  nanosleep uses
 * "while (kernel_ticks < target)" which tolerates temporary regression
 * because fresh increments always eventually advance past any target.
 *
 * Verification:
 *   spin -a ticks_race.pml && gcc -O2 -o pan pan.c -DSAFETY
 *   ./pan -E -m100000 -N safety          # safety: 0 errors
 *
 *   spin -a ticks_race.pml && gcc -O2 -o pan pan.c
 *   ./pan -a -f -E -m100000 -N liveness  # liveness: 0 errors
 *                                         # (-f = weak fairness, required
 *                                         #  because HW timers always fire)
 */

/* Shared counter -- models the volatile uint64_t kernel_ticks */
byte kernel_ticks = 0;

/* Per-CPU completion flags */
bool cpu0_done = false;
bool cpu1_done = false;

/* How many rounds each CPU performs */
#define ROUNDS 3

/*
 * CPU 0: timer_handler (BKL held, but BKL does NOT protect against
 * timer_eoi_kernel on the other CPU).
 *
 * Non-atomic increment modeled as three separate statements:
 *   tmp = kernel_ticks;  tmp = tmp + 1;  kernel_ticks = tmp;
 */
active proctype cpu0_timer_handler() {
    byte i = 0;
    byte tmp;
    do
    :: i < ROUNDS ->
        tmp = kernel_ticks;
        tmp = tmp + 1;
        kernel_ticks = tmp;
        i++
    :: else -> break
    od;
    cpu0_done = true
}

/*
 * CPU 1: timer_eoi_kernel (NO BKL).
 * Same non-atomic read-modify-write.
 */
active proctype cpu1_timer_eoi_kernel() {
    byte i = 0;
    byte tmp;
    do
    :: i < ROUNDS ->
        tmp = kernel_ticks;
        tmp = tmp + 1;
        kernel_ticks = tmp;
        i++
    :: else -> break
    od;
    cpu1_done = true
}

/*
 * nanosleep waiter: models  while (kernel_ticks < target) sched_sleep()
 *
 * Target is 2.  Even in the worst-case race, both CPUs together
 * produce enough forward progress to reach 2.  The waiter always
 * terminates -- lost ticks only delay it, they don't block it.
 */
bool waiter_done = false;

active proctype nanosleep_waiter() {
    do
    :: kernel_ticks >= 2 -> break
    :: else -> skip   /* sched_sleep: yield and re-check */
    od;
    waiter_done = true
}

/*
 * Safety: when both CPUs are done, kernel_ticks >= 1.
 *
 * Reasoning: the very last write to kernel_ticks is "tmp = old + 1"
 * for some old >= 0, so kernel_ticks >= 1 at termination.  In practice
 * SPIN finds the minimum is 2 for ROUNDS=3.
 *
 * The upper bound is TOTAL = 2*ROUNDS = 6 (no races at all).
 */
#define both_done       (cpu0_done && cpu1_done)
#define ticks_lower_ok  (kernel_ticks >= 1)
#define ticks_upper_ok  (kernel_ticks <= (2 * ROUNDS))
#define ticks_ok        (ticks_lower_ok && ticks_upper_ok)

ltl safety   { [] (both_done -> ticks_ok) }
ltl liveness { <> waiter_done }
