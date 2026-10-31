/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "../include/packetbridge_types.h"
/* The private adapter terminates Ethernet locally. The shared ABI carries IP only. */
#define PB_ETH_HEADER 14u
#define PB_ETH_CONTROL_MAX 128u
/* One VLAN header, the IPv6 base header, and its first payload byte cover
 * Inspect, the direct neighbor-message gate and address-only queue selection. */
#define PB_ETH_CLASSIFY_MAX (18u + 40u + 1u)
typedef enum PB_ETH_RESULT { PB_ETH_DROP, PB_ETH_IP, PB_ETH_REPLY } PB_ETH_RESULT;
/* Understand one in-band 802.1Q tag; no VLAN trunk is exposed through the L3 ABI. */
PB_ETH_RESULT PbEthernetInspect(const uint8_t *frame, uint32_t length, uint32_t *ipOffset);
/* Replies are limited to ARP and direct IPv6 neighbor solicitation. The SYS
 * send path gates this parser using Inspect and its neighbor-message policy;
 * adding a new reply family requires updating that gate and its regression. */
PB_ETH_RESULT PbEthernetControl(const uint8_t *frame, uint32_t length, const uint8_t peer[6],
                               uint8_t reply[PB_ETH_CONTROL_MAX], uint32_t *replyLength);
void PbEthernetHeader(uint8_t header[PB_ETH_HEADER], const uint8_t local[6], const uint8_t peer[6],
                      const uint8_t *ip, uint32_t length);
