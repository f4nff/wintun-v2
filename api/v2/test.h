/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
/* Hosted boundary controls; not exported by production DLLs. Configure only while
 * the corresponding operation is idle. The harness owns both event handles. */
#include "packetbridge.h"
#define PB_TEST_POLICY 0u
#define PB_TEST_DRAIN 1u
#define PB_TEST_DEVICE_CLOSE 2u
#define PB_TEST_ADAPTER_CLOSE 3u
#define PB_TEST_QUERY 4u
#define PB_TEST_WORKER_START 5u
#define PB_TEST_HOOKS 6u
PB_RESULT PbTestOpen(const PB_CONFIG *, PB_SESSION **, void **, PB_LAYOUT *, uint64_t *);
void PbTestActive(PB_SESSION *, uint32_t);
void PbTestFault(PB_SESSION *);
uint32_t PbTestWaiters(PB_SESSION *);
uint32_t PbTestQueries(PB_SESSION *);
uint32_t PbTestRetained(uint64_t);
void PbTestBlock(PB_SESSION *, uint32_t, HANDLE, HANDLE);
void PbTestFail(PB_SESSION *, uint32_t, BOOL);
uint32_t PbTestBoundaryThread(PB_SESSION *, uint32_t);
