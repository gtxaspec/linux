.. SPDX-License-Identifier: GPL-2.0

===============================================
Ingenic XBurst2 multi-core CPU, A1, T40 and T41
===============================================

The XBurst2 CPU of the Ingenic A1, T40 and T41 has two
MIPS32r2-compatible cores. They are controlled through a core control unit
(CCU) in the SoC, and the SoC's interrupt controller and OSTs are built
around the cores, not shared by them.

Identification
==============

The PRID holds the core's generation in bits 15:13 (1 is XBurst2) and a
revision of the generation below: 0x2000 on the X2000 and 0x2100 on the T40,
T41 and A1. Config4.IE reads 3, so the TLB has TLBINV and TLBINVF.

Core control unit (``ingenic,xburst2-ccu``)
===========================================

- RER (0xf00): the reset entry address of a released core, a kernel address.
- CSRR (0x40): bit n holds core n in software reset.
- PIMR (0x120): bit n lets core n take the interrupt controller's output.
- MIMR (0x160): bit n lets core n take its mailbox interrupt.
- OIMR (0x1a0): bit n lets core n take its core OST interrupt.
- MBR(n) (0x1000 + 4n): the mailbox of core n; a nonzero value raises its
  interrupt and zero clears it.

The three interrupts reach a core on the CPU interrupt pins 2 (interrupt
controller), 3 (mailbox) and 4 (core OST).

Booting the second core
=======================

``xb2_boot_secondary()`` writes the address of ``xb2_secondary_entry`` to RER,
holds the core in reset, publishes its idle task's stack and gp and releases
it. The entry code runs with the core's caches and CP0 in their reset state,
sets Config7, KSEG0 cacheability, Cause, Count and Status the way the vendor
kernel does, loads sp and gp and jumps to ``smp_bootstrap``. The core then
sets its bits in PIMR, MIMR and OIMR and its Status.IM bits for the pins 2 to 4.

Inter-processor interrupts
==========================

A sender ORs the action into a per-core word and writes 1 to the target's
mailbox. The target clears the mailbox, takes the word and runs the
reschedule and call-function actions.

Interrupt routing
=================

``ingenic,t41-intc`` (the A1's and T40's too) has one window per core (0x100 apart),
each with two banks of 32 sources and a mask of its own. A source is delivered to the core
in whose window it is unmasked. The driver keeps the core of every source
and moves the mask on an affinity change; all sources start on core 0.

Timers
======

``ingenic,t41-ost`` (also the A1's and T40's): a 64-bit global OST (clocksource and scheduler clock)
and a 32-bit one-shot OST per core (clockevent) at 0x100 intervals.

Not supported
=============

CPU hotplug (the vendor kernel's WAIT-based parking is not ported), the
MXU3 SIMD unit and the media blocks.
