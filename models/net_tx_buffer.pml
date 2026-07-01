/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Net TX assembly buffer ownership. Building an outbound frame is a multi-step,
 * non-atomic write (icmp writes the payload, ipv4 writes the L3 header, eth
 * writes the L2 header). The frame handed to the NIC must be internally
 * consistent: its header and payload must come from the SAME send.
 *
 * With per-call (private) assembly buffers, two senders never touch the same
 * bytes, so consistency holds under any interleaving - no lock required.
 *
 * Compile with -DSHARED_BUG to model the old process-global static buffers:
 * both senders assemble into one buffer and an interleave transmits a frame
 * whose header is from one send and payload from the other. SPIN finds the
 * counterexample, which is exactly the corruption the statics permitted once
 * more than one context can originate a packet.
 *
 *   spin -a models/net_tx_buffer.pml && cc -O2 -o pan pan.c && ./pan        (holds)
 *   spin -a -DSHARED_BUG models/net_tx_buffer.pml && cc -DSHARED_BUG ... && ./pan  (fails)
 */

#ifdef SHARED_BUG
#define SLOT1 0
#define SLOT2 0			/* both senders share one buffer */
#else
#define SLOT1 1
#define SLOT2 2			/* each send owns a private buffer */
#endif

byte hdr[3];			/* assembled header field, per slot */
byte pay[3];			/* assembled payload field, per slot */

inline assemble_and_transmit(id, slot) {
	pay[slot] = id;		/* icmp lays down the payload */
	hdr[slot] = id;		/* ipv4/eth lay down the header */
	/* hand the assembled frame to the NIC: it must be one send's bytes */
	assert(hdr[slot] == pay[slot]);
}

active proctype sender1() { assemble_and_transmit(1, SLOT1) }
active proctype sender2() { assemble_and_transmit(2, SLOT2) }
