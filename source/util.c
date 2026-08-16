/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "util.h"
#include "config.h"

// Per-thread TLS block for the engine stack-protector guard at tpidr_el0 + 0x28
// AND bionic thread-local storage. TPIDR_EL0 must be UNIQUE per thread: a single
// shared block let every game/CRI worker thread clobber each other's canary and
// thread-local state (errno, TLS keys) -> races/corruption on the async loader &
// decompression threads. Give each thread its own block (leaked; must stay live
// for the thread's lifetime). Matches the ys1x_nx / gdash_nx approach.
void tls_setup_guard(void) {
  uint8_t *tls = (uint8_t *)calloc(1, 0x1000);
  if (!tls) return;
  *(uint64_t *)(tls + 0x28) = 0x0123456789ABCDEFull;
  armSetTlsRw(tls);
}

// boost the CPU to 1785MHz while loading
void cpu_boost(int on) {
  appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}

int ret0(void) { return 0; }

int retm1(void) { return -1; }
