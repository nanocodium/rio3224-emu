#!/usr/bin/env python3
"""flash-set.py IN.bin OUT.bin OP... : edit the firmware's saved settings in a flash image the way the unit itself stores
them.  The parameter store at 0x700000 is a log of records {c1, seq, ?, ?, be32 size, be32 version 6, ff fe, be16 check}
+ serialized MMSX banks (check = two's complement of the 16-bit byte sum of the payload); the firmware appends a new record
with seq+1 whenever it saves (power-off sequence) and restores the newest one at boot.  This tool edits the NEWEST record.
  startup=N        SETUP Dev/UI/StartupMode (0 REFRESH w/o MUTE, 1 REFRESH = factory, 2 RESUME)   byte +0x360
  unitid=N         SETUP Dev/UI/UnitId                                                             byte +0x35F
  48v=CH:0|1       NonVolatile InputChannel[CH].HeadAmp/48V            (CH 1-32)
  gain=CH:dB       HeadAmp/Gain in dB (-6..66; stored as int16 of 0.01 dB)
  gcon=CH:0|1      GainCompensation/On
  gcstart=CH:dB    GainCompensation/StartGain (dB)
  hpfon=CH:0|1     HPF/On
  hpffreq=CH:Hz    HPF/Frequency (Hz, stored as uint32 of 0.1 Hz)
  all:OP           apply OP to all 32 channels, e.g. all:gain=20
"""
import sys, struct
REGION = 0x700000; REGION_LEN = 0x80000
SETUP_VALUES = 0x35C             # LastScreenType, PanelLock, FanSpeed, UnitId, StartupMode, SwitchMode, NetworkMode, Dimmer, ErrorMessage, Brightness, IP[4], Mask[4], GW[4]
NV_VALUES = 0x580                # 32 x { 48V u8, Gain s16, GCOn u8, StartGain s16, HPFOn u8, Freq u32 } = 11 bytes
NV_STRIDE = 11

def records(d):
    out = []; off = REGION
    while off + 16 <= REGION + REGION_LEN:
        if d[off] == 0xC1 and d[off + 12] == 0xFF and struct.unpack_from('>I', d, off + 8)[0] == 6:
            size = struct.unpack_from('>I', d, off + 4)[0]
            if size > 0x600: out.append((off, size, d[off + 1]))
            off += max(16, size)
        else: off += 2
    return out

def fix_check(d, rec):
    size = struct.unpack_from('>I', d, rec + 4)[0]
    chk = (-sum(d[rec + 0x10:rec + size])) & 0xFFFF
    struct.pack_into('>H', d, rec + 0xE, chk)
    return chk

def apply(d, rec, op):
    if op.startswith('all:'):
        key, val = op[4:].split('=')
        for ch in range(1, 33): apply(d, rec, '%s=%d:%s' % (key, ch, val))
        return
    key, val = op.split('=')
    if key == 'startup': d[rec + SETUP_VALUES + 4] = int(val); return
    if key == 'unitid': d[rec + SETUP_VALUES + 3] = int(val); return
    ch, v = val.split(':'); ch = int(ch); base = rec + NV_VALUES + (ch - 1) * NV_STRIDE
    if key == '48v': d[base] = int(v)
    elif key == 'gain': struct.pack_into('>h', d, base + 1, int(round(float(v) * 100)))
    elif key == 'gcon': d[base + 3] = int(v)
    elif key == 'gcstart': struct.pack_into('>h', d, base + 4, int(round(float(v) * 100)))
    elif key == 'hpfon': d[base + 6] = int(v)
    elif key == 'hpffreq': struct.pack_into('>I', d, base + 7, int(round(float(v) * 10)))
    else: raise SystemExit('unknown op ' + op)

d = bytearray(open(sys.argv[1], 'rb').read())
recs = records(d)
if not recs: raise SystemExit('no parameter-store record found at 0x700000')
rec = max(recs, key=lambda r: r[2])[0]           # newest = highest sequence number
for op in sys.argv[3:]: apply(d, rec, op)
chk = fix_check(d, rec)
open(sys.argv[2], 'wb').write(d)
S = rec + SETUP_VALUES; N = rec + NV_VALUES
print('written %s (record @%06x seq %d, check %04x); startup=%d unitid=%d ch1: 48v=%d gain=%.2f gcon=%d gcstart=%.2f hpfon=%d hpf=%.1f Hz' % (
    sys.argv[2], rec, d[rec + 1], chk, d[S + 4], d[S + 3], d[N], struct.unpack_from('>h', d, N + 1)[0] / 100.0,
    d[N + 3], struct.unpack_from('>h', d, N + 4)[0] / 100.0, d[N + 6], struct.unpack_from('>I', d, N + 7)[0] / 10.0))
