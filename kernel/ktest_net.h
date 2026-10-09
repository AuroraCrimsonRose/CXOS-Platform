// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// SPDX-FileCopyrightText: 2026 Aurora Tejeda (trading as CATX Systems)
/* /kernel/ktest_net.h */
/* Aurora Tejeda / CATX Systems */
/*
 * Adversarial cases for the network parsers (the 2026-10-09 platform security
 * review §1-§2, Phase 2.5 in docs/planning/HARDENING_PLAN.md).
 *
 * Kept out of ktest.c for the reason ktest_loader.c is: these bring their own
 * frame builder. They need no NIC - the parsers take a buffer and a length, so
 * every case is a frame built in memory and handed straight to them. That
 * matters, because it means the network parsers are now covered on every
 * machine rather than only on one with a working e1000.
 */

#ifndef KTEST_NET_H
#define KTEST_NET_H

/* Every malformed IPv4 frame ip_parse must refuse, plus well-formed ones it
   must accept. 1 = all cases behaved, 0 = at least one did not. */
int ktest_ip_parse_adversarial(void);

/* ARP frames: bounds, and the fields arp_input must check before believing
   the addresses it is given. */
int ktest_arp_input_adversarial(void);

#endif
