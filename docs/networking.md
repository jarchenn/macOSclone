# Networking (0.4)

One virtio-net NIC, IPv4 only. Tested against QEMU user-mode networking
(slirp): guest 10.0.2.15/24, gateway and host 10.0.2.2, DNS 10.0.2.3.
`make run` adds the NIC (`-netdev user` + `-device virtio-net-pci`), and `--no-net`
leaves it out (`-nic none`).

## Layers

| Layer | File | What is implemented |
|---|---|---|
| Driver | `drivers/virtio_net.c` | legacy VirtIO PCI; 10-byte header; 64 RX and 32 TX buffers of 2 KiB; RX refilled after each packet |
| Task | `net/stack.c` | a kernel task polls the NIC every 4 ms and runs the timers; no IRQ-driven RX |
| ARP | `net/stack.c` | cache with learning from requests and replies; a blocking resolve with retries |
| IPv4 | `net/proto.c`, `stack.c` | header checksum check and build; DF set on output; **fragments are dropped, not reassembled**; routing is "on-link or via the gateway" |
| ICMP | | echo request/reply (the guest answers pings; `ping` sends them) |
| UDP | | 16 sockets, a queue of 6 datagrams each, bind, sendto, recvfrom with a timeout |
| DHCP | | DISCOVER/OFFER/REQUEST/ACK state machine at boot; mask, router and DNS options |
| DNS | | A-record resolver with 3 tries; CNAME and other records ahead of the A record are skipped (no follow-up query for a CNAME target); NXDOMAIN gives "name not found"; dotted-quad names skip the lookup |
| TCP | `net/tcp.c` | client only (see below) |

Parsing helpers (`net/proto.c`) are portable and host-tested (`net` suite):
checksums, header parsing with truncated or bad-length packets, DNS query
building and answer parsing (compression pointers, loops, truncation), and
DHCP options.

## TCP

* Active open only. **There is no `listen`/`accept`**, so Aurora cannot run a
  server.
* 16 connections, each with a 16 KiB send buffer and a 16 KiB receive buffer.
  MSS option 1460. The advertised window is the free receive space.
* Retransmission is go-back-N from `snd_una`. The RTO starts at 1 s and
  doubles up to 8 s. After 8 retries the connection is reset (`ETIMEDOUT`).
* **No congestion control** (no slow start or cwnd), no SACK, no window
  scaling, no timestamps, no Nagle algorithm.
* Close: FIN handshake, FIN_WAIT_2 times out after 15 s and TIME_WAIT lasts
  2 s. RST from the peer gives `ECONNRESET`, and a refused SYN gives
  `ECONNREFUSED`.

## System calls and programs

Sockets are process file descriptors (`FD_SOCKET`) and are closed when the
process exits or is killed. A blocked `recv` returns `AE_INTR` on kill. The
calls are listed in [processes.md](processes.md).

| Program / command | Does |
|---|---|
| `ifconfig` (Terminal) | address, mask, gateway, DNS, MAC, packet counters |
| `netstat` (Terminal) | open sockets |
| `ping HOST [N]` | ICMP echo with round-trip times |
| `nslookup NAME [SERVER[:PORT]]` | DNS A lookup |
| `fetch URL [FILE]` | HTTP/1.1 GET (`Connection: close`) to the screen or a file; `https://` is refused: **no TLS** |
| `tcpecho HOST PORT BYTES` / `udpecho HOST PORT TEXT N` | echo tests used by `network_test.py` |

## Tests (`tools/network_test.py`)

The script starts DNS, UDP-echo, TCP-echo and HTTP servers on the host. The
guest reaches them through slirp at 10.0.2.2. Checks: DHCP lease values,
ping, DNS (A, CNAME, NXDOMAIN, literal), 4 UDP round trips, a 40,000-byte TCP
echo compared byte for byte, a 60,000-byte HTTP download saved to `/home`
and compared by CRC, an HTTP 404, connection to a closed port (Windows slirp
reports a timeout instead of a refusal, and the test accepts either), an
unreachable host, refusal of `https`, killing a `fetch` blocked in `recv`,
and one lookup through slirp's real DNS forwarder.

## Not implemented

IPv6, TLS, TCP server sockets, congestion control, IP fragment reassembly,
multiple NICs, routing tables, static IP configuration from the UI, raw
sockets and e1000.
