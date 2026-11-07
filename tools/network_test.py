#!/usr/bin/env python3
"""Networking tests (0.4) against the real guest. QEMU user-mode networking
(slirp) gives the guest 10.0.2.15 with the host reachable as 10.0.2.2, so
every server here listens on the host's loopback interface:

  HTTP  (http.server)  deterministic 60 000-byte body, a 404, a hanging URL
  DNS   (UDP)          aurora.test -> 10.1.2.3, everything else NXDOMAIN
  UDP echo, TCP echo

One test also tries a real-Internet DNS lookup through slirp; it is reported
as "skipped" (not failed) when the machine has no Internet access.
Results: build/test-results/network.json
"""
import http.server
import json
import re
import socket
import socketserver
import struct
import sys
import threading
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.stdout.reconfigure(errors="replace")  # guest output may contain any bytes
import aurora_env as env  # noqa: E402
from qemu_harness import Guest, GuestError  # noqa: E402

OUT = env.BUILD / "test-results"
IMG = env.BUILD / "test" / "aurora.img"
BODY = bytes((i * 31 + (i >> 7)) & 0xFF for i in range(60000))
results = []


class Http(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path == "/data.bin":
            body, code = BODY, 200
        elif self.path == "/hello.txt":
            body, code = b"Hello from the host!\n", 200
        elif self.path == "/hang":
            time.sleep(30)
            body, code = b"late", 200
        else:
            body, code = b"not here\n", 404
        self.send_response(code)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)


class TcpEcho(socketserver.BaseRequestHandler):
    def handle(self):
        while True:
            data = self.request.recv(65536)
            if not data:
                return
            self.request.sendall(data)


class UdpEcho(socketserver.BaseRequestHandler):
    def handle(self):
        data, sock = self.request
        sock.sendto(data, self.client_address)


def dns_answer(query: bytes) -> bytes:
    qid = query[:2]
    # parse QNAME
    off, labels = 12, []
    while query[off]:
        l = query[off]
        labels.append(query[off + 1:off + 1 + l].decode())
        off += 1 + l
    qend = off + 5
    name = ".".join(labels).lower()
    question = query[12:qend]
    if name == "aurora.test":
        hdr = qid + b"\x81\x80" + struct.pack(">HHHH", 1, 2, 0, 0)
        cname = b"\xc0\x0c" + struct.pack(">HHIH", 5, 1, 60, 2) + b"\xc0\x0c"  # odd CNAME first: must be skipped
        a = b"\xc0\x0c" + struct.pack(">HHIH", 1, 1, 60, 4) + bytes([10, 1, 2, 3])
        return hdr + question + cname + a
    return qid + b"\x81\x83" + struct.pack(">HHHH", 1, 0, 0, 0) + question  # NXDOMAIN


class Dns(socketserver.BaseRequestHandler):
    def handle(self):
        data, sock = self.request
        try:
            sock.sendto(dns_answer(data), self.client_address)
        except Exception:
            pass


def start_servers():
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Http)
    tcp = socketserver.ThreadingTCPServer(("127.0.0.1", 0), TcpEcho)
    udp = socketserver.ThreadingUDPServer(("127.0.0.1", 0), UdpEcho)
    dns = socketserver.ThreadingUDPServer(("127.0.0.1", 0), Dns)
    for s in (httpd, tcp, udp, dns):
        s.daemon_threads = True
        threading.Thread(target=s.serve_forever, daemon=True).start()
    closed = socket.socket()
    closed.bind(("127.0.0.1", 0))
    closed_port = closed.getsockname()[1]
    closed.close()  # nothing listens here any more
    return dict(http=httpd.server_address[1], tcp=tcp.server_address[1], udp=udp.server_address[1],
                dns=dns.server_address[1], closed=closed_port), (httpd, tcp, udp, dns)


def run(g, cmd, expect, timeout=30):
    since = g.mark()
    g.type_text(cmd + "\n")
    g.wait_for(r"@@TERM aurora:\S*\$ " + re.escape(cmd) + r"\s*$", timeout=timeout, since=since)
    m = g.wait_for(expect, timeout=timeout, since=since)
    # 0.6: keys typed while a program runs go to its terminal, so let a
    # foreground program finish before the next command is typed.
    started = re.search(r"@@PROC_START pid=(\d+) ", g.serial()[since:])
    if started and not cmd.rstrip().endswith("&"):
        g.wait_for(rf"@@PROC_EXIT pid={started.group(1)} ", timeout=timeout, since=since)
    return m


def check(name, fn, *a):
    t0 = time.time()
    try:
        detail = fn(*a)
        status = "skip" if detail.startswith("SKIP") else "pass"
        results.append({"test": name, "result": status, "detail": detail, "seconds": round(time.time() - t0, 1)})
        print(f"{status.upper():5} {name}: {detail}")
    except GuestError as e:
        results.append({"test": name, "result": "fail", "detail": str(e).splitlines()[0], "seconds": round(time.time() - t0, 1)})
        print(f"FAIL  {name}: {str(e)[:1500]}")


