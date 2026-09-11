#!/usr/bin/env python3
# PORT: port-written for angband-pico, stage 070. Save the board's serial output to a file
# while it is happening.
#
#   python angband-pico/tools/serial-capture.py                 # auto-find the port
#   python angband-pico/tools/serial-capture.py COM7 run1.txt
#
# It writes every line to the file AND to the screen, flushing after each one, so a capture
# survives a crash, a reset, an unplugged cable and a closed window.
#
# WHY THIS EXISTS
#
# "A conclusion without its capture is not a record." Stage 045 lost a key log, stage 050
# lost the level 50 and 98 heap lines, and both cost an acceptance criterion -- in each case
# to a terminal buffer that nobody saved while it was still open. Stage 070 asks for at
# least 1,000 logged player turns pasted into its run log, which is about 100 KB of serial
# that cannot be re-taken without another 4-minute boot and an hour of play.
#
# NEVER OPEN THE PORT AT 1200 BAUD. That resets an RP2350 into BOOTSEL and the game is gone.
# This script refuses any baud rate but 115200 for that reason.
#
# The tool only reads. Keys are typed on the PicoCalc's own keyboard; there is no serial
# input path into the game.

import io
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("serial-capture: pyserial is not installed --  py -m pip install pyserial")
    sys.exit(2)

BAUD = 115200


def find_port():
    """The board enumerates as a USB CDC device. Prefer one that names the RP2350."""
    candidates = list(list_ports.comports())
    for p in candidates:
        text = "%s %s %s" % (p.description, p.manufacturer, p.hwid)
        if "2E8A" in text.upper() or "RP2" in text.upper() or "Pico" in text:
            return p.device
    # Fall back to the only non-Bluetooth port, if there is exactly one.
    plain = [p for p in candidates if "Bluetooth" not in (p.description or "")]
    if len(plain) == 1:
        return plain[0].device
    print("serial-capture: could not pick a port. Candidates:")
    for p in candidates:
        print("    %-8s %s" % (p.device, p.description))
    return None


def main(argv):
    port = argv[1] if len(argv) > 1 else find_port()
    if not port:
        return 2

    out = argv[2] if len(argv) > 2 else time.strftime("angband-%Y%m%d-%H%M%S.txt")

    print("serial-capture: %s at %d -> %s   (Ctrl-C to stop)" % (port, BAUD, out))

    # dsrdtr/rtscts left at their defaults: the SDK's CDC stack ignores the control lines,
    # and a host that drops DTR on some drivers can make the first write block.
    with serial.Serial(port, BAUD, timeout=1) as ser, \
            io.open(out, "a", encoding="utf-8", newline="\n") as f:
        f.write("# serial-capture %s %s\n" % (port, time.strftime("%Y-%m-%d %H:%M:%S")))
        f.flush()
        pending = b""
        try:
            while True:
                chunk = ser.read(4096)
                if not chunk:
                    continue
                pending += chunk
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    text = line.decode("utf-8", "replace").rstrip("\r")
                    print(text)
                    # Flush every line. The whole point is that the file is complete at
                    # every instant, including the instant the board hard-faults.
                    f.write(text + "\n")
                    f.flush()
        except KeyboardInterrupt:
            if pending:
                f.write(pending.decode("utf-8", "replace") + "\n")
                f.flush()
            print("\nserial-capture: stopped, wrote %s" % out)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
