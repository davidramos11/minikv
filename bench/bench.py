#!/usr/bin/env python3
"""Load-test a running minikv server.

Opens several client connections in parallel. Each one sends SET then GET
pairs and waits for every reply before sending the next command, so this
measures request/response throughput and latency (no pipelining).

Usage:
    python3 bench/bench.py --clients 8 --requests 20000
"""
import argparse
import socket
import statistics
import threading
import time


def worker(host, port, n_ops, client_id, latencies, errors):
    sock = socket.create_connection((host, port))
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)  # don't batch small packets
    f = sock.makefile("rwb")
    local = []
    for i in range(n_ops):
        if i % 2 == 0:
            cmd = f"SET bench:{client_id}:{i} value{i}\n"
        else:
            cmd = f"GET bench:{client_id}:{i - 1}\n"
        start = time.perf_counter()
        f.write(cmd.encode())
        f.flush()
        reply = f.readline()
        local.append(time.perf_counter() - start)
        if not reply or reply.startswith(b"ERR"):
            errors.append(reply)
    f.write(b"QUIT\n")
    f.flush()
    sock.close()
    latencies.extend(local)  # list.extend is thread-safe in CPython


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=6380)
    p.add_argument("--clients", type=int, default=8)
    p.add_argument("--requests", type=int, default=20000, help="total requests across all clients")
    args = p.parse_args()

    per_client = args.requests // args.clients
    latencies, errors = [], []
    threads = [
        threading.Thread(target=worker, args=(args.host, args.port, per_client, c, latencies, errors))
        for c in range(args.clients)
    ]
    start = time.perf_counter()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    elapsed = time.perf_counter() - start

    total = len(latencies)
    lat_us = sorted(x * 1e6 for x in latencies)
    print(f"clients:    {args.clients}")
    print(f"requests:   {total}  (errors: {len(errors)})")
    print(f"throughput: {total / elapsed:,.0f} req/s")
    print(f"latency p50: {statistics.median(lat_us):.0f} us")
    print(f"latency p99: {lat_us[int(total * 0.99) - 1]:.0f} us")


if __name__ == "__main__":
    main()
