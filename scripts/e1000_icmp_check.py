#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Gabi Falk
# Connect to a QEMU -netdev socket (length-prefixed raw Ethernet). First ARP
# for 10.0.2.15 and learn the guest MAC from its reply, then send an ICMP echo
# request and assert a valid echo reply (type 0, matching id/seq, echoed
# payload, correct IP and ICMP checksums).
import socket, struct, sys, time

HOST, PORT = "127.0.0.1", int(sys.argv[1])
SRC_MAC = bytes.fromhex("525400123456")
BCAST = b"\xff" * 6
SENDER_IP = bytes([10, 0, 2, 2])
TARGET_IP = bytes([10, 0, 2, 15])
IDENT, SEQ = 0x1337, 1
PAYLOAD = bytes(range(32))


def csum(data):
    if len(data) % 2:
        data += b"\x00"
    s = 0
    for i in range(0, len(data), 2):
        s += (data[i] << 8) | data[i + 1]
    while s >> 16:
        s = (s & 0xffff) + (s >> 16)
    return (~s) & 0xffff


def arp_request():
    eth = BCAST + SRC_MAC + b"\x08\x06"
    arp = (b"\x00\x01\x08\x00\x06\x04\x00\x01" + SRC_MAC + SENDER_IP +
           b"\x00\x00\x00\x00\x00\x00" + TARGET_IP)
    return eth + arp


def icmp_echo(dst_mac):
    icmp = struct.pack(">BBHHH", 8, 0, 0, IDENT, SEQ) + PAYLOAD
    icmp = struct.pack(">BBHHH", 8, 0, csum(icmp), IDENT, SEQ) + PAYLOAD
    total = 20 + len(icmp)
    iph = struct.pack(">BBHHHBBH", 0x45, 0, total, 0xABCD, 0, 64, 1, 0) + SENDER_IP + TARGET_IP
    iph = struct.pack(">BBHHHBBH", 0x45, 0, total, 0xABCD, 0, 64, 1, csum(iph)) + SENDER_IP + TARGET_IP
    return dst_mac + SRC_MAC + b"\x08\x00" + iph + icmp


def send(s, frame):
    s.sendall(struct.pack(">I", len(frame)) + frame)


def main():
    deadline = time.time() + 60
    s = None
    while time.time() < deadline and s is None:
        try:
            s = socket.create_connection((HOST, PORT), timeout=2)
        except OSError:
            time.sleep(0.2)
    if s is None:
        print("FAIL: could not connect to QEMU netdev socket"); return 1
    s.settimeout(2)

    guest_mac = None
    last_send = 0.0
    buf = b""
    while time.time() < deadline:
        now = time.time()
        if now - last_send >= 3.0:
            if guest_mac is None:
                send(s, arp_request())
            else:
                send(s, icmp_echo(guest_mac))
            last_send = now
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            break
        buf += chunk
        while len(buf) >= 4:
            (flen,) = struct.unpack(">I", buf[:4])
            if len(buf) < 4 + flen:
                break
            f, buf = buf[4:4 + flen], buf[4 + flen:]
            if len(f) < 14:
                continue
            et = (f[12] << 8) | f[13]
            if et == 0x0806 and guest_mac is None and len(f) >= 42 \
               and f[20:22] == b"\x00\x02" and f[28:32] == TARGET_IP:
                guest_mac = f[22:28]            # SHA of the ARP reply
                last_send = 0.0                 # send the ping now
                continue
            if et == 0x0800 and len(f) >= 14 + 20 + 8:
                ihl = (f[14] & 0x0f) * 4
                ip = f[14:14 + ihl]
                if csum(ip) != 0:               # IP header must validate
                    continue
                if f[14 + 9] != 1:              # protocol ICMP
                    continue
                icmp = f[14 + ihl:14 + (f[16] << 8 | f[17])]
                if len(icmp) < 8 or csum(icmp) != 0:
                    continue
                if icmp[0] == 0 and (icmp[4] << 8 | icmp[5]) == IDENT \
                   and (icmp[6] << 8 | icmp[7]) == SEQ and icmp[8:] == PAYLOAD:
                    print("PASS: ICMP echo reply from 10.0.2.15"); return 0
    print("FAIL: no valid ICMP echo reply"); return 1


sys.exit(main())
