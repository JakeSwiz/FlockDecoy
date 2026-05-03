#!/bin/bash
# Send a single CLI command to the C5 and read responses for N seconds.
#
# Usage: c5-cmd.sh "<command>" [seconds=8]
# Example: c5-cmd.sh "sniffflockwifi -b 2g" 12
#
# Holds DTR/RTS low to avoid the auto-reset circuit. Read-only to stdout —
# returns when the timeout expires. Useful as a probe to see if the C5 is
# responsive without going through the Flipper FAP path.

CMD="${1:?need command (e.g. \"stopscan\" or \"sniffflockwifi -b 2g\")}"
SECS="${2:-8}"
PORT="${PORT:-/dev/cu.usbmodem5B7B0330131}"
BAUD="${BAUD:-115200}"

if [ ! -e "$PORT" ]; then
    echo "Port $PORT not found." >&2
    exit 1
fi

PYTHONPATH=/Users/xyconix/.local/pipx/venvs/esptool/lib/python3.14/site-packages \
exec python3 -c "
import serial, time, sys
s = serial.Serial(port=None, baudrate=$BAUD, timeout=0.5)
s.port = '$PORT'
s.dtr = False
s.rts = False
s.open()
time.sleep(0.2)
s.reset_input_buffer()
s.write(b'$CMD\r\n')
print(f'>>> sent: $CMD', file=sys.stderr)
end = time.time() + $SECS
got_anything = False
while time.time() < end:
    line = s.readline()
    if line:
        got_anything = True
        try:
            sys.stdout.write(line.decode('utf-8','replace'))
            sys.stdout.flush()
        except: pass
if not got_anything:
    print('>>> (silent — no response in $SECS seconds)', file=sys.stderr)
s.close()
"
