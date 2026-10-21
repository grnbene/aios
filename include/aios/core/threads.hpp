#pragma once
// Where a thread's body runs inline, to completion, when it is started: the bare-metal image (its
// pthread_create) and WebAssembly (one thread). Code that would start a thread to wait for work
// forever takes its serial path there instead.
#if defined(AIOS_METAL) || defined(__wasi__)
#define AIOS_INLINE_THREADS 1
#endif
