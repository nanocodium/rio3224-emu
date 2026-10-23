"""bk3-mkcap.py <out.bin> — synthesize a Brooklyn III module configuration ("capability" partition) for the
Rio3224-D2 Dante module as the device would carry it from the factory.

Layout from dac's DWARF (fw/d2/bk3/qemu/module_config_types.txt): all multi-byte fields are BIG endian,
offsets are relative to the start of the buffer; mc_buffer_read_binary only checks version >= 2 and length.
"""
import struct, sys

# enumerations (module_config / dac tables)
UART_MODE_NONE, UART_MODE_UNATTACHED, UART_MODE_CONSOLE, UART_MODE_BRIDGE, UART_MODE_METER, UART_MODE_HOST_CPU, UART_MODE_HAREMOTE = 0, 1, 2, 3, 4, 5, 6
UART_PARITY_NONE = 2
SPI_MODE_MASTER, SPI_MODE_SLAVE = 1, 2
AUDIO_MCLK_12M, AUDIO_FRAME_LRCLK, AUDIO_ALIGN_INS, AUDIO_START_FALLING = 2, 0, 0, 0
ENC_PCM_16, ENC_PCM_24, ENC_PCM_32 = 2, 1, 4
CLOCK_TYPE_VCXO = 1
# Yamaha manufacturer capability bits (ymhinit)
YMH_HAREMOTE, YMH_HAREMOTE_ACT, YMH_HAREMOTE_RST, YMH_SPIETHER_ACT, YMH_DAC_ACT = 0x1, 0x2, 0x4, 0x8, 0x10
YMH_HAREMOTE_ETHER, YMH_SPIETHER, YMH_DAC, YMH_DAC_ETHER = 0x400, 0x800, 0x1000, 0x2000

def be16(v): return struct.pack('>H', v)
def be32(v): return struct.pack('>I', v)
def fixed(s, n): b = s.encode(); assert len(b) <= n; return b + b'\0' * (n - len(b))

