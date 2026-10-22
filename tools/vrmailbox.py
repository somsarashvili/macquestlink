#!/usr/bin/env python3
# vrmailbox.py <mailbox> '<json>' : send one message to a SteamVR web mailbox (ws://127.0.0.1:27062)
# e.g. vrmailbox.py vrwebui_dashboard '{"type":"hide_dashboard_requested"}'
import base64, json, os, socket, struct, sys, time


def frame(text):
    data = text.encode()
    mask = os.urandom(4)
    n = len(data)
    head = bytes([0x81]) + (bytes([0x80 | n]) if n < 126 else bytes([0xFE]) + struct.pack(">H", n))
    return head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data))


s = socket.create_connection(("127.0.0.1", 27062), timeout=3)
key = base64.b64encode(os.urandom(16)).decode()
s.sendall((f"GET / HTTP/1.1\r\nHost: 127.0.0.1:27062\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\nOrigin: http://127.0.0.1:27062\r\n\r\n").encode())
s.recv(4096)
s.sendall(frame("mailbox_open mql_vrmailbox"))
time.sleep(0.5)
s.sendall(frame(f"mailbox_send {sys.argv[1]} {json.dumps(json.loads(sys.argv[2]))}"))
time.sleep(1)
s.sendall(frame("websocket_close"))
s.close()
