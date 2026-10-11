#!/usr/bin/env python3
"""极简行协议服务端：welcome → 对 ping 回 pong；可选 --drop-after 秒后主动断开以演示重连。"""
from __future__ import annotations

import argparse
import select
import socket
import time


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("port", type=int, nargs="?", default=9100)
    ap.add_argument("--drop-after", type=float, default=0.0, help="连接建立后 N 秒主动断开（0=不断）")
    args = ap.parse_args()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.port))
    srv.listen(8)
    print(f"[echo] listen 127.0.0.1:{args.port} drop_after={args.drop_after}", flush=True)

    while True:
        conn, addr = srv.accept()
        print(f"[echo] accept {addr}", flush=True)
        conn.setblocking(False)
        conn.sendall(b"welcome\n")
        buf = b""
        t0 = time.monotonic()
        try:
            while True:
                if args.drop_after > 0 and (time.monotonic() - t0) >= args.drop_after:
                    print(f"[echo] drop {addr}", flush=True)
                    break
                r, _, _ = select.select([conn], [], [], 0.2)
                if not r:
                    continue
                chunk = conn.recv(4096)
                if not chunk:
                    print(f"[echo] peer closed {addr}", flush=True)
                    break
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    text = line.decode("utf-8", errors="replace").strip()
                    print(f"[echo] <- {text!r}", flush=True)
                    if text == "ping":
                        conn.sendall(b"pong\n")
                    elif text == "quit":
                        conn.sendall(b"bye\n")
                        raise SystemExit
        finally:
            conn.close()


if __name__ == "__main__":
    raise SystemExit(main())
