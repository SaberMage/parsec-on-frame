#!/usr/bin/env python3
"""Add (or remove) the Parsec non-Steam shortcut in the running Steam client.

Steam on Steam Frame exposes its UI's Chrome DevTools endpoint on
127.0.0.1:8080. This talks to its SharedJSContext and calls Steam's own
SteamClient.Apps.* functions, so the shortcut is created exactly as if added
through the UI, with FEX ("fex-stable") set as its compatibility tool, and
without restarting Steam.

Usage:
    steam_shortcut.py add <launcher> <icon>
    steam_shortcut.py remove
"""
import base64
import json
import os
import socket
import struct
import sys
import urllib.request

NAME = "Parsec"
COMPAT_TOOL = "fex-stable"
DEVTOOLS = "127.0.0.1:8080"


class DevTools:
    def __init__(self):
        try:
            tabs = json.load(urllib.request.urlopen(f"http://{DEVTOOLS}/json", timeout=5))
        except OSError as e:
            sys.exit(f"Can't reach Steam's DevTools endpoint at {DEVTOOLS} ({e}). Is Steam running?")
        url = next((t["webSocketDebuggerUrl"] for t in tabs if t.get("title") == "SharedJSContext"), None)
        if not url:
            sys.exit("Steam's SharedJSContext wasn't found; is Steam fully started?")
        host, port = DEVTOOLS.split(":")
        self.sock = socket.create_connection((host, int(port)))
        key = base64.b64encode(os.urandom(16)).decode()
        path = url.split(DEVTOOLS, 1)[1]
        self.sock.send(
            f"GET {path} HTTP/1.1\r\nHost: {DEVTOOLS}\r\nUpgrade: websocket\r\n"
            f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n".encode()
        )
        self.buf = b""
        while b"\r\n\r\n" not in self.buf:
            self.buf += self.sock.recv(4096)
        self.buf = self.buf.split(b"\r\n\r\n", 1)[1]
        self.next_id = 1

    def _send(self, obj):
        payload = json.dumps(obj).encode()
        mask = os.urandom(4)
        n = len(payload)
        if n < 126:
            header = bytes([0x81, 0x80 | n])
        elif n < 65536:
            header = bytes([0x81, 0xFE]) + struct.pack(">H", n)
        else:
            header = bytes([0x81, 0xFF]) + struct.pack(">Q", n)
        self.sock.send(header + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def _read(self, n):
        while len(self.buf) < n:
            self.buf += self.sock.recv(65536)
        data, self.buf = self.buf[:n], self.buf[n:]
        return data

    def _recv(self):
        _, b2 = self._read(2)
        n = b2 & 0x7F
        if n == 126:
            n = struct.unpack(">H", self._read(2))[0]
        elif n == 127:
            n = struct.unpack(">Q", self._read(8))[0]
        return json.loads(self._read(n))

    def eval(self, expression):
        msg_id = self.next_id
        self.next_id += 1
        self._send({"id": msg_id, "method": "Runtime.evaluate",
                    "params": {"expression": expression, "awaitPromise": True, "returnByValue": True}})
        while True:
            reply = self._recv()
            if reply.get("id") == msg_id:
                result = reply["result"]
                if "exceptionDetails" in result:
                    raise RuntimeError(result["result"].get("description", "JS exception"))
                return result["result"].get("value")


FIND_EXISTING = f"""
  appStore.allApps
    .filter(a => a.app_type == 1073741824 && a.display_name == {json.dumps(NAME)})
    .map(a => a.appid)
"""


def add(launcher, icon):
    dt = DevTools()
    existing = dt.eval(FIND_EXISTING) or []
    if existing:
        appid = existing[0]
        print(f"Updating existing '{NAME}' shortcut (appid {appid}).")
        js = f"(async () => {{ const id = {appid};"
    else:
        js = f"""(async () => {{
          const id = await SteamClient.Apps.AddShortcut({json.dumps(NAME)}, {json.dumps(launcher)},
                                                         {json.dumps(os.path.dirname(launcher))}, "");"""
    js += f"""
          SteamClient.Apps.SetShortcutName(id, {json.dumps(NAME)});
          SteamClient.Apps.SetShortcutExe(id, {json.dumps('"' + launcher + '"')});
          SteamClient.Apps.SetShortcutStartDir(id, {json.dumps('"' + os.path.dirname(launcher) + '"')});
          SteamClient.Apps.SetShortcutIcon(id, {json.dumps(icon)});
          SteamClient.Apps.SetShortcutLaunchOptions(id, "");
          SteamClient.Apps.SpecifyCompatTool(id, {json.dumps(COMPAT_TOOL)});
          return id;
        }})()"""
    appid = dt.eval(js)
    print(f"Steam shortcut '{NAME}' ready (appid {appid}), compatibility tool: {COMPAT_TOOL}.")


def remove():
    dt = DevTools()
    for appid in dt.eval(FIND_EXISTING) or []:
        dt.eval(f"SteamClient.Apps.RemoveShortcut({appid})")
        print(f"Removed Steam shortcut '{NAME}' (appid {appid}).")


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "add":
        add(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 2 and sys.argv[1] == "remove":
        remove()
    else:
        sys.exit(__doc__)
