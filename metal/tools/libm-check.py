#!/usr/bin/env python3
"""Check the bare-metal libm (metal/libm_portable.inc) against the host's.

Compiles the kernels' shared math source verbatim on the host in a namespace
(sqrt supplied by the host - the one architecture-specific line), and compares every function over dense grids plus the
edge cases scene code actually hits. Prints the worst error per function.

    metal/tools/libm-check.py
"""
import os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
# The shared libm both kernels include; each kernel supplies sqrt/sqrtf with
# its own instruction, so the host build supplies them with the host's.
INC = os.path.join(HERE, "..", "libm_portable.inc")
body = ("double sqrt(double x) { return __builtin_sqrt(x); }\n"
        "float sqrtf(float x) { return __builtin_sqrtf(x); }\n" + open(INC).read())

harness = r'''
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
namespace mm {
%s
}
struct Worst { const char* name; double err = 0, at = 0, got = 0, want = 0; long bad = 0; long n = 0; };
static bool same_special(double a, double b) {
  if (std::isnan(a) && std::isnan(b)) return true;
  if (std::isinf(a) || std::isinf(b)) return a == b;
  return false;
}
template <class F, class G>
static void check1(const char* name, F mine, G ref, const std::vector<double>& xs, double tol) {
  Worst w; w.name = name;
  for (double x : xs) {
    const double got = mine(x), want = ref(x);
    ++w.n;
    double err;
    if (same_special(got, want)) err = 0;
    else if (std::isnan(got) || std::isnan(want) || std::isinf(got) || std::isinf(want)) err = 1e300;
    else err = std::fabs(got - want) / std::fmax(1.0, std::fabs(want));
    if (err > tol) ++w.bad;
    if (err > w.err) { w.err = err; w.at = x; w.got = got; w.want = want; }
  }
  std::printf("%%-8s worst rel err %%-10.3g at x=%%-14.8g got %%-14.8g want %%-14.8g  bad %%ld/%%ld%%s\n",
              name, w.err, w.at, w.got, w.want, w.bad, w.n, w.bad ? "   <-- FAIL" : "");
}
template <class F, class G>
static void check2(const char* name, F mine, G ref, const std::vector<std::pair<double,double>>& xs, double tol) {
  Worst w; w.name = name; double at2 = 0;
  for (auto [a, b] : xs) {
    const double got = mine(a, b), want = ref(a, b);
    ++w.n;
    double err;
    if (same_special(got, want)) err = 0;
    else if (std::isnan(got) || std::isnan(want) || std::isinf(got) || std::isinf(want)) err = 1e300;
    else err = std::fabs(got - want) / std::fmax(1.0, std::fabs(want));
    if (err > tol) ++w.bad;
    if (err > w.err) { w.err = err; w.at = a; at2 = b; w.got = got; w.want = want; }
  }
  std::printf("%%-8s worst rel err %%-10.3g at (%%g, %%g) got %%-14.8g want %%-14.8g  bad %%ld/%%ld%%s\n",
              name, w.err, w.at, at2, w.got, w.want, w.bad, w.n, w.bad ? "   <-- FAIL" : "");
}
int main() {
  std::vector<double> g, pos, unit, big;
  for (double x = -50; x <= 50; x += 0.0137) g.push_back(x);
  for (double x : {0.0, -0.0, 1e-300, -1e-300, 1e300, -1e300, 700.0, 709.5, -700.0, -709.0, -720.0, -740.0, -744.0, -746.0, 1e6, -1e6, 1e12, (double)INFINITY, -(double)INFINITY, (double)NAN}) g.push_back(x);
  for (double x = 1e-6; x <= 1e6; x *= 1.013) pos.push_back(x);
  for (double x : {0.0, 1e-310, 1.0, 2.0, 0.5, 1e300, (double)INFINITY, (double)NAN, -1.0}) pos.push_back(x);
  for (double x = -1.0; x <= 1.0; x += 0.0007) unit.push_back(x);
  for (double x = -2000; x <= 2000; x += 0.77) big.push_back(x);
  const double T = 1e-9;
  check1("sin", [](double x){ return mm::sin(x); }, [](double x){ return std::sin(x); }, g, T);
  check1("cos", [](double x){ return mm::cos(x); }, [](double x){ return std::cos(x); }, g, T);
  check1("sin-big", [](double x){ return mm::sin(x); }, [](double x){ return std::sin(x); }, big, T);
  check1("tan", [](double x){ return mm::tan(x); }, [](double x){ return std::tan(x); }, unit, 1e-7);
  check1("exp", [](double x){ return mm::exp(x); }, [](double x){ return std::exp(x); }, g, T);
  check1("log", [](double x){ return mm::log(x); }, [](double x){ return std::log(x); }, pos, T);
  check1("atan", [](double x){ return mm::atan(x); }, [](double x){ return std::atan(x); }, g, T);
  check1("asin", [](double x){ return mm::asin(x); }, [](double x){ return std::asin(x); }, unit, T);
  check1("acos", [](double x){ return mm::acos(x); }, [](double x){ return std::acos(x); }, unit, T);
  check1("tanh", [](double x){ return mm::tanh(x); }, [](double x){ return std::tanh(x); }, g, T);
  check1("floor", [](double x){ return mm::floor(x); }, [](double x){ return std::floor(x); }, g, 0);
  check1("ceil", [](double x){ return mm::ceil(x); }, [](double x){ return std::ceil(x); }, g, 0);
  check1("round", [](double x){ return mm::round(x); }, [](double x){ return std::round(x); }, g, 0);
  std::vector<std::pair<double,double>> pw, a2, fm;
  for (double a : {0.0, 0.5, 1.0, 2.0, 10.0, 1e-3, 123.456, -2.0, -8.0}) for (double b : {0.0, 1.0, 2.0, 0.5, -1.0, 3.0, 2.2, 1.0/3, -0.5, 10.0}) pw.push_back({a, b});
  for (double y = -3; y <= 3; y += 0.37) for (double x = -3; x <= 3; x += 0.41) a2.push_back({y, x});
  a2.push_back({0, 0}); a2.push_back({1, 0}); a2.push_back({-1, 0}); a2.push_back({0, -1});
  for (double a = -20; a <= 20; a += 0.93) for (double b : {1.0, 2.5, -3.0, 0.7, 6.283185307179586}) fm.push_back({a, b});
  check2("pow", [](double a, double b){ return mm::pow(a, b); }, [](double a, double b){ return std::pow(a, b); }, pw, 1e-9);
  check2("atan2", [](double a, double b){ return mm::atan2(a, b); }, [](double a, double b){ return std::atan2(a, b); }, a2, T);
  check2("fmod", [](double a, double b){ return mm::fmod(a, b); }, [](double a, double b){ return std::fmod(a, b); }, fm, 1e-12);
  return 0;
}
''' % body

with tempfile.TemporaryDirectory() as d:
    c = os.path.join(d, "t.cpp"); exe = os.path.join(d, "t")
    open(c, "w").write(harness)
    r = subprocess.run(["g++", "-std=c++17", "-O1", "-fno-builtin", "-w", c, "-o", exe], capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[:4000]); sys.exit(1)
    subprocess.run([exe])
