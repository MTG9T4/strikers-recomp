/* SPDX-License-Identifier: GPL-3.0-or-later
 * Included once by the generated SDK Network.cpp. The caller supplies the
 * authenticated sourcePeer, never a node claimed inside an SLDN/IP payload. */
#include <stdint.h>
#include <mutex>
#include <string.h>
#include <stdio.h>
static std::mutex strikers_group_mutex;
static bool strikers_group_enabled;
static uint64_t strikers_group_network, strikers_group_nodes[128];
static unsigned strikers_group_count;
static unsigned strikers_group_rejected;
extern "C" __declspec(dllexport) int zts_strikers_group_peers(
    int enabled, uint64_t network, const uint64_t *nodes, unsigned count) {
    if (count > 128 || (count && !nodes)) return -1;
    std::lock_guard<std::mutex> guard(strikers_group_mutex);
    strikers_group_enabled = enabled != 0;
    strikers_group_network = network;
    strikers_group_count = count;
    strikers_group_rejected = 0; /* bounded diagnostics until the next list refresh */
    if (count) memcpy(strikers_group_nodes, nodes, count * sizeof *nodes);
    return 0;
}
static bool strikers_group_frame(uint64_t network, uint64_t source,
    unsigned etherType, const uint8_t *frame, unsigned length) {
    std::lock_guard<std::mutex> guard(strikers_group_mutex);
    if (!strikers_group_enabled) return true;
    if (!strikers_group_network || network != strikers_group_network) return false;
    bool member = false;
    for (unsigned i = 0; i < strikers_group_count; ++i)
        if (strikers_group_nodes[i] == source) { member = true; break; }
    if (!member) {
        if (strikers_group_rejected < 8) {
            ++strikers_group_rejected;
            printf("[zt-group] rejected authenticated node=%010llx\n", (unsigned long long)source);
        }
        return false;
    }
    if ((network >> 56) != 255) return true; /* Central still authenticates the node. */
    if (etherType != 0x86dd || length < 40) return false;
    uint32_t prefix = (uint32_t)(network ^ (network >> 32));
    uint8_t address[16] = {0xfc, (uint8_t)(prefix >> 24), (uint8_t)(prefix >> 16),
        (uint8_t)(prefix >> 8), (uint8_t)prefix, (uint8_t)(source >> 32),
        (uint8_t)(source >> 24), (uint8_t)(source >> 16), (uint8_t)(source >> 8),
        (uint8_t)source, 0,0,0,0,0,1};
    bool own = !memcmp(frame + 8, address, 16);
    if (frame[6] == 17) return own;
    /* Audit 08: ICMPv6 is not harmless. lwIP's nd6_input acts on what it says: an NA naming a member's address marks that
     * address duplicated and its socket goes dead, an NS or NA rewrites the neighbour entry of the address it speaks for, and an
     * RA, redirect or packet-too-big changes addresses, routes or the MTU. Only neighbour unreachability detection crosses the
     * network here (ZeroTier answers address resolution locally): a unicast NS from the sender's link-local address, answered by
     * an NA from its 6plane address. So a member may speak for itself and for nothing else. */
    if (frame[6] != 58 || length < 64 || (!own && !(frame[8] == 0xfe && (frame[9] & 0xc0) == 0x80))) return false;
    return frame[40] == 135 || (frame[40] == 136 && !memcmp(frame + 48, address, 16));
}
