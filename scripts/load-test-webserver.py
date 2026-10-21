#!/usr/bin/env python3
"""
load-test-webserver.py - Concurrent load tester for aios-webserver + PHP-FPM + ASQL

Tests high-concurrency HTTP/1.1 streaming throughput, latency percentiles,
keep-alive handling, and ASQL query throughput under load.
"""

import argparse
import http.client
import math
import os
import subprocess
import sys
import threading
import time
from urllib.parse import urlparse

class LoadStats:
    def __init__(self):
        self.lock = threading.Lock()
        self.total_requests = 0
        self.successful_requests = 0
        self.status_counts = {}
        self.error_counts = {}
        self.latencies_ms = []
        self.total_bytes = 0
        self.start_time = 0.0
        self.end_time = 0.0

    def record_success(self, status_code: int, latency_ms: float, byte_count: int):
        with self.lock:
            self.total_requests += 1
            self.successful_requests += 1
            self.status_counts[status_code] = self.status_counts.get(status_code, 0) + 1
            self.latencies_ms.append(latency_ms)
            self.total_bytes += byte_count

    def record_error(self, err_msg: str, latency_ms: float = 0.0):
        with self.lock:
            self.total_requests += 1
            self.error_counts[err_msg] = self.error_counts.get(err_msg, 0) + 1
            if latency_ms > 0:
                self.latencies_ms.append(latency_ms)

    def snapshot(self):
        with self.lock:
            return {
                "total": self.total_requests,
                "success": self.successful_requests,
                "bytes": self.total_bytes,
                "errors": sum(self.error_counts.values())
            }


def worker_loop(worker_id: int, parsed_url, req_path: str, headers: dict,
                stats: LoadStats, stop_event: threading.Event,
                request_limit: int, request_counter: list, keep_alive: bool):
    host = parsed_url.hostname or "127.0.0.1"
    port = parsed_url.port or 80

    conn = None

    def get_connection():
        c = http.client.HTTPConnection(host, port, timeout=15)
        return c

    while not stop_event.is_set():
        if request_limit > 0:
            with stats.lock:
                if request_counter[0] >= request_limit:
                    break
                request_counter[0] += 1

        t0 = time.perf_counter()
        byte_count = 0
        try:
            if conn is None:
                conn = get_connection()

            conn.request("GET", req_path, headers=headers)
            resp = conn.getresponse()
            # Drain body
            body = resp.read()
            byte_count = len(body)
            status = resp.status
            t1 = time.perf_counter()
            lat_ms = (t1 - t0) * 1000.0

            if 200 <= status < 400:
                stats.record_success(status, lat_ms, byte_count)
            else:
                stats.record_error(f"HTTP_{status}", lat_ms)

            if not keep_alive or resp.getheader("Connection", "").lower() == "close":
                conn.close()
                conn = None

        except Exception as e:
            t1 = time.perf_counter()
            lat_ms = (t1 - t0) * 1000.0
            err_name = type(e).__name__
            stats.record_error(f"{err_name}: {e}", lat_ms)
            if conn:
                try:
                    conn.close()
                except Exception:
                    pass
                conn = None

    if conn:
        try:
            conn.close()
        except Exception:
            pass


def get_asql_snapshot(asql_top_bin: str, sock_path: str, pass_file: str, db: str):
    if not (os.path.exists(asql_top_bin) and os.path.exists(sock_path) and os.path.exists(pass_file)):
        return None
    try:
        pw = open(pass_file).readline().strip()
        cmd = [asql_top_bin, "-1", "-S", sock_path, "-u", "root", "-p", pw, "-d", db]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=5)
        if res.returncode == 0:
            lines = res.stdout.splitlines()
            metrics = {}
            for l in lines:
                l_strip = l.strip()
                if "Total Queries" in l_strip or "Avg Latency" in l_strip or "Live QPS" in l_strip:
                    metrics["summary"] = l_strip
                if "RAM ColumnStore" in l_strip or "Tables Loaded" in l_strip:
                    metrics["storage"] = l_strip
                if "Parallel Morsels" in l_strip or "Radix Joins" in l_strip:
                    metrics["accel"] = l_strip
            return metrics
    except Exception:
        pass
    return None


