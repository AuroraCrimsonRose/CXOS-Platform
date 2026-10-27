# Networking
### CX Design Spec: Aurora Tejeda / CATX Systems

> **Status: DESIGN, for x86-64.** Where the network stack lives, its shape, and
> where CXOS aims to do better than the systems it was compared with. What is
> built today is `kernel/drivers/net/` and `ktest_net.c`.

---

## 1. Where it stands

About 1,900 lines, all in the kernel: the `e1000` and `rtl8169` drivers,
`netif`, ARP, IPv4 and ICMP. No UDP, TCP, DHCP, DNS or IPv6. Programs reach it
through the `SYS_NET_*` calls:

- `SYS_NET_SET_IP` sets the address by hand; there is no DHCP.
- `SYS_NET_SEND` and `SYS_NET_RECV` move **raw Ethernet frames**. Any program
  holding `GRANT_NET` can therefore read every frame on the machine and send
  frames claiming to be anyone. `GRANT_NET` is all or nothing.

Little is built, so this is the cheapest moment to choose the shape.

## 2. How the others do it

**In the kernel:**

- **Serenity:** `Kernel/Net`; TCP is one long function, `handle_tcp` in
  `NetworkTask.cpp` (lines 584–897), with retransmission in `TCPSocket.cpp`.
- **Haiku:** in the kernel but modular: each protocol is an add-on
  (`src/add-ons/kernel/network/protocols/{ipv4,ipv6,tcp,udp,icmp,icmp6,unix}`).
- **NT:** `tcpip.sys` under NDIS. NT5 shipped **two** stacks, `net/tcpip/driver`
  for IPv4 and `net/tcpip/tpipv6` for IPv6, a split Vista spent a rewrite
  undoing.
- **XNU:** the BSD stack (`bsd/netinet`), plus **Skywalk** (`bsd/skywalk`):
  shared-memory rings of packet slots between the kernel and user processes
  (`kern_channel_slot_get_packet`, `channel_kern.c`). Apple uses it to move
  networking work out of the kernel. Network.framework gives apps
  connect-by-name with TLS built in.

**In user space:**

- **Fuchsia:** netstack3, a user-space process written in Rust; drivers hand
  it packets through shared-memory rings.
- **Redox:** `smolnetd` (on smoltcp) serves `tcp:` and `udp:` schemes. Not in
  the indexed tree (kernel only).

**Lessons:**

- The socket API is the lowest common denominator, and everyone has
  retrofitted asynchronous I/O: IOCP, epoll, kqueue, io_uring.
- On Linux each app brings its own TLS library, so a flaw is patched app by
  app. Windows (SChannel) and macOS (Network.framework) provide TLS from the
  system, so one fix covers every app.

## 3. The shape

**The stack is a user-space service**, not part of the kernel.

```
NIC driver  ⇄  shared rings  ⇄  network service  ⇄  per-socket rings / IPC  ⇄  apps
(own process, (memory objects)  (IPv4/IPv6, TCP,
 own device)                     UDP, DHCP, DNS)
```

- **Why:** the stack parses packets that anyone on the network can send, which
  is the classic remote kernel exploit. As a process it holds only its rings
  and its configuration, it can restart without a reboot, and the same code
  runs on the host for tests (§4.5).
- **Cost:** one more hop than an in-kernel stack. Rings in shared memory with
  batching keep the per-packet cost low (Skywalk, Fuchsia); the rig's gigabit
  link is well within reach.
- **Driver to stack:** rings of packet buffers in memory objects, with
  notifications through the wait queue. Nothing is copied. A driver is an
  ordinary user-space driver (`kernel/CX_DRIVERS.md`) holding only its device.
- **Stack to apps:** a typed `.xfil` interface for control (open, connect,
  bind, options) and, per socket, a pair of shared-memory rings for data, so a
  send or receive does not pass through the kernel at all.
- **Dual stack from the start:** IPv4 and IPv6 in one code path, never two
  stacks.
- **Today's code moves:** ARP, IPv4 and ICMP leave the kernel for the service;
  the two drivers become user-space drivers. The `SYS_NET_*` calls are retired.

## 4. Where CXOS aims to be better

1. **No ambient network.** An app gets a network handle from the broker, never
   by default. Its package declares what it needs: none, the local network,
   the internet, or named hosts. The user sees the declaration at install and
   can change it.
2. **Every connection is attributed** to the app that opened it, through the
   IPC badge. The Process Manager shows each app's connections and traffic,
   and the user can cut an app off. A built-in, per-app firewall.
3. **Connect by name.** `connect("example.com", 443, tls)` does DNS, IPv4/IPv6
   racing (Happy Eyeballs, RFC 8305) and TLS in one call, as Network.framework
   does. Apps do not each carry a resolver and a TLS library.
4. **TLS from the system:** one implementation, the system trust store, and
   optional pinning per app. A flaw is fixed once.
5. **Deterministic and testable.** The stack has no clock or randomness of its
   own; time, random numbers and packets all arrive through its interface. The
   same code runs in the DevKit against a simulated network with loss,
   reordering and delay, any failure replays exactly, and every packet parser
   is fuzzed on the host.
6. **Written in X** once X has bounds-checked slices and overflow traps
   (`language/CX_X_PLAN.md` §3). Packet parsers are where memory safety pays
   most; this is the first large X service.
7. **Asynchronous by default.** Completions arrive through the wait queue; a
   blocking call is a convenience in `std` on top.
8. **Modern TCP:** SACK, window scaling and timestamps; RACK-TLP loss
   detection; CUBIC first, BBR later; path MTU discovery; SYN cookies; ECN;
   randomised initial sequence numbers and ports.
9. **Private by default:** IPv6 temporary addresses (RFC 8981); DNS over TLS
   when the resolver supports it; a randomised MAC per network later.
10. **Offloads where the NIC has them:** checksum offload (both the e1000 and
    the 8111 have it); segmentation offload later.

## 5. Order

Needs Phase 1 (memory objects, IPC v3, the wait queue) and Phase 2's
user-space drivers. Then, in Phase B:

1. Driver-to-stack rings; the network service with today's ARP, IPv4 and
   ICMP; UDP and DHCP. The kernel's network code is deleted.
2. The DNS service.
3. TCP.
4. IPv6 with SLAAC and DHCPv6.
5. Per-app network permission and attribution in the Process Manager.
6. Connect by name; TLS from the system.

Later: VPN (WireGuard), firewall rules beyond per-app, Wi-Fi (the rig has
none).

## 6. Open questions

- **TLS:** write it, or port a small audited library (BearSSL, for instance)
  under the Build / Port / Embed rule.
- Whether the first version is in C, moved to X later, or waits for X's
  slices and starts in X.
