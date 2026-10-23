"""tcp-shell.py HOST PORT WAIT CMDFILE [STEP] — connect to a QEMU serial socket after WAIT seconds, send the
commands of CMDFILE one per STEP seconds, print everything received (raw bytes as text)."""
import socket, sys, time, select
host, port, wait, cmdfile = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]), sys.argv[4]
step = float(sys.argv[5]) if len(sys.argv) > 5 else 3.0
out = bytearray()
def pump(s, secs):
    end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select([s], [], [], 0.2)
        if r:
            d = s.recv(4096)
            if not d: return False
            out.extend(d)
    return True
time.sleep(wait)
s = socket.create_connection((host, port))
pump(s, 1.0)
for line in open(cmdfile):
    line = line.rstrip('\n')
    if not line: continue
    s.sendall((line + '\n').encode())
    if not pump(s, step): break
pump(s, 5.0)
sys.stdout.write(out.decode('latin1').replace('\r', ''))
