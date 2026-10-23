#!/usr/bin/env python3
"""dante-pcap-ptp.py FILE.pcap [MAX] : decode the PTPv1 (IEEE 1588-2002) messages in an endpoint trace: Sync / Follow_Up /
Delay_Req / Delay_Resp with sub-domain, source uuid/port, sequence, flags, grandmaster data set (uuid, stratum,
identifier, variance, preferred), sync interval and local clock fields.  Shows how other devices (Dante hardware,
Dante Virtual Soundcard) fill the fields so the emulator's clock can be compared with them."""
import struct, sys
sys.path.insert(0, __import__('os').path.dirname(__file__))
from importlib import import_module
pcap = import_module('dante-pcap-summary')

CTRL = {0: 'Sync', 1: 'Delay_Req', 2: 'Follow_Up', 3: 'Delay_Resp', 4: 'Management'}
def ts(b): s, ns = struct.unpack('>II', b[:8]); return '%d.%09d' % (s, ns)

def main():
    maxn = int(sys.argv[2]) if len(sys.argv) > 2 else 40; n = 0; seen = {}
    for t, ip in pcap.pcap_packets(sys.argv[1]):
        if len(ip) < 28 or ip[9] != 17: continue
        ihl = (ip[0] & 15) * 4; src = '.'.join(map(str, ip[12:16])); udp = ip[ihl:]; sp, dp = struct.unpack('>HH', udp[:4]); p = udp[8:]
        if dp not in (319, 320) or len(p) < 40 or struct.unpack('>H', p[:2])[0] != 1: continue
        sub = p[4:20].split(b'\0')[0].decode('ascii', 'replace'); mtype = p[20]; uuid = ':'.join('%02x' % x for x in p[22:28]); port = struct.unpack('>H', p[28:30])[0]; seq = struct.unpack('>H', p[30:32])[0]; ctrl = p[32]; flags = struct.unpack('>H', p[34:36])[0]
        key = (uuid, ctrl)
        if seen.get(key, 0) >= 2: continue
        seen[key] = seen.get(key, 0) + 1; n += 1
        line = '%.3f %s:%d -> :%d %-10s sub "%s" type %d uuid %s port %d seq %d flags 0x%04x len %d' % (t, src, sp, dp, CTRL.get(ctrl, ctrl), sub, mtype, uuid, port, seq, flags, len(p))
        b = p[40:]
        if ctrl in (0, 1) and len(b) >= 84:
            gm = b[12:40]
            line += '\n     origin %s epoch %d utcoff %d | GM: tech %d uuid %s port %d seq %d stratum %d id "%s" var %d pref %d boundary %d | syncInt %d localVar %d steps %d localStratum %d localId "%s" parent %s/%d estVar %d drift %d utcReasonable %d' % (
                ts(b[0:8]), struct.unpack('>H', b[8:10])[0], struct.unpack('>h', b[10:12])[0], gm[1], ':'.join('%02x' % x for x in gm[2:8]), struct.unpack('>H', gm[8:10])[0], struct.unpack('>H', gm[10:12])[0], gm[15], gm[16:20].decode('ascii', 'replace'), struct.unpack('>h', gm[22:24])[0], gm[25], gm[27],
                struct.unpack('>b', b[43:44])[0], struct.unpack('>h', b[46:48])[0], struct.unpack('>H', b[50:52])[0], b[55], b[56:60].decode('ascii', 'replace'), ':'.join('%02x' % x for x in b[62:68]), struct.unpack('>H', b[70:72])[0], struct.unpack('>h', b[74:76])[0], struct.unpack('>i', b[76:80])[0], b[83])
        elif ctrl == 2 and len(b) >= 12: line += '\n     assoc seq %d precise origin %s' % (struct.unpack('>H', b[2:4])[0], ts(b[4:12]))
        elif ctrl == 3 and len(b) >= 20: line += '\n     receipt %s requesting uuid %s port %d seq %d' % (ts(b[0:8]), ':'.join('%02x' % x for x in b[10:16]), struct.unpack('>H', b[16:18])[0], struct.unpack('>H', b[18:20])[0])
        print(line)
        if n >= maxn: break

if __name__ == '__main__': main()
