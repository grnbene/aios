#!/bin/bash
# The host half of `bench`: metal/bench_core.inc built with the kernel's own
# optimization flags (-O2, C++20), run here. Compare with `bench` on the Pi.
set -e
here="$(cd "$(dirname "$0")/../.." && pwd)"
tmp="$(mktemp -d)"
cat > "$tmp/b.cpp" <<'CPP'
#include <cstdint>
#include <cstdio>
#include <chrono>
static uint64_t bench_now_ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static void bench_report(const char* name, uint64_t ns, long check) { std::printf("  %-22s %llu.%llu ms  (check %ld)\n", name, (unsigned long long)(ns / 1000000), (unsigned long long)((ns / 100000) % 10), check); }
#include "bench_core.inc"
int main() { bench_core::run_all(); }
CPP
g++ -std=c++20 -O2 -I"$here/metal" -o "$tmp/b" "$tmp/b.cpp" -lpthread
"$tmp/b"
rm -rf "$tmp"