def run_load_test(target_url: str, concurrency: int, duration_sec: int,
                  max_requests: int, keep_alive: bool, vhost: str = ""):
    parsed = urlparse(target_url)
    req_path = parsed.path
    if parsed.query:
        req_path += "?" + parsed.query
    if not req_path:
        req_path = "/"

    req_headers = {
        "User-Agent": "aios-load-test/1.0",
        "Accept": "*/*",
    }
    if vhost:
        req_headers["Host"] = vhost
    elif parsed.netloc:
        req_headers["Host"] = parsed.netloc

    if keep_alive:
        req_headers["Connection"] = "keep-alive"
    else:
        req_headers["Connection"] = "close"

    print("=" * 78)
    print(f"  AIOS WEBSERVER + ASQL LOAD TEST")
    print("=" * 78)
    print(f"  Target URL    : {target_url}")
    print(f"  Host Header   : {req_headers.get('Host', '')}")
    print(f"  Concurrency   : {concurrency} parallel workers")
    if duration_sec > 0:
        print(f"  Mode          : Fixed Duration ({duration_sec}s)")
    else:
        print(f"  Mode          : Fixed Requests ({max_requests} total)")
    print(f"  Keep-Alive    : {'Enabled' if keep_alive else 'Disabled (new conn per req)'}")

    # Inspect ASQL before
    # The ASQL to watch: ASQL_SOCKET and ASQL_PASS_FILE name its root's socket and password file,
    # ASQL_TOP the aios-asql-top binary (default: this tree's build/).
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    asql_top = os.environ.get("ASQL_TOP", os.path.join(repo, "build", "aios-asql-top"))
    asql_sock = os.environ.get("ASQL_SOCKET", "")
    asql_pass = os.environ.get("ASQL_PASS_FILE", "")
    asql_before = get_asql_snapshot(asql_top, asql_sock, asql_pass, "versioneight")
    if asql_before:
        print("  ASQL Engine   : ONLINE (attached to local socket)")
        if "summary" in asql_before:
            print(f"    Initial     : {asql_before['summary']}")
    else:
        print("  ASQL Engine   : not attached (set ASQL_SOCKET and ASQL_PASS_FILE, and build aios-asql-top)")
    print("-" * 78)

    stats = LoadStats()
    stop_event = threading.Event()
    request_counter = [0]

    threads = []
    stats.start_time = time.perf_counter()

    for i in range(concurrency):
        t = threading.Thread(
            target=worker_loop,
            args=(i, parsed, req_path, req_headers, stats, stop_event,
                  max_requests, request_counter, keep_alive),
            daemon=True
        )
        threads.append(t)
        t.start()

    # Progress loop
    last_count = 0
    t_start = stats.start_time
    try:
        while True:
            time.sleep(1.0)
            now = time.perf_counter()
            elapsed = now - t_start
            snap = stats.snapshot()
            cur_total = snap["total"]
            delta = cur_total - last_count
            last_count = cur_total
            rps = delta / 1.0
            mb = snap["bytes"] / (1024 * 1024)
            err_str = f" | errs={snap['errors']}" if snap['errors'] > 0 else ""
            print(f"  [{elapsed:5.1f}s] requests: {cur_total:6d} ({rps:6.1f} req/s) | {mb:6.2f} MB streamed{err_str}")

            if duration_sec > 0 and elapsed >= duration_sec:
                break
            if max_requests > 0 and snap["total"] >= max_requests:
                break
            if not any(t.is_alive() for t in threads):
                break
    except KeyboardInterrupt:
        print("\n  [!] Test interrupted by user.")
    finally:
        stop_event.set()
        for t in threads:
            t.join(timeout=2.0)
        stats.end_time = time.perf_counter()

    duration = max(stats.end_time - stats.start_time, 0.001)
    tot = stats.total_requests
    succ = stats.successful_requests
    lats = sorted(stats.latencies_ms)

    print("=" * 78)
    print("  RESULTS & PERFORMANCE SUMMARY")
    print("=" * 78)
    print(f"  Total Duration       : {duration:.2f} s")
    print(f"  Total Requests       : {tot}")
    print(f"  Successful Requests  : {succ} ({succ / max(tot, 1) * 100:.1f}%)")
    print(f"  Throughput (RPS)     : {tot / duration:.2f} requests/sec")
    print(f"  Data Transferred     : {stats.total_bytes / (1024 * 1024):.2f} MB ({stats.total_bytes / (1024 * 1024) / duration:.2f} MB/s)")

    if stats.status_counts:
        print("\n  HTTP Status Codes:")
        for sc, count in sorted(stats.status_counts.items()):
            print(f"    HTTP {sc:3d} : {count:6d} ({count / tot * 100:.1f}%)")

    if stats.error_counts:
        print("\n  Errors / Failures:")
        for err, count in sorted(stats.error_counts.items(), key=lambda x: -x[1]):
            print(f"    {err:30s} : {count:6d}")

    if lats:
        def p(pct):
            idx = int(math.ceil((pct / 100.0) * len(lats))) - 1
            return lats[max(0, min(idx, len(lats) - 1))]

        avg_lat = sum(lats) / len(lats)
        print("\n  Latency Distribution (Round-Trip):")
        print(f"    Min   : {lats[0]:8.2f} ms")
        print(f"    p50   : {p(50):8.2f} ms (median)")
        print(f"    p90   : {p(90):8.2f} ms")
        print(f"    p95   : {p(95):8.2f} ms")
        print(f"    p99   : {p(99):8.2f} ms")
        print(f"    Max   : {lats[-1]:8.2f} ms")
        print(f"    Avg   : {avg_lat:8.2f} ms")

    # Inspect ASQL after
    asql_after = get_asql_snapshot(asql_top, asql_sock, asql_pass, "versioneight")
    if asql_after and "summary" in asql_after:
        print("-" * 78)
        print(f"  ASQL Engine Post-Run : {asql_after['summary']}")
        if "accel" in asql_after:
            print(f"  ASQL Acceleration    : {asql_after['accel']}")
    print("=" * 78)


def main():
    parser = argparse.ArgumentParser(description="Load test aios-webserver + PHP-FPM + ASQL")
    parser.add_argument("--url", default="http://127.0.0.1:18080/?route=apir/orders&key=9NPZ5AJ4FNNT9C7Y&userid=ALANB",
                        help="Target URL (default: ERP orders query route)")
    parser.add_argument("-c", "--concurrency", type=int, default=10,
                        help="Number of concurrent worker threads (default: 10)")
    parser.add_argument("-d", "--duration", type=int, default=10,
                        help="Duration of test in seconds (default: 10, set 0 to use -n)")
    parser.add_argument("-n", "--requests", type=int, default=0,
                        help="Max total requests to send (default: 0 = duration-based)")
    parser.add_argument("--vhost", default="",
                        help="Custom Host header if different from URL")
    parser.add_argument("--no-keep-alive", action="store_true",
                        help="Disable HTTP keep-alive (open fresh socket each time)")

    args = parser.parse_args()
    run_load_test(
        target_url=args.url,
        concurrency=args.concurrency,
        duration_sec=args.duration,
        max_requests=args.requests,
        keep_alive=not args.no_keep_alive,
        vhost=args.vhost,
    )


if __name__ == "__main__":
    main()
