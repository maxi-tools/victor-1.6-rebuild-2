#!/usr/bin/env python3
"""
visage_sock_sender — debug sender for the visage_sock cross-bot protocol.

Streams a parametric sweep over the eye-parameter axes to /run/visage.sock,
which is consumed by BehaviorVisageRender (or any other visage_sock
consumer) and rendered as a procedural face.

Usage:
    visage_sock_sender.py --target unix:/run/visage.sock --pattern sine --rate 30 --duration 10

For SSH-to-Vector usage, copy this file to the bot under /data/ and run it there,
or run it on a host that has a Unix-socket bridge to the bot.

Implements visage_sock protocol v1.
See: maxi-tools/maxi-visage Documentation/visage_sock_protocol.md
"""

import argparse
import math
import os
import socket
import struct
import sys
import time
from dataclasses import dataclass
from typing import List, Optional


# --- Protocol constants -----------------------------------------------------

FRAME_MAGIC = 0x47534956  # 'V','I','S','G' LE
EVENT_MAGIC = 0x54564556  # 'V','E','V','T' LE
PROTOCOL_VERSION = 1
EYE_AXES = 25

FLAG_INTERRUPT_RUNNING = 1 << 0
FLAG_SENDER_IS_IDLE    = 1 << 1
FLAG_HANDSHAKE         = 1 << 2
FLAG_SUBSCRIBE_EVENTS  = 1 << 3

# Per-eye axis indices (must match procedural_25 vocabulary).
class Eye:
    EyeCenterX, EyeCenterY = 0, 1
    EyeScaleX, EyeScaleY = 2, 3
    EyeAngle = 4
    LowerInnerRadiusX, LowerInnerRadiusY = 5, 6
    UpperInnerRadiusX, UpperInnerRadiusY = 7, 8
    UpperOuterRadiusX, UpperOuterRadiusY = 9, 10
    LowerOuterRadiusX, LowerOuterRadiusY = 11, 12
    UpperLidY, UpperLidAngle, UpperLidBend = 13, 14, 15
    LowerLidY, LowerLidAngle, LowerLidBend = 16, 17, 18
    Saturation, Lightness = 19, 20
    GlowSize, HotSpotCenterX, HotSpotCenterY, GlowLightness = 21, 22, 23, 24


# --- Frame builder ---------------------------------------------------------

# struct VisageFrame (240 bytes, packed, little-endian)
#   uint32 magic
#   uint16 version
#   uint16 flags
#   uint32 duration_ms
#   uint32 scene_clock_us
#   float face_angle_deg, face_center_x, face_center_y,
#         face_scale_x, face_scale_y, scanline_opacity
#   float left_eye[25]
#   float right_eye[25]
# = 16 (header) + 24 (face) + 200 (eyes) = 240 bytes
FRAME_FORMAT = "<IHHII" + "6f" + "25f" + "25f"
FRAME_SIZE = struct.calcsize(FRAME_FORMAT)
assert FRAME_SIZE == 240, f"FRAME_SIZE={FRAME_SIZE}, expected 240"

EVENT_HEADER_FORMAT = "<IHHII"
EVENT_HEADER_SIZE = struct.calcsize(EVENT_HEADER_FORMAT)


@dataclass
class Frame:
    flags: int = 0
    duration_ms: int = 0
    scene_clock_us: int = 0
    face_angle_deg: float = 0.0
    face_center_x: float = 0.0
    face_center_y: float = 0.0
    face_scale_x: float = 1.0
    face_scale_y: float = 1.0
    scanline_opacity: float = 1.0
    left_eye: List[float] = None
    right_eye: List[float] = None

    def __post_init__(self):
        if self.left_eye is None:
            self.left_eye = neutral_eye()
        if self.right_eye is None:
            self.right_eye = neutral_eye()

    def pack(self) -> bytes:
        if len(self.left_eye) != EYE_AXES or len(self.right_eye) != EYE_AXES:
            raise ValueError(f"eye arrays must be {EYE_AXES} floats")
        return struct.pack(
            FRAME_FORMAT,
            FRAME_MAGIC,
            PROTOCOL_VERSION,
            self.flags,
            self.duration_ms,
            self.scene_clock_us,
            self.face_angle_deg,
            self.face_center_x,
            self.face_center_y,
            self.face_scale_x,
            self.face_scale_y,
            self.scanline_opacity,
            *self.left_eye,
            *self.right_eye,
        )


def neutral_eye() -> List[float]:
    eye = [0.0] * EYE_AXES
    eye[Eye.EyeScaleX] = 1.0
    eye[Eye.EyeScaleY] = 1.0
    return eye


# --- Patterns --------------------------------------------------------------

def pattern_sine_eyecenterx(t: float) -> Frame:
    """EyeCenterX swings ±20 px over a 2-second period; everything else neutral."""
    amp = 20.0
    period_s = 2.0
    x = amp * math.sin(2.0 * math.pi * (t / period_s))
    f = Frame()
    f.left_eye[Eye.EyeCenterX] = x
    f.right_eye[Eye.EyeCenterX] = x
    return f


def pattern_blink_loop(t: float) -> Frame:
    """Eyes close every 2 seconds for 200 ms."""
    period_s = 2.0
    blink_dur_s = 0.2
    phase = t % period_s
    closed = 1.0 if phase < blink_dur_s else 0.0
    f = Frame()
    f.left_eye[Eye.UpperLidY] = closed
    f.right_eye[Eye.UpperLidY] = closed
    f.left_eye[Eye.LowerLidY] = closed
    f.right_eye[Eye.LowerLidY] = closed
    return f