def main():
    if not IMG.exists():
        sys.exit("error: build/test/aurora.img missing")
    OUT.mkdir(parents=True, exist_ok=True)
    ports, servers = start_servers()
    g = Guest(IMG, name="network", network=True, keep_dir=OUT / "network")
    try:
        g.start()
        g.wait_for(r"AURORA_DESKTOP_READY", timeout=180)
        g.key("ret")
        g.key("ctrl", "1")
        g.wait_for(r"@@WIN_OPEN id=\d+ app=Terminal", timeout=20)
        time.sleep(0.5)
        H = "10.0.2.2"

        def dhcp():
            m = g.wait_for(r"@@NET_UP ip=(\S+) gw=(\S+) dns=(\S+)", timeout=30, since=0)
            if m.groups() != ("10.0.2.15", "10.0.2.2", "10.0.2.3"):
                raise GuestError(f"unexpected lease {m.groups()}")
            run(g, "ifconfig", r"@@TERM +inet 10\.0\.2\.15 +netmask 255\.255\.255\.0 +gateway 10\.0\.2\.2 +dns 10\.0\.2\.3")
            return "DHCP lease 10.0.2.15, gateway 10.0.2.2, dns 10.0.2.3"

        def ping():
            m = run(g, f"ping {H} 3", r"@@TERM --- 10\.0\.2\.2: 3 sent, (\d) received ---")
            if int(m.group(1)) < 2:
                raise GuestError(f"only {m.group(1)}/3 echo replies")
            return f"{m.group(1)}/3 ICMP echo replies from the gateway"

        def dns():
            run(g, f"nslookup aurora.test {H}:{ports['dns']}", r"@@TERM aurora\.test has address 10\.1\.2\.3")
            run(g, f"nslookup missing.test {H}:{ports['dns']}", r"@@TERM nslookup: missing\.test: name not found")
            run(g, "nslookup 192.168.7.9", r"@@TERM 192\.168\.7\.9 has address 192\.168\.7\.9")
            return "A record (after a CNAME) resolved, NXDOMAIN reported, literal IPs pass through"

        def udp():
            run(g, f"udpecho {H} {ports['udp']} hello-udp 4", r"@@TERM udpecho: 4/4 replies matched")
            return "4 datagrams echoed and verified"

        def tcp():
            run(g, f"tcpecho {H} {ports['tcp']} 40000", r"@@TERM tcpecho: 40000 bytes echoed, identical", timeout=60)
            return "40000 bytes through a TCP echo server, byte-identical (exceeds both 16 KiB buffers)"

        def http_fetch():
            want = f"{zlib.crc32(BODY) & 0xFFFFFFFF:08x}"
            run(g, f"fetch http://{H}:{ports['http']}/data.bin /home/data.bin",
                rf"@@TERM fetch: 60000 body bytes in \d+ ms, crc32 {want}", timeout=60)
            run(g, "cat /home/data.bin", rf"@@CAT path=/home/data.bin len=60000 crc={want}")
            run(g, f"fetch http://{H}:{ports['http']}/hello.txt", r"@@TERM Hello from the host!")
            run(g, f"fetch http://{H}:{ports['http']}/nothing", r"@@TERM HTTP/1\.[01] 404")
            return f"60000-byte body crc {want} matched in fetch and after saving to /home; 404 reported"

        def refused():
            # A refused connection shows up as RST -> "connection refused" where
            # the network delivers one; QEMU's slirp on Windows drops the SYN
            # silently instead, so the connect then times out after 5 s.
            c = run(g, f"fetch http://{H}:{ports['closed']}/",
                    r"@@TERM fetch: connect 10\.0\.2\.2:\d+: (Connection refused|Connection reset by peer|Connection timed out)", timeout=20)
            m = run(g, "ping 10.0.2.77 1", r"@@TERM seq=1: (Host is unreachable|Connection timed out)")
            run(g, "fetch https://example.com/", r"@@TERM fetch: https is not supported")
            return f"closed port: {c.group(1)}; unknown host: {m.group(1)}; https rejected"

        def kill_blocked():
            since = g.mark()
            g.type_text(f"fetch http://{H}:{ports['http']}/hang &\n")
            pid = g.wait_for(r"@@PROC_START pid=(\d+) name=fetch", timeout=10, since=since).group(1)
            time.sleep(2.0)
            run(g, f"kill {pid}", rf"@@PROC_EXIT pid={pid} name=fetch status=9 reason=killed by SIGKILL", timeout=15)
            run(g, f"fetch http://{H}:{ports['http']}/hello.txt", r"@@TERM Hello from the host!")
            return "a process blocked in recv() was killed; networking kept working"

        def internet():
            since = g.mark()
            g.type_text("nslookup example.com\n")
            m = g.wait_for(r"@@TERM (example\.com has address [\d.]+|nslookup: example\.com: .*)", timeout=20, since=since)
            if "has address" in m.group(1):
                return "real DNS through slirp: " + m.group(1)
            return "SKIP no Internet DNS from this machine: " + m.group(1)

        for name, fn in (("dhcp", dhcp), ("icmp-ping", ping), ("dns", dns), ("udp-echo", udp), ("tcp-echo", tcp),
                         ("http-fetch", http_fetch), ("errors", refused), ("kill-blocked-socket", kill_blocked),
                         ("internet-dns-optional", internet)):
            check(name, fn)
        g.screendump(OUT / "network.png")
    finally:
        g.stop()
        for s in servers:
            s.shutdown()
    (OUT / "network.json").write_text(json.dumps(results, indent=2))
    failed = [r for r in results if r["result"] == "fail"]
    print(f"network: {len(results) - len(failed)}/{len(results)} passed or skipped")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
