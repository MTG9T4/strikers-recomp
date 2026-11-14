"""Reproducible, checked SDK adaptations; deps/libzt remains untouched.

The pinned ZeroTierOne peer structure includes bonding fields and 64 paths;
libzt 1.8.10's public structure has 16 paths. Raw memcpy is not an ABI adapter.
"""
from pathlib import Path
import hashlib
import json


def adapt(root, folder):
    root, folder = Path(root), Path(folder)
    original = root / 'src/NodeService.cpp'
    data = original.read_text()
    old = '''            memcpy(pd, peer, sizeof(zts_peer_info_t));
            for (unsigned int j = 0; j < peer->pathCount; j++) {
                native_ss_to_zts_ss(&(pd->paths[j].address), &(peer->paths[j].address));
            }'''
    new = '''            pd->peer_id = peer->address;
            pd->ver_major = peer->versionMajor;
            pd->ver_minor = peer->versionMinor;
            pd->ver_rev = peer->versionRev;
            pd->latency = peer->latency;
            pd->role = (zts_peer_role_t)peer->role;
            pd->path_count = std::min(peer->pathCount, (unsigned int)ZTS_MAX_PEER_NETWORK_PATHS);
            for (unsigned int j = 0; j < pd->path_count; j++) {
                native_ss_to_zts_ss(&(pd->paths[j].address), &(peer->paths[j].address));
                pd->paths[j].last_tx = peer->paths[j].lastSend;
                pd->paths[j].last_rx = peer->paths[j].lastReceive;
                pd->paths[j].trusted_path_id = peer->paths[j].trustedPathId;
                pd->paths[j].expired = peer->paths[j].expired;
                pd->paths[j].preferred = peer->paths[j].preferred;
            }'''
    assert data.count(old) == 1, 'pinned peer ABI adapter drift'
    data = data.replace(old, new)
    # Refresh latency even when the number of paths has not changed. Audit 05: at most once a second. The service loop runs
    # generateSyntheticEvents on every pass (every UDP packet the node receives and every timer), and an event per peer on
    # every pass was a heap-allocated copy, a queue entry and a pass of the event thread and the game's handler for each.
    marker = '    ZT_PeerList* pl = _node->peers();'
    assert data.count(marker) == 1, 'pinned peer list anchor drift'
    data = data.replace(marker, '''    // Strikers: user-visible peer metrics are refreshed at most once a second, not on every service-loop pass.
    static int64_t lastPeerMetrics = 0;
    const int64_t peerMetricsNow = OSUtils::now();
    const bool peerMetrics = peerMetricsNow - lastPeerMetrics >= 1000;
    if (peerMetrics) {
        lastPeerMetrics = peerMetricsNow;
    }
''' + marker)
    marker = '            // Update our cache with most recently observed path count'
    assert data.count(marker) == 1
    data = data.replace(marker, '''            if (peerMetrics) {
                sendEventToUser(pl->peers[i].pathCount ? ZTS_EVENT_PEER_DIRECT : ZTS_EVENT_PEER_RELAY,
                                (void*)&pl->peers[i]);
            }
''' + marker)
    marker = '''    ZTS_UNUSED_ARG(localSocket);
    // Make sure we're not trying to do ZeroTier-over-ZeroTier'''
    assert data.count(marker) == 1, 'pinned path policy adapter drift'
    data = data.replace(marker, '''    // Opt-in test simulator: reject direct paths to these exact node IDs.
    // Planet/root paths and relay delivery remain intact. No OS firewall changes.
    const char *blocked = getenv("STRIKERS_ZT_BLOCK_DIRECT");
    if (blocked) {
        std::string list(blocked);
        size_t pos = 0;
        while (pos < list.size()) {
            size_t end = list.find(',', pos);
            std::string id = list.substr(pos, end == std::string::npos ? end : end-pos);
            if (id.size() == 10 && strtoull(id.c_str(), NULL, 16) == ztaddr) {
                static unsigned logged = 0;
                if (logged++ < 8) printf("[zt-path-sim] rejected direct path node=%010llx\\n", (unsigned long long)ztaddr);
                return 0;
            }
            if (end == std::string::npos) break;
            pos = end + 1;
        }
    }
''' + marker)
    data = data.replace('#include "../version.h"', '#include "' + (root / 'ext/ZeroTierOne/version.h').as_posix() + '"')
    data = '/* Modified for Strikers: bounded peer ABI conversion, refreshed metrics, optional direct-path test policy. */\n' + data
    target = folder / 'NodeService.cpp'
    if not target.exists() or target.read_text() != data:
        target.write_text(data)
    record = {
        'input': str(original), 'input_sha256': hashlib.sha256(original.read_bytes()).hexdigest(),
        'output_sha256': hashlib.sha256(target.read_bytes()).hexdigest(),
        'changes': ['field-wise bounded peer conversion', 'periodic peer metrics (at most once a second)', 'opt-in exact-node direct-path rejection',
                    'IPv6 input on tcpip mailbox; disable synchronous core input to prevent recursive NDP delivery']
    }
    tap = (root / 'src/VirtualTap.cpp').read_text()
    old = 'zts_netif_init6, ethernet_input);'
    assert tap.count(old) == 1, 'pinned IPv6 input adapter drift'
    tap = tap.replace(old, 'zts_netif_init6, tcpip_input);')
    tap = '/* Modified for Strikers: route IPv6 ingress through the lwIP TCP/IP mailbox. */\n' + tap
    opts = (root / 'src/lwipopts.h').read_text()
    old = '#define LWIP_TCPIP_CORE_LOCKING_INPUT   1'
    assert opts.count(old) == 1, 'pinned tcpip input configuration drift'
    opts = opts.replace(old, '#define LWIP_TCPIP_CORE_LOCKING_INPUT   0')
    opts = '/* Modified for Strikers: queued input prevents synchronous neighbor-discovery reentry. */\n' + opts
    for name, contents in [('VirtualTap.cpp', tap), ('lwipopts.h', opts)]:
        path = folder / name
        if not path.exists() or path.read_text() != contents:
            path.write_text(contents)
    # Authenticate group membership before a frame can enter lwIP. Never trust
    # the virtual source IP alone: ad-hoc's default rules permit spoofing it.
    original_network = root / 'ext/ZeroTierOne/node/Network.cpp'
    network = original_network.read_text()
    anchor = '''const unsigned int vlanId)
{
\tAddress ztFinalDest(ztDest);'''
    assert network.count(anchor) == 1, 'pinned authenticated frame boundary drift'
    network = network.replace(anchor, '''const unsigned int vlanId)
{
    if (!strikers_group_frame(_id, sourcePeer->address().toInt(), etherType, frameData, frameLen)) return 0;
\tAddress ztFinalDest(ztDest);''')
    policy = root.parents[1] / 'runtime/services/zt_group_filter.h'
    network = '/* Modified for Strikers: authenticated group filter at the start of filterIncomingPacket. */\n#include "' + policy.as_posix() + '"\n' + network
    network = network.replace('#include "../version.h"', '#include "' + (root / 'ext/ZeroTierOne/version.h').as_posix() + '"')
    target_network = folder / 'Network.cpp'
    if not target_network.exists() or target_network.read_text() != network:
        target_network.write_text(network)
    record['changes'].append('authenticated source-node group filter before lwIP; bind ad-hoc UDP source to 6plane identity')
    record['group_filter_sha256'] = hashlib.sha256(policy.read_bytes()).hexdigest()
    record['network_source_sha256'] = hashlib.sha256(original_network.read_bytes()).hexdigest()
    record['network_generated_sha256'] = hashlib.sha256(target_network.read_bytes()).hexdigest()
    record['files'] = [{
        'source': (root / 'src' / name).as_posix(),
        'source_sha256': hashlib.sha256((root / 'src' / name).read_bytes()).hexdigest(),
        'generated_sha256': hashlib.sha256((folder / name).read_bytes()).hexdigest(),
    } for name in ('NodeService.cpp', 'VirtualTap.cpp', 'lwipopts.h')]
    (folder / 'adaptations.json').write_text(json.dumps(record, indent=2) + '\n')
    return target