def pattern_eye_dart(t: float) -> Frame:
    """Eyes dart around in a circle every 3 seconds."""
    period_s = 3.0
    radius = 15.0
    phase = 2.0 * math.pi * (t / period_s)
    f = Frame()
    f.left_eye[Eye.EyeCenterX] = radius * math.cos(phase)
    f.left_eye[Eye.EyeCenterY] = radius * math.sin(phase)
    f.right_eye[Eye.EyeCenterX] = radius * math.cos(phase)
    f.right_eye[Eye.EyeCenterY] = radius * math.sin(phase)
    return f


def pattern_neutral(t: float) -> Frame:
    return Frame()


PATTERNS = {
    "sine": pattern_sine_eyecenterx,
    "blink": pattern_blink_loop,
    "dart": pattern_eye_dart,
    "neutral": pattern_neutral,
}


# --- Event reception --------------------------------------------------------

def parse_event(raw: bytes) -> Optional[dict]:
    if len(raw) < EVENT_HEADER_SIZE:
        return None
    magic, version, kind, scene_clock_us, payload_len = struct.unpack(
        EVENT_HEADER_FORMAT, raw[:EVENT_HEADER_SIZE]
    )
    if magic != EVENT_MAGIC:
        return None
    if version != PROTOCOL_VERSION:
        return None
    payload = raw[EVENT_HEADER_SIZE:EVENT_HEADER_SIZE + payload_len]
    return {
        "kind": kind,
        "scene_clock_us": scene_clock_us,
        "payload": payload,
    }


EVENT_NAMES = {
    0x0001: "WakeWordBegin",
    0x0002: "WakeWordEnd",
    0x0003: "ChargerMounted",
    0x0004: "ChargerDismounted",
    0x0005: "CubeTapped",
    0x0006: "BehaviorPreempted",
    0x0007: "BehaviorResumed",
    0x0008: "Distress",
    0x0009: "IdleHint",
    0x000A: "Heartbeat",
}


# --- Main loop --------------------------------------------------------------

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--target", default="unix:/run/visage.sock",
                   help="Unix socket path of the consumer (e.g. unix:/run/visage.sock)")
    p.add_argument("--sender-path", default=None,
                   help="Sender's own socket path (default: /tmp/visage-sender-<pid>.sock)")
    p.add_argument("--pattern", default="sine", choices=sorted(PATTERNS.keys()),
                   help="Pattern to send (sine|blink|dart|neutral)")
    p.add_argument("--rate", type=float, default=30.0, help="Frames per second")
    p.add_argument("--duration", type=float, default=10.0, help="Total seconds; <=0 = forever")
    p.add_argument("--no-interrupt", action="store_true",
                   help="Set FLAG_INTERRUPT_RUNNING=0 (default is 1)")
    p.add_argument("--subscribe-events", action="store_true",
                   help="Request the event back-channel; received events printed to stderr.")
    args = p.parse_args()

    if not args.target.startswith("unix:"):
        sys.exit("--target must be unix:<path>")
    consumer_path = args.target[len("unix:"):]

    sender_path = args.sender_path or f"/tmp/visage-sender-{os.getpid()}.sock"
    try:
        os.unlink(sender_path)
    except FileNotFoundError:
        pass

    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    s.bind(sender_path)
    s.setblocking(False)

    fn = PATTERNS[args.pattern]
    interval_s = 1.0 / args.rate
    period_us = int(interval_s * 1_000_000)
    duration_ms = int(interval_s * 1000)
    deadline = None if args.duration <= 0 else time.monotonic() + args.duration

    flags_first = FLAG_HANDSHAKE
    if not args.no_interrupt:
        flags_first |= FLAG_INTERRUPT_RUNNING
    if args.subscribe_events:
        flags_first |= FLAG_SUBSCRIBE_EVENTS

    flags_steady = FLAG_INTERRUPT_RUNNING if not args.no_interrupt else 0

    t0 = time.monotonic()
    next_send = t0
    sent = 0
    handshake_sent = False
    scene_us = 0

    try:
        while True:
            now = time.monotonic()
            if deadline is not None and now >= deadline:
                break

            if now >= next_send:
                t = now - t0
                frame = fn(t)
                frame.flags = (flags_first if not handshake_sent else flags_steady)
                frame.duration_ms = duration_ms
                frame.scene_clock_us = scene_us
                scene_us += period_us
                try:
                    s.sendto(frame.pack(), consumer_path)
                    sent += 1
                    handshake_sent = True
                except BlockingIOError:
                    # consumer's recv buffer is full; drop and retry next tick
                    pass
                next_send += interval_s

            # Drain incoming events (non-blocking).
            try:
                while True:
                    raw, _ = s.recvfrom(4096)
                    evt = parse_event(raw)
                    if evt is not None:
                        name = EVENT_NAMES.get(evt["kind"], f"Unknown(0x{evt['kind']:04x})")
                        sys.stderr.write(f"[event] {name} scene_us={evt['scene_clock_us']}\\n")
                        sys.stderr.flush()
            except BlockingIOError:
                pass

            # Sleep until next send.
            sleep_for = next_send - time.monotonic()
            if sleep_for > 0:
                time.sleep(min(sleep_for, 0.05))

        # Send one idle frame on clean exit so the consumer falls back to its arbiter.
        idle = Frame()
        idle.flags = FLAG_SENDER_IS_IDLE
        idle.duration_ms = duration_ms
        s.sendto(idle.pack(), consumer_path)
        sys.stderr.write(f"[done] sent={sent} pattern={args.pattern} rate={args.rate}\\n")
    finally:
        s.close()
        try:
            os.unlink(sender_path)
        except FileNotFoundError:
            pass


if __name__ == "__main__":
    main()
