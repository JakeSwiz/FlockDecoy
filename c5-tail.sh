#!/bin/bash
# Tail the C5's USB-CDC without resetting the chip.
#
# Holds DTR/RTS low across the read so the chip's auto-reset circuit doesn't
# fire. Read-only — Ctrl-C to exit. (Use pyserial-miniterm in a real terminal
# if you need bidirectional input.)

PORT="${1:-/dev/cu.usbmodem5B7B0330131}"
BAUD="${2:-115200}"

if [ ! -e "$PORT" ]; then
    echo "Port $PORT not found. Currently visible ports:" >&2
    ls /dev/cu.usbmodem* /dev/cu.usbserial-* 2>/dev/null | grep -v flip_ >&2
    exit 1
fi

PYTHONPATH=/Users/xyconix/.local/pipx/venvs/esptool/lib/python3.14/site-packages \
exec python3 -c "
import serial, sys, signal
def stop(*a):
    sys.exit(0)
signal.signal(signal.SIGINT, stop)
s = serial.Serial(port=None, baudrate=$BAUD, timeout=1)
s.port = '$PORT'
s.dtr = False
s.rts = False
s.open()
print('Tailing $PORT @ $BAUD (DTR/RTS held low). Ctrl-C to exit.', file=sys.stderr)
while True:
    line = s.readline()
    if not line: continue
    try:
        sys.stdout.write(line.decode('utf-8','replace'))
        sys.stdout.flush()
    except: pass
"
