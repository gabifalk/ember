/* elf_overlap.pml — ELF loader overlapping PT_LOAD segment model
 *
 * Bug: elf_load_user() allocates a fresh PA for every page in each
 * PT_LOAD segment's page-aligned range.  When two segments share a
 * boundary page, the second allocation overwrites the PTE, losing
 * the first segment's data and leaking the old PA.
 *
 * Scenario (matches /bin/gcc crash at RIP=0x41fc18):
 *   seg0 (.text):  VA pages [0,1,2]   — code
 *   seg1 (.rodata): VA pages [2,3,4]  — read-only data
 *   Overlap at VA page 2.
 *
 * Compile:
 *   Buggy:  spin -a elf_overlap.pml && cc -o pan pan.c && ./pan
 *   Fixed:  spin -a -DFIXED elf_overlap.pml && cc -o pan pan.c && ./pan
 *
 * Properties:
 *   P1 (seg0_intact):  seg0 data reachable at VA 0, 1, 2
 *   P2 (seg1_intact):  seg1 data reachable at VA 2, 3, 4
 *   P3 (no_leak):      every allocated PA is mapped to some VA
 */

#define NPA  8          /* physical page pool */
#define NVA  5          /* virtual pages 0..4 */

byte pt[NVA];           /* page table: VA -> PA;  255 = unmapped */
bool used[NPA];         /* PMM: allocated? */
/* Per-PA data bitmap: bit 0 = seg0 wrote, bit 1 = seg1 wrote */
byte seg_data[NPA];

active proctype elf_loader()
{
    byte va, pa, i, j;

    /* ---------- init ---------- */
    d_step {
        i = 0;
        do
        :: (i < NVA) -> pt[i] = 255; i++
        :: else      -> break
        od;
        i = 0;
        do
        :: (i < NPA) -> used[i] = false; seg_data[i] = 0; i++
        :: else      -> break
        od
    };

    /* ========== Load segment 0: VA 0, 1, 2 ========== */
    va = 0;
    do
    :: (va <= 2) ->
        d_step {
#ifdef FIXED
            /* FIXED: reuse existing mapping */
            if
            :: (pt[va] != 255) -> pa = pt[va]
            :: else ->
                pa = 255; i = 0;
                do
                :: (i < NPA && pa == 255) ->
                    if :: !used[i] -> used[i] = true; pa = i
                       :: else     -> skip
                    fi; i++
                :: else -> break
                od
            fi;
#else
            /* BUGGY: always allocate a new page */
            pa = 255; i = 0;
            do
            :: (i < NPA && pa == 255) ->
                if :: !used[i] -> used[i] = true; pa = i
                   :: else     -> skip
                fi; i++
            :: else -> break
            od;
#endif
            assert(pa != 255);
            pt[va] = pa;
            seg_data[pa] = seg_data[pa] | 1     /* seg0 data present */
        };
        va++
    :: else -> break
    od;

    /* ========== Load segment 1: VA 2, 3, 4 ========== */
    va = 2;
    do
    :: (va <= 4) ->
        d_step {
#ifdef FIXED
            if
            :: (pt[va] != 255) -> pa = pt[va]
            :: else ->
                pa = 255; i = 0;
                do
                :: (i < NPA && pa == 255) ->
                    if :: !used[i] -> used[i] = true; pa = i
                       :: else     -> skip
                    fi; i++
                :: else -> break
                od
            fi;
#else
            pa = 255; i = 0;
            do
            :: (i < NPA && pa == 255) ->
                if :: !used[i] -> used[i] = true; pa = i
                   :: else     -> skip
                fi; i++
            :: else -> break
            od;
#endif
            assert(pa != 255);
            pt[va] = pa;
            seg_data[pa] = seg_data[pa] | 2     /* seg1 data present */
        };
        va++
    :: else -> break
    od;

    /* ========== Verification ========== */

    /* P1: seg0 data reachable at its VA range */
    assert(seg_data[pt[0]] & 1);    /* VA 0: seg0 */
    assert(seg_data[pt[1]] & 1);    /* VA 1: seg0 */
    assert(seg_data[pt[2]] & 1);    /* VA 2: seg0 — FAILS when buggy */

    /* P2: seg1 data reachable at its VA range */
    assert(seg_data[pt[2]] & 2);    /* VA 2: seg1 */
    assert(seg_data[pt[3]] & 2);    /* VA 3: seg1 */
    assert(seg_data[pt[4]] & 2);    /* VA 4: seg1 */

    /* P3: no leaks — every allocated PA must be in pt[] */
    byte leaks = 0;
    i = 0;
    do
    :: (i < NPA) ->
        if
        :: used[i] ->
            byte found = 0;
            j = 0;
            do
            :: (j < NVA) ->
                if :: (pt[j] == i) -> found = 1
                   :: else         -> skip
                fi; j++
            :: else -> break
            od;
            if :: (found == 0) -> leaks++
               :: else         -> skip
            fi
        :: else -> skip
        fi; i++
    :: else -> break
    od;
    assert(leaks == 0);             /* no leaked pages */

    printf("P1-P3 pass.  Overlap page VA2: seg_data = %d\n", seg_data[pt[2]])
}
