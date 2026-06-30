/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ARP cache state machine: scan-first insert (update in place), then first
 * free slot, else round-robin eviction. All access is BKL-serialized, so a
 * single process performs arbitrary insert sequences. Checks, after every
 * insert: no duplicate IP, the inserted (ip,mac) is findable (lookup-after-
 * insert), and the eviction cursor stays in range. Eviction isolation holds
 * by construction: only slot `victim` is overwritten. */
#define N 3
#define NIP 3        /* IP id domain 1..NIP; 0 marks an empty slot */

byte ipv[N];
byte macv[N];
byte victim = 0;

active proctype driver() {
	byte a, m, i, k;
	bit done, found;
	do
	:: atomic {
		select(a : 1 .. NIP);
		select(m : 1 .. NIP);

		/* 1. scan-first: update an existing entry for this IP */
		done = 0; i = 0;
		do
		:: (i < N && !done) ->
			if
			:: (ipv[i] == a) -> macv[i] = m; done = 1
			:: else -> skip
			fi; i++
		:: else -> break
		od;

		/* 2. first free slot */
		i = 0;
		do
		:: (i < N && !done) ->
			if
			:: (ipv[i] == 0) -> ipv[i] = a; macv[i] = m; done = 1
			:: else -> skip
			fi; i++
		:: else -> break
		od;

		/* 3. evict the round-robin victim */
		if
		:: !done -> ipv[victim] = a; macv[victim] = m;
			    victim = (victim + 1) % N
		:: else -> skip
		fi;

		/* invariants */
		assert(victim < N);
		assert(!(ipv[0] == ipv[1] && ipv[0] != 0));
		assert(!(ipv[0] == ipv[2] && ipv[0] != 0));
		assert(!(ipv[1] == ipv[2] && ipv[1] != 0));
		found = 0; k = 0;
		do
		:: (k < N && !found) ->
			if
			:: (ipv[k] == a && macv[k] == m) -> found = 1
			:: else -> skip
			fi; k++
		:: else -> break
		od;
		assert(found);
	  }
	od
}
