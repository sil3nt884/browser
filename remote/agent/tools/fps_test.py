# Copyright 2026 The Umbra Authors.
"""Measures streaming speed by acting as a phone against a running agent
(ws://127.0.0.1:7777/ws). It says hello with a Pixel-8-like screen, keeps
scrolling the page, acks each frame `delay_ms` after it arrives (to stand in
for a real phone's Wi-Fi round trip and decode; 0 = best case), and reports
frames per second, frame size and the gaps between frames.

  python -I remote/agent/tools/fps_test.py [seconds] [quality] [dpr] [delay_ms] [url]

Without a url it opens a long built-in test page (no network needed).

With H.264 video the stream is also written to fps_test.h264 in the temp
folder (check it with examples/h264_check.rs).

Note it takes over from a connected phone (which then shows "open on another
device"), and it scrolls whatever page is on screen; use a long one.
"""
import base64, json, os, socket, struct, sys, tempfile, time

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 8
quality = int(sys.argv[2]) if len(sys.argv) > 2 else 70
dpr = float(sys.argv[3]) if len(sys.argv) > 3 else 2.625
ack_delay = (float(sys.argv[4]) if len(sys.argv) > 4 else 0) / 1000
TEST_PAGE = ("data:text/html,<meta name=viewport content='width=device-width'><body style='font:16px sans-serif'>"
             "<script>for(let i=0;i<400;i++)document.write('<p>'+i+' The quick brown fox jumps over the lazy dog. "
             "Pack my box with five dozen liquor jugs.</p>')</script>")
url = sys.argv[5] if len(sys.argv) > 5 else TEST_PAGE

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


def frame():
    """The next WebSocket message as (opcode, payload); waits at most the
    socket timeout, and leaves a partly received message in `buf`."""
    global buf
    while True:
        if len(buf) >= 2:
            n, head = buf[1] & 0x7F, 2
            if n == 126 and len(buf) >= 4:
                n, head = struct.unpack(">H", buf[2:4])[0], 4
            elif n == 127 and len(buf) >= 10:
                n, head = struct.unpack(">Q", buf[2:10])[0], 10
            if n < 126 or head > 2:
                if len(buf) >= head + n:
                    op, payload, buf = buf[0] & 0x0F, buf[head:head + n], buf[head + n:]
                    return op, payload
        chunk = s.recv(1 << 20)
        if not chunk:
            raise EOFError
        buf += chunk


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
send({"t": "nav", "url": url})
time.sleep(1.5)
s.settimeout(0.005)
t0 = time.time()
last_scroll, frames, down, last_t, acks, last_jpeg = 0, [], True, None, [], b""
video, h264 = None, bytearray()
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
    if op == 1:
        m = json.loads(payload)
        if m.get("t") == "video":
            video, h264 = f"{m['w']}x{m['h']} H.264", bytearray()
    if op == 2 and payload[:1] in (b"", b""):
        t = time.time()
        frames.append((t, len(payload), (t - last_t) if last_t else 0))
        last_t = t
        acks.append(t + ack_delay)
        if payload[:1] == b"":
            last_jpeg = payload[1:]
        else:
            h264 += payload[1:]

if frames:
    span = frames[-1][0] - frames[0][0]
    gaps = sorted(g for _, _, g in frames[1:]) or [0]
    if h264:
        open(os.path.join(tempfile.gettempdir(), "fps_test.h264"), "wb").write(h264)
    kind = video or f"{jpeg_size(last_jpeg)} JPEG"
    mbit = sum(n for _, n, _ in frames) * 8 / max(span, 0.01) / 1e6
    print(f"quality {quality}, dpr {dpr}, phone delay {ack_delay * 1000:.0f} ms: {kind} frames, {mbit:.1f} Mbit/s, "
          f"{len(frames)} in {span:.1f}s = {len(frames) / max(span, 0.01):.1f} fps; "
          f"avg {sum(n for _, n, _ in frames) / len(frames) / 1024:.0f} KB; "
          f"median gap {gaps[len(gaps) // 2] * 1000:.0f} ms, worst {gaps[-1] * 1000:.0f} ms")
else:
    print("no frames")
