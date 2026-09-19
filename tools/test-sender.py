#!/usr/bin/env python3
"""Stream an ffmpeg test pattern to VScreen, speaking the vscreen protocol.

Lets you check the receiver without the Linux laptop:
    tools/test-sender.py 127.0.0.1 --size 2560x1440 --seconds 10
"""
import argparse
import socket
import struct
import subprocess
import sys
import threading
import time

MAGIC = 0x56534352
HELLO, VIDEO, KEYFRAME_REQ = 1, 2, 3
AUD = b"\x00\x00\x00\x01\x09"


def header(msg_type, flags, length, pts):
    return struct.pack(">IBBHIQ", MAGIC, msg_type, flags, 0, length, pts)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=7310)
    ap.add_argument("--size", default="2560x1440")
    ap.add_argument("--fps", type=int, default=60)
    ap.add_argument("--seconds", type=float, default=0, help="0 = forever")
    ap.add_argument("--encoder", default="h264_videotoolbox")
    args = ap.parse_args()
    w, h = map(int, args.size.split("x"))

    sock = socket.create_connection((args.host, args.port))
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.sendall(header(HELLO, 0, 12, 0) + struct.pack(">HHII", 1, 1, w, h))

    def read_requests():
        while True:
            data = sock.recv(20)
            if not data:
                return
            if len(data) >= 5 and data[4] == KEYFRAME_REQ:
                print("receiver requested a keyframe", file=sys.stderr)

    threading.Thread(target=read_requests, daemon=True).start()

    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-re",
           "-f", "lavfi", "-i", f"testsrc2=size={w}x{h}:rate={args.fps}"]
    if args.seconds:
        cmd += ["-t", str(args.seconds)]
    cmd += ["-c:v", args.encoder, "-g", str(args.fps), "-bf", "0", "-b:v", "20M",
            "-pix_fmt", "nv12", "-bsf:v", "h264_metadata=aud=insert", "-f", "h264", "-"]
    if args.encoder == "h264_videotoolbox":
        cmd[cmd.index("-c:v") + 2:cmd.index("-c:v") + 2] = ["-realtime", "1"]
    ff = subprocess.Popen(cmd, stdout=subprocess.PIPE)

    pending = b""
    frames = 0
    start = time.monotonic()

    def send_au(au):
        nonlocal frames
        key = b"\x00\x00\x01\x65" in au or b"\x00\x00\x01\x25" in au or b"\x00\x00\x01\x45" in au
        pts = int((time.monotonic() - start) * 1e6)
        sock.sendall(header(VIDEO, 1 if key else 0, len(au), pts) + au)
        frames += 1

    try:
        while True:
            chunk = ff.stdout.read1(1 << 16)
            if not chunk:
                break
            pending += chunk
            # Every access unit starts with an AUD; send everything before the last one.
            parts = pending.split(AUD)
            for part in parts[1:-1]:
                send_au(AUD + part)
            pending = AUD + parts[-1] if len(parts) > 1 else pending
        if pending:
            send_au(pending)
    except (BrokenPipeError, ConnectionResetError):
        print("receiver closed the connection", file=sys.stderr)
    finally:
        ff.kill()
        elapsed = time.monotonic() - start
        print(f"sent {frames} frames in {elapsed:.1f}s ({frames / elapsed:.1f} fps)", file=sys.stderr)


if __name__ == "__main__":
    main()
