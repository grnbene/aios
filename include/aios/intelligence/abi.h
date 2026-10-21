/* AIOS Intelligence ABI, version 1.
 *
 * This is the whole contract between the OS and whatever occupies the
 * intelligence slot. The kernel asks an intelligence to PERCEIVE state, to
 * THINK (produce a compute graph the kernel schedules), and to ACT (produce the
 * next state from what was computed). It does not know or care what the
 * intelligence is. Plugins export `aios_intelligence_create`.
 */
#ifndef AIOS_INTELLIGENCE_ABI_H
#define AIOS_INTELLIGENCE_ABI_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define AIOS_ABI_VERSION 1u

typedef struct aios_id { uint64_t hi, lo; } aios_id;
typedef struct aios_surface aios_surface; /* opaque: the kernel API bound to one entity */

/* Status codes mirror aios::Status. */
enum { AIOS_OK = 0, AIOS_NOT_FOUND = 1, AIOS_DENIED = 2, AIOS_EXHAUSTED = 3, AIOS_INVALID = 4, AIOS_CONFLICT = 5, AIOS_UNSUPPORTED = 6, AIOS_IO = 7, AIOS_CORRUPT = 8, AIOS_BUSY = 9, AIOS_EXPIRED = 10 };

/* Tensor dtypes mirror aios::DType. */
enum { AIOS_F32 = 0, AIOS_F64 = 1, AIOS_I32 = 2, AIOS_I64 = 3, AIOS_U8 = 4 };
/* State kinds mirror aios::StateKind. */
enum { AIOS_STATE_WORKING = 0, AIOS_STATE_PERSISTENT = 1, AIOS_STATE_CONTEXT = 2, AIOS_STATE_RELATIONSHIP = 3, AIOS_STATE_EXECUTION = 4 };
/* Opcodes mirror aios::OpCode. */
enum { AIOS_OP_NOP = 0, AIOS_OP_COPY = 1, AIOS_OP_FILL = 2, AIOS_OP_ADD = 3, AIOS_OP_MUL = 4, AIOS_OP_SCALE = 5, AIOS_OP_MATMUL = 6, AIOS_OP_REDUCE_SUM = 7, AIOS_OP_RELU = 8, AIOS_OP_SOFTMAX = 9, AIOS_OP_HISTOGRAM = 10, AIOS_OP_NORMALIZE = 11, AIOS_OP_CUSTOM = 100 };
/* Device types mirror aios::DeviceType. */
enum { AIOS_DEV_ANY = 0, AIOS_DEV_CPU = 1, AIOS_DEV_GPU = 2, AIOS_DEV_NPU = 3 };

typedef struct aios_tensor {
  aios_id memory;
  uint8_t dtype;
  uint64_t offset;
  uint32_t rank;
  uint64_t shape[8];
} aios_tensor;

typedef struct aios_op {
  uint16_t code;
  uint8_t device;
  uint32_t parallelism;
  uint32_t priority;
  uint64_t memory;      /* bytes required resident */
  uint64_t flops;       /* estimate */
  double scalar;
  uint32_t n_inputs, n_outputs;
  const aios_tensor* inputs;
  const aios_tensor* outputs;
  uint32_t n_depends;
  const aios_id* depends;
} aios_op;

/* The kernel API exposed to an intelligence, always exercised as the bound
 * entity and always capability-checked. */
typedef struct aios_surface_ops {
  int (*mem_alloc)(aios_surface*, uint64_t size, aios_id* out);
  int (*mem_write)(aios_surface*, aios_id mem, uint64_t off, const void* data, uint64_t n);
  int (*mem_read)(aios_surface*, aios_id mem, uint64_t off, void* data, uint64_t n);
  int (*mem_size)(aios_surface*, aios_id mem, uint64_t* out);
  int (*state_create)(aios_surface*, uint8_t kind, const void* payload, uint64_t n, aios_id* out);
  int (*state_payload)(aios_surface*, aios_id state, void* buf, uint64_t cap, uint64_t* full_size);
  int (*state_set_payload)(aios_surface*, aios_id state, const void* payload, uint64_t n);
  int (*state_attach_memory)(aios_surface*, aios_id state, aios_id mem);
  int (*state_memory)(aios_surface*, aios_id state, aios_id* out, uint32_t cap, uint32_t* count);
  int (*graph_create)(aios_surface*, aios_id* out);
  int (*graph_add_op)(aios_surface*, aios_id graph, const aios_op* op, aios_id* out);
  uint64_t (*now_ns)(aios_surface*);
  aios_id (*self)(aios_surface*);
} aios_surface_ops;

typedef struct aios_intelligence {
  uint32_t abi_version;
  const char* name;
  void* ctx;
  int (*perceive)(void* ctx, aios_surface* s, const aios_surface_ops* ops, aios_id context_state, aios_id* out_state);
  int (*think)(void* ctx, aios_surface* s, const aios_surface_ops* ops, aios_id perceived_state, aios_id* out_graph); /* out_graph zero = nothing to compute */
  int (*act)(void* ctx, aios_surface* s, const aios_surface_ops* ops, aios_id perceived_state, aios_id graph, uint64_t ops_run, int complete, aios_id* out_state);
  void (*destroy)(struct aios_intelligence* self);
} aios_intelligence;

typedef aios_intelligence* (*aios_intelligence_create_fn)(void);
/* Every plugin exports exactly this symbol. */
#define AIOS_INTELLIGENCE_ENTRY "aios_intelligence_create"

#ifdef __cplusplus
}
#endif
#endif
