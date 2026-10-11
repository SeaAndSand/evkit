#!/usr/bin/env python3
"""简易 NDJSON 命令服务器：演示 sync/async、单/多返回，以及异常用例。

用法:
  python3 mock_server.py [port]
  默认端口 9090

启动后可交互输入预设脚本，或自动跑 demo 序列。
"""
from __future__ import annotations

import json
import socket
import sys
import threading
import time


def send(conn: socket.socket, obj: dict) -> None:
    line = json.dumps(obj, ensure_ascii=False) + "\n"
    conn.sendall(line.encode("utf-8"))
    print("  →", line.strip(), flush=True)


def recv_lines(conn: socket.socket, timeout: float = 5.0):
    conn.settimeout(timeout)
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            chunk = conn.recv(4096)
        except socket.timeout:
            return
        if not chunk:
            return
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            if not line.strip():
                continue
            text = line.decode("utf-8", errors="replace")
            print("  ←", text, flush=True)
            try:
                yield json.loads(text)
            except json.JSONDecodeError:
                print("  ! bad reply json", flush=True)
            deadline = time.time() + timeout  # 每收到一行重置等待


def handle_client(conn: socket.socket, addr) -> None:
    print(f"[server] client {addr}", flush=True)
    try:
        # 1) sync ping
        send(conn, {"id": "1", "cmd": "ping", "mode": "sync"})
        list(recv_lines(conn, 2.0))

        # 2) sync echo
        send(conn, {"id": "2", "cmd": "echo", "mode": "sync", "args": {"text": "hello"}})
        list(recv_lines(conn, 2.0))

        # 3) async single
        send(conn, {"id": "3", "cmd": "slow", "mode": "async", "args": {"delay_ms": 300}})
        list(recv_lines(conn, 3.0))

        # 4) async multi
        send(
            conn,
            {
                "id": "4",
                "cmd": "count",
                "mode": "async",
                "args": {"n": 3, "interval_ms": 150},
            },
        )
        list(recv_lines(conn, 3.0))

        # 5) unknown cmd
        send(conn, {"id": "5", "cmd": "nope", "mode": "sync"})
        list(recv_lines(conn, 2.0))

        # 6) bad mode
        send(conn, {"id": "6", "cmd": "ping", "mode": "async"})
        list(recv_lines(conn, 2.0))

        # 7) bad json
        conn.sendall(b"{not-json\n")
        list(recv_lines(conn, 2.0))

        # 8) quit — 让客户端干净退出
        send(conn, {"id": "8", "cmd": "quit", "mode": "sync"})
        list(recv_lines(conn, 2.0))

        time.sleep(0.3)
        print("[server] demo sequence done, closing", flush=True)
    except (BrokenPipeError, ConnectionResetError) as exc:
        print(f"[server] client gone: {exc}", flush=True)
    finally:
        conn.close()


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9090
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(5)
    print(f"[server] listen 0.0.0.0:{port}", flush=True)
    while True:
        conn, addr = srv.accept()
        threading.Thread(target=handle_client, args=(conn, addr), daemon=True).start()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("\n[server] bye")
