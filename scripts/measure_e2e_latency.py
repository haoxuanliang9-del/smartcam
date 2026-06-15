#!/usr/bin/env python3
"""
RTSP end-to-end latency measurement client.

Connects to an RTSP server via TCP interleaved mode, receives RTP/RTCP packets,
and measures per-frame latency using RTCP Sender Report NTP<->RTP timestamp mapping.

Usage:
    python3 measure_e2e_latency.py rtsp://HOST:8554/live [--duration 65] [--output /tmp/e2e_latency.csv]
"""

import socket
import struct
import time
import sys
import argparse
import csv
import threading
from datetime import datetime, timezone


def ntp_to_us(ntp_hi, ntp_lo):
    secs = ntp_hi - 2208988800
    frac = ntp_lo / (2**32)
    return int((secs + frac) * 1_000_000)


def wall_us():
    return int(time.time() * 1_000_000)


class RTSPClient:
    def __init__(self, url):
        self.url = url
        parts = url.replace("rtsp://", "").split("/")
        host_port = parts[0].split(":")
        self.host = host_port[0]
        self.port = int(host_port[1]) if len(host_port) > 1 else 8554
        self.path = "/" + "/".join(parts[1:])
        self.sock = None
        self.cseq = 0
        self.session_id = None
        self.recv_buf = b""

    def connect(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(10)
        self.sock.connect((self.host, self.port))
        self.sock.settimeout(5)

    def send_request(self, method, extra_headers="", body=""):
        self.cseq += 1
        req = f"{method} {self.url} RTSP/1.0\r\nCSeq: {self.cseq}\r\n"
        if self.session_id:
            req += f"Session: {self.session_id}\r\n"
        req += extra_headers
        if body:
            req += f"Content-Length: {len(body)}\r\n"
        req += "\r\n"
        if body:
            req += body
        self.sock.sendall(req.encode())

    def read_response(self):
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("Connection closed")
            buf += chunk

        header_end = buf.index(b"\r\n\r\n") + 4
        header = buf[:header_end].decode()
        body = buf[header_end:]

        content_len = 0
        for line in header.split("\r\n"):
            if line.lower().startswith("content-length:"):
                content_len = int(line.split(":")[1].strip())
                break

        while len(body) < content_len:
            chunk = self.sock.recv(content_len - len(body))
            if not chunk:
                break
            body += chunk

        status_line = header.split("\r\n")[0]
        status_code = int(status_line.split()[1])

        for line in header.split("\r\n"):
            if line.lower().startswith("session:"):
                self.session_id = line.split(":")[1].strip().split(";")[0]

        return status_code, header, body

    def setup_tcp(self):
        self.connect()

        self.send_request("OPTIONS")
        self.read_response()

        self.send_request("DESCRIBE", "Accept: application/sdp\r\n")
        _, _, sdp_body = self.read_response()

        self.send_request("SETUP",
                          f"Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
        self.read_response()

        self.send_request("PLAY")
        self.read_response()

        self.sock.settimeout(0.1)

    def teardown(self):
        try:
            self.sock.settimeout(2)
            self.send_request("TEARDOWN")
            self.read_response()
        except Exception:
            pass
        finally:
            if self.sock:
                self.sock.close()

    def recv_interleaved(self):
        while True:
            while len(self.recv_buf) < 4:
                try:
                    chunk = self.sock.recv(65536)
                    if not chunk:
                        return None, None, None
                    self.recv_buf += chunk
                except socket.timeout:
                    continue
                except Exception:
                    return None, None, None

            if self.recv_buf[0] != ord('$'):
                idx = self.recv_buf.find(b'$')
                if idx < 0:
                    self.recv_buf = b""
                    continue
                self.recv_buf = self.recv_buf[idx:]

            if len(self.recv_buf) < 4:
                continue

            channel = self.recv_buf[1]
            pkt_len = (self.recv_buf[2] << 8) | self.recv_buf[3]

            while len(self.recv_buf) < 4 + pkt_len:
                try:
                    chunk = self.sock.recv(65536)
                    if not chunk:
                        return None, None, None
                    self.recv_buf += chunk
                except socket.timeout:
                    continue
                except Exception:
                    return None, None, None

            data = self.recv_buf[4:4 + pkt_len]
            self.recv_buf = self.recv_buf[4 + pkt_len:]

            return channel, data, wall_us()


def parse_rtp_header(data):
    if len(data) < 12:
        return None
    v = (data[0] >> 6) & 0x03
    if v != 2:
        return None
    pt = data[1] & 0x7F
    marker = (data[1] >> 7) & 1
    seq = (data[2] << 8) | data[3]
    timestamp = struct.unpack('>I', data[4:8])[0]
    ssrc = struct.unpack('>I', data[8:12])[0]
    return {
        'pt': pt, 'marker': marker, 'seq': seq,
        'timestamp': timestamp, 'ssrc': ssrc
    }


def parse_rtcp_sr(data):
    if len(data) < 28:
        return None
    pt = data[1]
    if pt != 200:
        return None
    ssrc = struct.unpack('>I', data[4:8])[0]
    ntp_hi = struct.unpack('>I', data[8:12])[0]
    ntp_lo = struct.unpack('>I', data[12:16])[0]
    rtp_ts = struct.unpack('>I', data[16:20])[0]
    return {
        'ssrc': ssrc,
        'ntp_us': ntp_to_us(ntp_hi, ntp_lo),
        'rtp_timestamp': rtp_ts
    }


def measure_latency(url, duration_sec, output_path):
    client = RTSPClient(url)
    client.setup_tcp()

    sr_map = {}
    latencies = []
    ntp_offsets = []

    print(f"Measuring E2E latency for {duration_sec}s...")
    start_time = time.time()

    try:
        while time.time() - start_time < duration_sec:
            channel, data, recv_us = client.recv_interleaved()
            if data is None:
                continue

            if channel % 2 == 1:
                sr = parse_rtcp_sr(data)
                if sr:
                    sr_map[sr['ssrc']] = sr
                    ntp_now = int(time.time() * 1_000_000)
                    offset = ntp_now - sr['ntp_us']
                    ntp_offsets.append(offset)
                    print(f"  RTCP SR: ssrc={sr['ssrc']}, "
                          f"ntp_offset={offset}us, rtp_ts={sr['rtp_timestamp']}")
                continue

            rtp = parse_rtp_header(data)
            if not rtp or rtp['pt'] != 96:
                continue

            ssrc = rtp['ssrc']
            rtp_ts = rtp['timestamp']

            if rtp['marker']:
                if ssrc in sr_map:
                    sr = sr_map[ssrc]
                    clock_rate = 90000
                    rtp_diff = rtp_ts - sr['rtp_timestamp']
                    if rtp_diff < 0:
                        rtp_diff += 2**32
                    server_ntp_us = sr['ntp_us'] + int(rtp_diff * 1_000_000 / clock_rate)
                    latency_us = recv_us - server_ntp_us
                    latencies.append(latency_us)

                    if len(latencies) % 100 == 0:
                        print(f"  Frames: {len(latencies)}, "
                              f"latest latency: {latency_us/1000:.1f}ms")

    except KeyboardInterrupt:
        pass
    finally:
        client.teardown()

    if not latencies:
        print("ERROR: No latency measurements collected!")
        return

    clock_offset_us = 0
    if ntp_offsets:
        ntp_offsets.sort()
        clock_offset_us = ntp_offsets[len(ntp_offsets) // 2]
        print(f"\n  Clock offset (median of {len(ntp_offsets)} SRs): {clock_offset_us/1000:.1f}ms")
        print(f"  (positive = client ahead of server)")

    latencies = [l - clock_offset_us for l in latencies]

    latencies.sort()
    n = len(latencies)
    p50 = latencies[int(n * 0.50)]
    p90 = latencies[int(n * 0.90)]
    p95 = latencies[int(n * 0.95)]
    p99 = latencies[int(n * 0.99)]
    avg = sum(latencies) // n
    mn = latencies[0]
    mx = latencies[-1]

    print(f"\n{'='*60}")
    print(f"End-to-End Latency Results ({n} frames)")
    print(f"{'='*60}")
    print(f"  Min:    {mn/1000:.1f} ms")
    print(f"  Avg:    {avg/1000:.1f} ms")
    print(f"  P50:    {p50/1000:.1f} ms")
    print(f"  P90:    {p90/1000:.1f} ms")
    print(f"  P95:    {p95/1000:.1f} ms")
    print(f"  P99:    {p99/1000:.1f} ms")
    print(f"  Max:    {mx/1000:.1f} ms")
    print(f"{'='*60}")

    with open(output_path, 'w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['frame_idx', 'latency_us', 'latency_ms'])
        for i, lat in enumerate(latencies):
            writer.writerow([i, lat, f"{lat/1000:.3f}"])

    print(f"Raw data saved to {output_path}")


def main():
    parser = argparse.ArgumentParser(description='RTSP E2E latency measurement')
    parser.add_argument('url', help='RTSP URL (e.g. rtsp://100.111.77.78:8554/live)')
    parser.add_argument('--duration', type=int, default=65, help='Measurement duration in seconds')
    parser.add_argument('--output', default='/tmp/e2e_latency.csv', help='Output CSV path')
    args = parser.parse_args()

    measure_latency(args.url, args.duration, args.output)


if __name__ == '__main__':
    main()
