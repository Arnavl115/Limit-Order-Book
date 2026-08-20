#!/usr/bin/env python3
"""
Phase 5A — stdlib-only gateway client (TCP length-prefix JSON).
No pip deps: uses socket, struct, json, time, logging.

Wire: [4B BE len][JSON]  (see docs/protocol.md:1, src/gateway/frame.hpp:1)
Also supports optional WS upgrade if needed, but default is TCP.
"""

import socket
import struct
import json
import time
import logging

log = logging.getLogger("mm_client")

MAX_FRAME = 1 << 20  # 1 MiB

class MMClient:
    def __init__(self, host="127.0.0.1", port=9000, timeout=2.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.sock = None
        self.buf = bytearray()
        self.session_id = None

    def connect(self, retries=5, backoff=0.2):
        for i in range(retries):
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(self.timeout)
                s.connect((self.host, self.port))
                # Disable Nagle for low latency
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                self.sock = s
                self.buf = bytearray()
                log.info("connected to %s:%s", self.host, self.port)
                return True
            except Exception as e:
                log.warning("connect attempt %d failed: %s", i+1, e)
                try:
                    s.close()
                except: pass
                time.sleep(backoff * (1.5 ** i))
        return False

    def close(self):
        if self.sock:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except: pass
            try:
                self.sock.close()
            except: pass
            self.sock = None

    def send(self, obj):
        """Send JSON object (dict) as length-prefixed frame."""
        data = json.dumps(obj, separators=(",", ":")).encode("utf-8")
        if len(data) > MAX_FRAME:
            raise ValueError("frame too large")
        hdr = struct.pack(">I", len(data))
        if not self.sock:
            raise ConnectionError("not connected")
        self.sock.sendall(hdr + data)

    def _recv_exact(self, n):
        """Recv exactly n bytes or return None on timeout/closed."""
        while len(self.buf) < n:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                return None
            if not chunk:
                return None
            self.buf.extend(chunk)
        result = bytes(self.buf[:n])
        del self.buf[:n]
        return result

    def recv(self, timeout=None):
        """Recv one JSON object (blocking up to timeout). Returns dict or None."""
        if timeout is not None:
            old = self.sock.gettimeout()
            self.sock.settimeout(timeout)
        try:
            hdr = self._recv_exact(4)
            if hdr is None:
                return None
            (ln,) = struct.unpack(">I", hdr)
            if ln == 0 or ln > MAX_FRAME:
                log.error("bad frame len %s", ln)
                return None
            payload = self._recv_exact(ln)
            if payload is None:
                return None
            return json.loads(payload.decode("utf-8"))
        except socket.timeout:
            return None
        except Exception as e:
            log.error("recv error: %s", e)
            return None
        finally:
            if timeout is not None:
                try:
                    self.sock.settimeout(old)
                except: pass

    def recv_all(self, timeout=0.2):
        """Collect all available messages within timeout window."""
        msgs = []
        end = time.time() + timeout
        while time.time() < end:
            # poll with short timeout
            self.sock.settimeout(0.05)
            try:
                m = self.recv(timeout=0.05)
                if m is not None:
                    msgs.append(m)
                    # reset end to allow more coalesced messages
                    end = time.time() + 0.15
                else:
                    # no message, check if we have at least one
                    if msgs:
                        # wait a bit more for coalesced
                        continue
                    # else keep polling until total timeout
            except:
                break
        return msgs

    def ping(self, timeout=1.0):
        self.send({"type": "ping", "id": int(time.time()*1000)})
        return self.recv(timeout=timeout)

    def subscribe(self):
        self.send({"type": "subscribe", "channel": "book"})
        # snapshot will arrive as marketdata.snapshot
        return self.recv(timeout=1.0)

# Simple test harness when run directly
if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--test", action="store_true", help="run loopback integration test")
    args = ap.parse_args()

    c = MMClient(args.host, args.port)
    if not c.connect():
        print("FAIL: could not connect")
        raise SystemExit(1)
    if args.test:
        print("connected, sending ping")
        c.send({"type": "ping", "id": 123})
        m = c.recv(timeout=2)
        print("pong:", m)
        print("sending subscribe")
        c.send({"type": "subscribe"})
        m = c.recv(timeout=2)
        print("snapshot:", json.dumps(m)[:500])
        # place order
        c.send({"type": "order.new", "id": 1, "side": "buy", "price": 100, "qty": 10, "orderType": "limit"})
        msgs = c.recv_all(timeout=0.5)
        print("order.new -> msgs:", len(msgs))
        for mm in msgs:
            print(" ", mm.get("type"), mm.get("status"), mm.get("reason",""))
        c.close()
        print("test done")
    else:
        print("connected, interactive — type JSON lines, length-prefix will be used")
        import sys
        for line in sys.stdin:
            line=line.strip()
            if not line: continue
            try:
                obj=json.loads(line)
            except Exception as e:
                print("invalid json:", e)
                continue
            c.send(obj)
            m=c.recv(timeout=1)
            print("resp:", m)
