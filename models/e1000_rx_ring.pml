/* SPDX-License-Identifier: GPL-2.0-or-later */
/* e1000 legacy RX ring: hardware sets DD as it fills descriptors; the driver
 * consumes [head .. first non-DD), processing each exactly once, then advances
 * the tail. Checks no double-process and no skip across wraparound. */
#define N 4
bit dd[N];          /* descriptor done bits (hardware sets, driver clears) */
byte head = 0;      /* next descriptor the driver will inspect */
byte produced = 0;  /* count hardware has filled */
byte consumed = 0;  /* count driver has processed */

active proctype hardware() {
	byte i = 0;
	do
	:: (produced < N) ->
		atomic { dd[(i) % N] = 1; produced++; i++ }
	:: (produced == N) -> break
	od
}

active proctype driver() {
	do
	:: dd[head] == 1 ->
		atomic {
			dd[head] = 0;          /* recycle */
			consumed++;
			head = (head + 1) % N;
		}
	:: (consumed == N) -> break
	:: else -> skip                /* nothing ready; would wait for IRQ */
	od
}

/* Every produced frame is eventually consumed exactly once. */
ltl complete { <> (consumed == N) }
/* Never consume more than produced (no processing a non-DD descriptor). */
ltl no_overrun { [] (consumed <= produced) }
