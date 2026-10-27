# Copyright 2026 The Umbra Authors.
"""Measures streaming speed by acting as a phone against a running agent
(ws://127.0.0.1:7777/ws). It says hello with a Pixel-8-like screen, keeps
scrolling the page, acks each frame `delay_ms` after it arrives (to stand in
for a real phone's Wi-Fi round trip and decode; 0 = best case), and reports
frames per second, frame size and the gaps between frames.

  python -I remote/agent/tools/fps_test.py [seconds] [quality] [dpr] [delay_ms]

Note it takes over from a connected phone (which then shows "open on another
device"), and it scrolls whatever page is on screen; use a long one.
"""
import base64, json, os, socket, struct, sys, time

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 8
quality = int(sys.argv[2]) if len(sys.argv) > 2 else 70
dpr = float(sys.argv[3]) if len(sys.argv) > 3 else 2.625
ack_delay = (float(sys.argv[4]) if len(sys.argv) > 4 else 0) / 1000

s = socket.create_connection(("127.0.0.1", 7777))
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
key = base64.b64encode(os.urandom(16)).decode()
s.sendall((f"GET /ws HTTP/1.1\r\nHost: 127.0.0.1:7777\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
buf = b""
while b"\r\n\r\n" not in buf:
    buf += s.recv(4096)
buf = buf.split(b"\r\n\r\n", 1)[1]


def send(obj):
    data = json.dumps(obj).encode()
    mask = os.urandom(4)
    head = bytes([0x81])
    n = len(data)
    head += bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n)
    s.sendall(head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))


def read_exact(n):
    global buf
    while len(buf) < n:
        chunk = s.recv(1 << 20)
        if not chunk:
            raise EOFError
        buf += chunk
    out, buf = buf[:n], buf[n:]
    return out


def frame():
    b0, b1 = read_exact(2)
    n = b1 & 0x7F
    if n == 126:
        n = struct.unpack(">H", read_exact(2))[0]
    elif n == 127:
        n = struct.unpack(">Q", read_exact(8))[0]
    return b0 & 0x0F, read_exact(n)


def jpeg_size(j):
    i = 2
    while i + 9 < len(j):
        marker, length = j[i + 1], struct.unpack(">H", j[i + 2:i + 4])[0]
        if marker in (0xC0, 0xC1, 0xC2):
            h, w = struct.unpack(">HH", j[i + 5:i + 9])
            return f"{w}x{h}"
        i += 2 + length
    return "?"


send({"t": "hello", "w": 412, "h": 860, "sw": 412, "sh": 915, "dpr": dpr, "mobile": True, "quality": quality})
s.settimeout(0.005)
t0 = time.time()
last_scroll, frames, down, last_t, acks, last_jpeg = 0, [], True, None, [], b""
while time.time() - t0 < secs:
    now = time.time()
    if now - last_scroll > 0.03:  # keep the page moving, like a finger flicking
        send({"t": "wheel", "x": 200, "y": 400, "dx": 0, "dy": 120 if down else -120})
        last_scroll = now
        if int((now - t0) / 2) % 2 == 1:
            down = not down
    while acks and acks[0] <= now:
        acks.pop(0)
        send({"t": "ack"})
    try:
        op, payload = frame()
    except socket.timeout:
        continue
    if op == 2 and payload[:1] == b"\x01":
        t = time.time()
        frames.append((t, len(payload), (t - last_t) if last_t else 0))
        last_t = t
        acks.append(t + ack_delay)
        last_jpeg = payload[1:]

if frames:
    span = frames[-1][0] - frames[0][0]
    gaps = sorted(g for _, _, g in frames[1:]) or [0]
    print(f"quality {quality}, dpr {dpr}, phone delay {ack_delay * 1000:.0f} ms: {jpeg_size(last_jpeg)} frames, "
          f"{len(frames)} in {span:.1f}s = {len(frames) / max(span, 0.01):.1f} fps; "
          f"avg {sum(n for _, n, _ in frames) / len(frames) / 1024:.0f} KB; "
          f"median gap {gaps[len(gaps) // 2] * 1000:.0f} ms, worst {gaps[-1] * 1000:.0f} ms")
else:
    print("no frames")
