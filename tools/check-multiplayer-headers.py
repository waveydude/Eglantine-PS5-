#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check active Ninja objects agree on the derived multiplayer class declarations."""
import pathlib
import subprocess
import sys

build = pathlib.Path(sys.argv[1])
def ninja(*args):
    return subprocess.run(['ninja', '-C', str(build), '-t', *args],
                          capture_output=True, text=True, check=True).stdout

active = {line.split(': ', 1)[0] for line in ninja('targets', 'all').splitlines()}
headers = ('network/packet.h', 'network/room_member.h',
           'core/internal_network/sockets.h', 'core/internal_network/socket_proxy.h',
           'core/hle/service/sockets/bsd.h')
# headless/multiplayer_packets.cmake rewrites socket_proxy.h / bsd.h only for cores that do not harden
# the room sockets themselves (no AcceptsProxyPacket); newer cores keep their own, and objects then
# include the originals by design. Same probe as the CMake file.
proxy = build.parent / 'source/src/core/internal_network/socket_proxy.h'
if proxy.is_file() and 'AcceptsProxyPacket' in proxy.read_text():
    headers = headers[:3]
seen = dict.fromkeys(headers, 0)
wrong, target = [], None
for line in ninja('deps').splitlines():
    if line and not line.startswith(' '):
        target = line.split(': ', 1)[0]
        continue
    if target not in active:
        continue
    path = line.strip().replace('\\', '/')
    for header in headers:
        if path.endswith('/' + header):
            prefix = '/headless/multiplayer/' + ('socket-headers/' if header.startswith('core/') else '')
            if path.endswith(prefix + header):
                seen[header] += 1
            else:
                wrong.append(f'{target}: {path}')
missing = [header for header, count in seen.items() if not count]
if wrong or missing:
    print('\n'.join(wrong[:20]))
    sys.exit(f'Multiplayer headers: {len(wrong)} original-header dependencies, missing {missing}')
print(f'Multiplayer derived headers: {seen} PASS')
