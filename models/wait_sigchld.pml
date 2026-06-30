/*
 * wait_sigchld.pml -- SIGCHLD + SA_RESTART + wait4 EINTR race.
 *
 * Bug path (EINTR race):
 *   1. Parent calls wait4(-1, 0) — blocking
 *   2. Child exits → ZOMBIE, SIGCHLD pending
 *   3. wait4 sees SIGCHLD with user handler → returns EINTR
 *   4. signal_deliver: SA_RESTART rewinds syscall, runs handler
 *   5. Handler bookkeeping (does NOT call waitpid)
 *   6. rt_sigreturn → restarted wait4
 *   7. Restarted wait4 scans — finds zombie → OK
 *
 * The EINTR itself isn't the problem if the zombie survives.
 * But if SIGCHLD is pending when wait4 hasn't found a zombie yet,
 * it returns EINTR instead of sleeping.  With SA_RESTART, this
 * just restarts — wasteful but correct... UNLESS the handler
 * itself calls waitpid(WNOHANG) (some programs do this).
 *
 * Fix: wait4 never returns EINTR for SIGCHLD.  Exclude SIGCHLD
 * from the pending-signal check.  For SIG_DFL/SIG_IGN, clear
 * sig_pending.  For user handlers, leave sig_pending so
 * signal_deliver fires the handler after wait4 returns.
 *
 * Model: handler is realistic (sets flag, no waitpid call).
 * Also models a "reaping handler" variant to show it's safe too.
 */

#define PROC_UNUSED  0
#define PROC_RUNNING 1
#define PROC_ZOMBIE  2

byte child_state = PROC_UNUSED;
bool sigchld_pending = false;
bool has_sigchld_handler = false;
bool sa_restart = false;

bool echild_bug = false;
bool wait_ok = false;
bool handler_ran = false;

#ifdef FIX
#define SIGCHLD_CAUSES_EINTR false
#else
#define SIGCHLD_CAUSES_EINTR true
#endif

/* Handler behavior: realistic (flag-only) vs aggressive (reaping) */
#ifdef REAPING_HANDLER
#define HANDLER_REAPS true
#else
#define HANDLER_REAPS false
#endif

proctype Child() {
    child_state == PROC_RUNNING;
    child_state = PROC_ZOMBIE;
    sigchld_pending = true;
}

/*
 * signal_deliver: fires on return to userspace from any syscall/ISR.
 * If SIGCHLD pending with user handler, diverts to handler.
 */
inline signal_deliver() {
    if
    :: (sigchld_pending && has_sigchld_handler) ->
        sigchld_pending = false;
        handler_ran = true;
        /* Aggressive handler variant: calls waitpid(WNOHANG) */
        if
        :: HANDLER_REAPS ->
            if
            :: (child_state == PROC_ZOMBIE) ->
                child_state = PROC_UNUSED
            :: else ->
                skip
            fi
        :: else ->
            skip  /* Realistic: just sets a flag */
        fi
    :: else ->
        skip
    fi
}

proctype Parent() {
    byte r;

    has_sigchld_handler = true;
    sa_restart = true;

    /* fork() -- atomically creates child in RUNNING state */
    atomic { child_state = PROC_RUNNING; }

    /* fork returns to userspace -- signal_deliver fires */
    signal_deliver();

    /* --- wait4(-1, 0) --- */
wait4_entry:
    if
    :: (child_state == PROC_ZOMBIE) ->
        child_state = PROC_UNUSED;
        goto wait4_done_ok
    :: (child_state == PROC_RUNNING) ->
        goto wait4_check_sig
    :: (child_state == PROC_UNUSED) ->
        goto wait4_echild
    fi;

wait4_check_sig:
    if
    :: (sigchld_pending && has_sigchld_handler && SIGCHLD_CAUSES_EINTR) ->
        goto wait4_eintr
    :: else ->
        goto wait4_sleep
    fi;

wait4_sleep:
    (child_state == PROC_ZOMBIE || child_state == PROC_UNUSED);
    goto wait4_entry;

wait4_eintr:
    /* signal_deliver: handler runs */
    signal_deliver();
    /* SA_RESTART: re-execute wait4 */
    if
    :: sa_restart -> goto wait4_entry
    :: else -> skip
    fi;
    /* No restart -- user sees EINTR (not modeled as bug) */
    goto wait4_done_ok;

wait4_done_ok:
    /* signal_deliver on return to userspace */
    signal_deliver();
    wait_ok = true;
    goto done;

wait4_echild:
    echild_bug = true;
    goto done;

done:
    skip
}

init {
    run Child();
    run Parent();
}

ltl no_echild { [] !echild_bug }
ltl handler_runs { <> handler_ran }
