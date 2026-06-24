#!/usr/bin/env python3
"""Bench-test the feldd 0.16 `led` verb without the full daemon.

Drives the SP-1's 8 LEDs from the host over the CDC config port, one step at a
time (press Enter between steps so you can watch). Auto-finds the config port by
hello-probing each /dev/cu.usbmodem*. Requires: pip3 install pyserial.

    python3 bench_led.py            # interactive walk (press Enter between steps, for a human)
    python3 bench_led.py --probe    # non-interactive: confirm + flash once, then exit (agents / Beat 0)

ix 0-3 = the 4 front TRACK LEDs, ix 4-7 = the 4 SIDE LEDs (the layer row).
"""
import glob, time, json, sys

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pip3 install --break-system-packages pyserial")


def find_cfg_port():
    for p in sorted(glob.glob("/dev/cu.usbmodem*")):
        try:
            s = serial.Serial(p, 115200, timeout=0.5)  # baud ignored on USB CDC
            s.reset_input_buffer()
            s.write(b'{"t":"hello"}\n')
            time.sleep(0.2)
            if b"hello_r" in s.readline():
                return s, p
            s.close()
        except Exception:
            pass
    sys.exit("no feldd config port found (plugged in? in MIDI mode? pyserial installed?)")


def main():
    probe = "--probe" in sys.argv
    s, port = find_cfg_port()
    print(f"config port: {port}\n")

    def led(**kw):
        s.write((json.dumps({"t": "led", **kw}) + "\n").encode())
        time.sleep(0.06)
        print(f"  sent {json.dumps(kw)}   <- {s.readline().decode('utf-8', 'ignore').strip()}")

    if probe:                       # non-interactive: confirm + flash once, no prompts (headless-safe)
        s.reset_input_buffer()
        s.write(b'{"t":"hello"}\n')
        time.sleep(0.25)
        print("hello   <-", s.readline().decode("utf-8", "ignore").strip())
        led(mask=255)
        time.sleep(0.7)
        led(mask=0)
        time.sleep(0.3)
        led(release=True)
        s.close()
        print("\nprobe OK: SP-1 present, led verb responds (firmware + caps in the hello line above).")
        return

    steps = [
        ("ALL 8 ON  (mask 255)  -> every track + side LED lights", dict(mask=255)),
        ("ALL OFF   (mask 0)    -> override WINS: even the lit layer LED goes dark", dict(mask=0)),
        ("track LED 1 only (ix 0, on)", dict(ix=0, on=True)),
        ("+ side LED 4 (ix 7, on)  -> two LEDs, opposite corners", dict(ix=7, on=True)),
        ("walk the 8: each LED in turn", None),  # special-cased below
        ("RELEASE -> LEDs snap back to the normal layer indicator + press feedback", dict(release=True)),
    ]
    for label, frame in steps:
        input(f"[enter] {label}")
        if frame is None:
            for i in range(8):
                led(mask=(1 << i))
                time.sleep(0.35)
        else:
            led(**frame)
        print()
    s.close()
    print("done. Now press a track button / move a fader: normal behavior is back.")


if __name__ == "__main__":
    main()
