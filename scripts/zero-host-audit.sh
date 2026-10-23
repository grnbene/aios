#!/usr/bin/env bash
# AOS-010 forensic instrument. Compiles the kernel core and lists every symbol
# it needs from outside itself, grouped by who provides it today. The kernel
# core is everything under src/ except the host boundary (src/hal) and the
# host-specific intelligence adapters. `--gate` fails when the core needs
# anything outside the freestanding allowance listed in this script: mem/str
# primitives, the C allocator, libm (every function the bare-metal kernels
# supply in metal/libm_portable.inc), the C++ runtime.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd); build=${BUILD:-$here/build}
cmake --build "$build" --target aios >/dev/null
core=$(find "$build/CMakeFiles/aios.dir/src" -name '*.o' | grep -vE "src/hal/|src/media/|src/userland/|src/intelligence/(plugin|external_process)\.cpp\.o|src/compute/opencl_device\.cpp\.o")
tmp=$(mktemp); nm -u --demangle $core | sed -E 's/^ *[Uw] //' | grep -vE '^aios::|:$|^$' | sort -u > "$tmp"
grp() { echo; echo "## $1"; (grep -E "$2" "$tmp" || true) | sed -E "s/\(.*//; s/<.*//" | sort -u | sed "s/^/  /"; }
echo "kernel core: $(echo "$core" | wc -l) objects, $(wc -l < "$tmp") external symbols"
grp "POSIX / host threads (must be behind hal::)"      '^(pthread_|std::thread|std::this_thread|std::(__cxx11::)?(mutex|condition_variable))'
grp "libc string/memory primitives (freestanding: trivial)" '^(mem(cpy|move|set|cmp|chr)|__memcpy_chk|str(len|cmp|ncmp|chr|nlen))$'
grp "libm (freestanding: metal/libm_portable.inc)"   '^(exp|expf|expm1|log|log2|logf|log2f|log10|log1p|sqrt|sqrtf|cbrt|pow|powf|fabs|floor|ceil|trunc|nearbyint|nextafter|fmod|frexp|sinf?|cosf?|tanf?|tanhf?|sinh|cosh|asinh|acosh|atanh|asin|acos|atan|atan2f?)$'
grp "libc runtime (parsing / formatting / misc)"      '^(__isoc23_|strto|snprintf|vsnprintf|__vsnprintf_chk|sscanf|abort|__assert|__stack_chk|__errno|getenv|qsort|bsearch)'
grp "iostream / locale (must leave the core)"         '^std::(__cxx11::)?(basic_(i|o)?stream|basic_ios|basic_stringbuf|basic_stringstream|basic_istringstream|basic_ostringstream|ios_base|locale|cout|cerr|endl)'
grp "libstdc++ runtime (containers, exceptions, abi)"  '^(std::|__cxa|__gxx|_Unwind|operator new|operator delete|typeinfo|vtable|VTT|__dynamic_cast|__cxxabiv1|guard variable|__dso_handle|__tls_get_addr|_ITM|__gmon|__libc_single_threaded)'
other=$(grep -vE '^$|^_GLOBAL_OFFSET_TABLE_$|^operator |^(pthread___libc_single_threaded|mem(cpy|move|set|cmp|chr)$|__memcpy_chk$|__memset_chk$|(malloc|calloc|realloc|free)$|(expm1|log10|log1p|cbrt|trunc|nearbyint|nextafter|fmod|frexp|tan|sinh|cosh|asinh|acosh|atanh|asin|acos|atan)$|str(len|cmp|ncmp|chr|nlen)$|expf?$|logf?$|log2f?$|sqrtf?$|powf?$|fabs$|floor$|ceil$|sinf?$|cosf?$|atan2f?$|tanhf?$|is(alnum|alpha|space|digit|upper|lower|xdigit|punct|print)$|to(lower|upper)$|strncpy$|__snprintf_chk$|__isoc23_|strto|snprintf|vsnprintf|__vsnprintf_chk|sscanf|abort|__assert|__stack_chk|__errno|getenv|qsort|bsearch|std::|__cxa|__gxx|_Unwind|typeinfo|vtable|VTT|__dynamic_cast|__cxxabiv1|guard variable|__dso_handle|__tls_get_addr|_ITM|__gmon|__libc_single_threaded)' "$tmp" || true)
echo; echo "## unclassified"; [ -n "$other" ] && echo "$other" | sed 's/^/  /' || echo "  (none)"
if [ "${1:-}" = "--gate" ]; then
  bad=$(grep -E '^(pthread_|std::thread|std::this_thread|std::(__cxx11::)?(basic_(i|o)?stream|basic_ios|basic_stringbuf|basic_stringstream|ios_base|locale)|__isoc23_|strto|sscanf|getenv)' "$tmp" || true)
  if [ -n "$bad" ] || [ -n "$other" ]; then echo; echo "ZERO-HOST GATE: FAIL — the kernel core reaches the host directly:"; printf '%s\n%s\n' "$bad" "$other" | sed '/^$/d;s/^/  /'; rm -f "$tmp"; exit 1; fi
  echo; echo "ZERO-HOST GATE: PASS — the core needs only the freestanding allowance (mem/str primitives, libm, allocator, C++ runtime)."
fi
rm -f "$tmp"