def build(uart0_mode=UART_MODE_BRIDGE, uart1_mode=UART_MODE_HAREMOTE, caps=YMH_DAC | YMH_HAREMOTE | YMH_SPIETHER):
    buf = bytearray()
    # ---- module_config (20) ----
    buf += fixed('AUDMCFG', 8) + bytes([2, 0]) + be16(0)          # sentinel, version 2.0, length (patched)
    buf += be16(0) + be16(0) + be32(0)                             # uboot_config_offset, app_config_offset, crc
    # ---- uboot_config (668) ----
    uboot_off = len(buf)
    u = bytearray()
    u += be16(0) + bytes([0, 0])                                   # pin_config, pin_aux, pin_mode
    u += bytes([SPI_MODE_SLAVE, 0, 0, 0]) + bytes([SPI_MODE_MASTER, 0, 0, 0])   # spi 0 (host pipe, slave), spi 1
    def uart(mode, speed): return bytes([mode, 8, 1, UART_PARITY_NONE]) + be32(speed) + bytes([0, 0]) + be16(1)
    u += uart(uart0_mode, 115200) + uart(uart1_mode, 38400) + uart(UART_MODE_NONE, 115200)
    eth = bytearray()
    eth += bytes([1, 0x03, 1, 0])                                  # eth_default_mode switched, switch_modes (switched|redundant), static ip ok
    eth += be32(0) + be32(0)                                       # vlan_config[2]
    eth += bytes([0, 0]) + bytes([1, 0]) + bytes([0, 0, 0]) + bytes([0])   # control_port[2], gigabit_preferred, support_stp, pad, num_vlan_configs
    eth += be32(0) * 16                                            # multicast_address_list
    eth += be32(0) * 2 + be32(0) * 2                               # vlan_config_2, control_port_2
    eth += b'\0' * 512                                             # vlan_name[4][128]
    eth += be32(0) * 2                                             # control_port_1
    assert len(eth) == 620, len(eth)
    u += eth
    assert len(u) == 668, len(u)
    buf += u
    # ---- app_config (32) ----
    app_off = len(buf)
    buf += b'\0' * 32                                              # filled below
    # ---- audio_config (20) + srates (4 x 16) + encodings (3 x 2) ----
    audio_off = len(buf)
    srates = [(44100, 24, 32), (48000, 24, 32), (88200, 24, 32), (96000, 24, 32)]   # rate, rx (to the Rio outputs), tx (Rio inputs)
    encs = [ENC_PCM_16, ENC_PCM_24, ENC_PCM_32]
    srate_off = audio_off + 20
    enc_off = srate_off + 16 * len(srates)
    a = bytearray()
    a += bytes([1, AUDIO_FRAME_LRCLK, AUDIO_ALIGN_INS, AUDIO_START_FALLING])      # dynamic_srate_change, frame, align, start
    a += be32(48000) + be16(ENC_PCM_24) + bytes([len(srates), len(encs)])
    a += be16(srate_off) + be16(enc_off) + be16(0)                 # srate_configs_offset, encodings_offset, chan_enc_offset
    a += bytes([0, 0])                                             # runtime_enc_change, boot_muted
    assert len(a) == 20
    buf += a
    for rate, rx, tx in srates:
        buf += be32(rate) + be16(rx) + be16(tx) + bytes([AUDIO_MCLK_12M, 8]) + be16(0) + be32(0)
    for e in encs: buf += be16(e)
    while len(buf) % 4: buf += b'\0'
    # ---- clock_config (8) ----
    clock_off = len(buf)
    buf += bytes([1, 1, 0, CLOCK_TYPE_VCXO, 0, 0, 0, 0])            # ext_wc, support_pullups, syncmode, type, restrict_ext_wc, logging, unmute_offset, auto_wc
    # ---- hmi_config (12) ----
    hmi_off = len(buf)
    buf += be32(0) + be32(0) + bytes([0, 0]) + be16(0)
    # ---- apec (4) ----
    apec_off = len(buf)
    buf += be16(4) + bytes([0, 0])
    # ---- device_config (8) ----
    dev_off = len(buf)
    buf += be32(0) + be32(0)                                       # wide_area_support, reset_flags
    # ---- manf_version_config (336) ----
    manf_off = len(buf)
    m = bytearray()
    m += bytes([1, 0]) + be16(0)                                   # override
    m += fixed('YAMAHA', 8) + fixed('RIO3224D', 8)                 # manufacturer_id, model_id (id64)
    m += fixed('Yamaha Corporation', 128) + fixed('Rio3224-D2', 128)
    m += bytes([4, 2]) + be16(5) + be32(3)                         # sw_version 4.2.5 build 3
    m += bytes([4, 2]) + be16(5) + be32(3)                         # fw_version
    m += bytes([1, 0]) + be16(0) + be32(0)                         # model_version 1.0.0
    m += be32(caps)                                                # manufacturer capabilities (YMH_*)
    m += fixed('1.0.0', 32)
    assert len(m) == 336, len(m)
    buf += m
    # ---- fill app_config ----
    app = bytearray()
    app += fixed('Rio3224D', 8)                                    # device_prefix
    app += be16(0) + be16(0)                                       # num_rxlabels, num_txlabels
    app += be16(audio_off) + be16(clock_off) + be16(0) + be16(0)   # audio, clock, rxlabels, txlabels offsets
    app += be16(hmi_off) + be16(apec_off)
    app += bytes([1, 1, 1, 0])                                     # support_identify, support_baseboard, custom_metering, pad
    app += be16(manf_off) + be16(dev_off)
    assert len(app) == 32
    buf[app_off:app_off + 32] = app
    # ---- header offsets / length ----
    buf[10:12] = be16(len(buf))
    buf[12:14] = be16(uboot_off)
    buf[14:16] = be16(app_off)
    return bytes(buf)

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else 'capability.bin'
    b = build()
    open(out, 'wb').write(b)
    print(f'wrote {out}: {len(b)} bytes')
