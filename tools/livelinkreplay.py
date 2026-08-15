#!/usr/bin/env python3
"""Sends synthetic Live Link Face packets so the engine's receiver, calibration
and recording can be exercised without an iPhone.

Standard library only - no pip install, nothing to download.

  py -3 tools/livelinkreplay.py                 # animated face on 127.0.0.1:11111
  py -3 tools/livelinkreplay.py --rest          # hold a resting face (calibration)
  py -3 tools/livelinkreplay.py --host 1.2.3.4  # another machine
  py -3 tools/livelinkreplay.py --dump take.bin # replay captured packets
  py -3 tools/livelinkreplay.py --capture take.bin   # record a real phone's packets

The resting face is deliberately NOT all zeros: a real one never is, which is
what neutral calibration exists to remove.
"""

import argparse
import math
import socket
import struct
import sys
import time

# Apple's blendshape order, which is the packet order. Values 52-60 are head
# yaw/pitch/roll then left/right eye yaw/pitch/roll.
SHAPES = [
    "eyeBlinkLeft", "eyeLookDownLeft", "eyeLookInLeft", "eyeLookOutLeft",
    "eyeLookUpLeft", "eyeSquintLeft", "eyeWideLeft",
    "eyeBlinkRight", "eyeLookDownRight", "eyeLookInRight", "eyeLookOutRight",
    "eyeLookUpRight", "eyeSquintRight", "eyeWideRight",
    "jawForward", "jawLeft", "jawRight", "jawOpen",
    "mouthClose", "mouthFunnel", "mouthPucker", "mouthLeft", "mouthRight",
    "mouthSmileLeft", "mouthSmileRight", "mouthFrownLeft", "mouthFrownRight",
    "mouthDimpleLeft", "mouthDimpleRight", "mouthStretchLeft", "mouthStretchRight",
    "mouthRollLower", "mouthRollUpper", "mouthShrugLower", "mouthShrugUpper",
    "mouthPressLeft", "mouthPressRight", "mouthLowerDownLeft", "mouthLowerDownRight",
    "mouthUpperUpLeft", "mouthUpperUpRight",
    "browDownLeft", "browDownRight", "browInnerUp", "browOuterUpLeft", "browOuterUpRight",
    "cheekPuff", "cheekSquintLeft", "cheekSquintRight",
    "noseSneerLeft", "noseSneerRight", "tongueOut",
]
VALUE_COUNT = 61
INDEX = {name: i for i, name in enumerate(SHAPES)}

# A resting face reads slightly expressive to the tracker, so the replay does too.
REST = {"browInnerUp": 0.08, "mouthPressLeft": 0.05, "mouthPressRight": 0.05,
        "eyeSquintLeft": 0.04, "eyeSquintRight": 0.04}


def encode(values, subject="Replay", frame=0, fps=60):
    """Build one packet: version is LITTLE endian, everything after is BIG."""
    name = subject.encode("utf-8")
    out = bytearray()
    out += struct.pack("<i", 6)                 # version
    out += b"A" * 37                            # device uuid
    out += struct.pack("!i", len(name))
    out += name
    out += struct.pack("!if2ib", frame, 0.0, fps, 1, VALUE_COUNT)
    out += struct.pack("!%df" % VALUE_COUNT, *values)
    return bytes(out)


def frame_values(t, rest_only):
    values = [0.0] * VALUE_COUNT
    for name, amount in REST.items():
        values[INDEX[name]] = amount
    if rest_only:
        return values

    def add(name, amount):
        values[INDEX[name]] = max(0.0, min(1.0, values[INDEX[name]] + amount))

    # Speech-ish jaw with rounding on the slower part of the cycle.
    speech = max(0.0, math.sin(t * 7.0)) * (0.55 + 0.35 * math.sin(t * 1.7))
    add("jawOpen", speech)
    add("mouthFunnel", max(0.0, math.sin(t * 3.1)) * 0.3)
    add("mouthPucker", max(0.0, math.sin(t * 2.3 + 1.0)) * 0.4)

    # A smile that comes and goes, slightly asymmetric like a real one.
    smile = max(0.0, math.sin(t * 0.6)) * 0.7
    add("mouthSmileLeft", smile)
    add("mouthSmileRight", smile * 0.85)

    # Brows drifting, and a blink every ~3.5s.
    brow = (math.sin(t * 0.9) * 0.5 + 0.5) * 0.5
    add("browInnerUp", brow)
    add("browOuterUpLeft", brow * 0.7)
    add("browOuterUpRight", brow * 0.7)

    phase = t % 3.5
    if phase < 0.12:
        closed = 1.0 - abs(phase - 0.06) / 0.06
        add("eyeBlinkLeft", closed)
        add("eyeBlinkRight", closed)

    # Eyes tracking left/right, in degrees like the real app sends.
    values[55] = values[58] = math.sin(t * 0.8) * 15.0   # yaw
    values[56] = values[59] = math.sin(t * 0.5) * 6.0    # pitch
    return values


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=11111)
    ap.add_argument("--fps", type=float, default=60.0)
    ap.add_argument("--rest", action="store_true", help="hold a resting face")
    ap.add_argument("--seconds", type=float, default=0.0, help="0 = until Ctrl-C")
    ap.add_argument("--subject", default="Replay")
    ap.add_argument("--dump", help="replay packets previously captured with --capture")
    ap.add_argument("--capture", help="listen on --port and record packets to this file")
    args = ap.parse_args()

    if args.capture:
        listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        listener.bind(("0.0.0.0", args.port))
        print("capturing on port %d, Ctrl-C to stop" % args.port)
        count = 0
        with open(args.capture, "wb") as handle:
            try:
                while True:
                    data, _ = listener.recvfrom(4096)
                    handle.write(struct.pack("<I", len(data)))
                    handle.write(data)
                    count += 1
                    if count % 60 == 0:
                        print("\r%d packets" % count, end="", flush=True)
            except KeyboardInterrupt:
                pass
        print("\nwrote %s (%d packets)" % (args.capture, count))
        return

    sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    target = (args.host, args.port)
    period = 1.0 / max(1.0, args.fps)

    packets = None
    if args.dump:
        packets = []
        with open(args.dump, "rb") as handle:
            while True:
                header = handle.read(4)
                if len(header) < 4:
                    break
                packets.append(handle.read(struct.unpack("<I", header)[0]))
        if not packets:
            sys.exit("%s holds no packets" % args.dump)
        print("replaying %d captured packets to %s:%d" % (len(packets), args.host, args.port))
    else:
        print("sending %s face to %s:%d at %g Hz (Ctrl-C to stop)"
              % ("resting" if args.rest else "animated", args.host, args.port, args.fps))

    start = time.time()
    frame = 0
    try:
        while True:
            now = time.time() - start
            if args.seconds and now >= args.seconds:
                break
            if packets:
                sender.sendto(packets[frame % len(packets)], target)
            else:
                sender.sendto(encode(frame_values(now, args.rest), args.subject,
                                     frame, int(args.fps)), target)
            frame += 1
            if frame % 60 == 0:
                print("\r%d packets" % frame, end="", flush=True)
            time.sleep(period)
    except KeyboardInterrupt:
        pass
    print("\nsent %d packets" % frame)


if __name__ == "__main__":
    main()
