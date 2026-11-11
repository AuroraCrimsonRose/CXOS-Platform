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

### 4a. More, and the open questions settled (Aurora, 2026-11-11)

**TLS is our own,** written in X, not ported: the userland is X, so a C
library such as BearSSL would be rewritten anyway, and "build our own
reader" (roadmap Phase B, media) applies to the protocol too.

- TLS 1.3 first; a TLS 1.2 client with modern cipher suites only, for servers
  without 1.3. No SSL, no TLS 1.0 or 1.1, no RSA key exchange, no CBC.
- X25519 key exchange, with the post-quantum hybrid (X25519 + ML-KEM-768)
  once the base is done; ChaCha20-Poly1305 and AES-GCM (AES-NI on the
  FX-8350, since table-based AES is not constant-time); certificate
  signatures ECDSA P-256 and P-384, Ed25519, RSA-PSS and PKCS#1 v1.5.
- Cryptography is the riskiest code to write ourselves: every primitive is
  tested against Wycheproof's vectors (Apache-2.0, vendored as data) and
  NIST's; secret-dependent branches and memory accesses are checked for;
  every check is sabotaged.
- Certificates (X.509 and ASN.1 DER, a classic source of parser bugs) are
  parsed in their own process with no grants, behind a checker, as the media
  decoders are.
- The trust store is Mozilla's CA list (data, vendored with its licence);
  certificate transparency is checked once the logs' list is vendored too.

**The first version is in X** (§4 item 6): the service waits for X's
bounds-checked slices and overflow traps rather than being written in C and
moved.

**Further points:**

1. **A system HTTP client** (HTTP/1.1 and HTTP/2; QUIC and HTTP/3 later) with
   caching, proxies and cookies kept per app, so apps do not each carry one
   (macOS's URLSession is the precedent).
2. **Per-app packet capture:** every packet passes through the service, so
   one app's traffic can be captured (pcapng, for the host's tools), behind
   its own grant and the consent screen, with an indicator while it runs.
3. **Listening is declared:** inbound connections are refused unless the
   app's package declares that it listens; the Process Manager shows which
   app listens on which port.
4. **Local network permission** (the iOS precedent), separate from the
   internet; mDNS and DNS-SD service discovery provided by the system, so an
   app sees only what it was allowed to.
5. **Data policy per app:** "unmetered networks only", "never in the
   background", and usage per app per network.
6. **Network change notices** (Network.framework's path monitor): an app is
   told when the route, the interface or metering changes, instead of
   hanging on a dead connection.
7. **Authenticated time:** NTP with Network Time Security (RFC 8915), since
   checking a certificate's dates needs a clock nobody on the network can
   set.
8. **Encrypted DNS:** DNS over HTTPS beside §4 item 9's DNS over TLS;
   DNSSEC validation as a setting.
9. **VPN per app:** WireGuard (Later, below), able to carry chosen apps only.

### 4b. IPv6 (Aurora, 2026-11-11)

1. **IPv6-only networks work:** "IPv6-only preferred" (DHCP option 108,
   RFC 8925) is honoured; the NAT64 prefix is discovered (RFC 7050, RFC 8781);
   the service runs a small translator (464XLAT's CLAT, RFC 6877), so an
   IPv4-only app or literal address still connects. iOS and Android are the
   precedent.
2. **Stable private addresses** (RFC 7217): the long-lived address differs
   per network and never comes from the MAC; temporary addresses (§4 item 9)
   stay for outgoing connections.
3. **Happy Eyeballs v3** (the IETF draft): uses DNS HTTPS/SVCB records, which
   also carry QUIC and Encrypted Client Hello hints.
4. **Prefix delegation** (DHCPv6-PD), so a CXOS machine can route for VMs or
   containers later without NAT.
5. **Tested on every kind of network:** the simulated network (§4 item 5)
   runs IPv4-only, dual-stack and IPv6-only with NAT64; a regression on any
   of them fails.

### 4c. The update and download protocol (Aurora, 2026-11-11)

Our own protocol, for the OS's updates and application downloads from
CATX's service. Being obscure is not where its security comes from
(Kerckhoffs's principle): that is published cryptography and signed content.
What is ours is the protocol built from them.

1. **The content carries the security.** Every package and every update list
   is signed with offline keys (the `.xuex` signing); nothing is used before
   its signature is checked. The update list follows TUF's design (The Update
   Framework): separate keys for root, targets, snapshot and timestamp, with
   protection against rollback to an old version and against a frozen list.
   A mirror, a cache or a broken connection can therefore never install
   anything.
2. **The connection: CX Update Protocol** instead of TLS.
   - A Noise framework handshake (`Noise_IK`, as WireGuard), written here;
     the server's static key is in the OS image, so there are no certificate
     authorities and no certificates.
   - X25519 with ML-KEM-768 (post-quantum hybrid), ChaCha20-Poly1305.
   - **Silent to scanners:** the server answers nothing unless the first
     packet is a valid handshake.
   - Over UDP, with our own resumable transfer: files split into chunks named
     by their hash, fetched in parallel, each checked on arrival.
   - Falls back to TCP, then to plain HTTPS on port 443 for networks that
     block the rest; the content is signed, so the fallback loses nothing.
   - Optional device keys for downloads limited to registered machines.
   - Messages are fixed records described in `.xfil`, read through a
     checker; the protocol is versioned.
3. **The server is ours too:** C#, in the DevKit, sharing the protocol code
   and the simulator tests; every check is sabotaged.
4. **Not invented here:** ciphers and the handshake pattern. The code is
   ours; the designs are published.

## 5. Order

Needs Phase 1 (memory objects, IPC v3, the wait queue) and Phase 2's
user-space drivers. Then, in Phase B:

1. Driver-to-stack rings; the network service with today's ARP, IPv4 and
   ICMP; UDP and DHCP. The kernel's network code is deleted.
2. The DNS service.
3. TCP.
4. IPv6 with SLAAC and DHCPv6, stable private addresses, NAT64 and the
   CLAT (§4b).
5. Per-app network permission and attribution in the Process Manager.
6. Declared listening (§4a item 3), local network permission and mDNS.
7. Our own TLS (§4a), the certificate process, NTS time; connect by name.
8. The system HTTP client, change notices, data policy, encrypted DNS,
   per-app capture.
9. The update and download protocol (§4c), with Phase 3's packages.

Later: Happy Eyeballs v3, prefix delegation, VPN (WireGuard, per app), the post-quantum hybrid, QUIC and HTTP/3,
firewall rules beyond per-app, Wi-Fi (the rig has none).

## 6. Open questions

- Both earlier questions are settled in §4a (TLS our own; the first version
  in X).
- MASQUE (IP over HTTP/3, RFC 9484) as a second VPN type beside WireGuard,
  after QUIC: not yet decided.
