/* reflect: a native intelligence built against the C ABI.
 *
 * perceive  copies the entity's context bytes into a memory object it owns
 * think     emits a compute graph the KERNEL schedules:
 *             histogram(u8 bytes)   -> f32[256]
 *             normalize             -> f32[256]  (byte distribution)
 *             matmul(dist · W)      -> f32[1x16] (projection, W hash-derived)
 *             softmax               -> f32[1x16] (attention over the projection)
 * act       reads the computed tensors back and produces the next state.
 *
 * It knows nothing about processes, files or the host; only the surface.
 */
#include "aios/intelligence/abi.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

typedef struct {
  aios_id dist, att, emb, hist, w;
  uint64_t n_bytes;
  uint64_t cycles;
} reflect_ctx;

static int is_zero(aios_id id) { return id.hi == 0 && id.lo == 0; }

static int perceive(void* vctx, aios_surface* s, const aios_surface_ops* ops, aios_id context, aios_id* out) {
  reflect_ctx* ctx = (reflect_ctx*)vctx;
  uint64_t n = 0;
  int rc = ops->state_payload(s, context, NULL, 0, &n);
  if (rc) return rc;
  uint64_t alloc = n ? n : 1;
  unsigned char* buf = (unsigned char*)malloc(alloc);
  if (!buf) return AIOS_EXHAUSTED;
  memset(buf, 0, alloc);
  if (n) { rc = ops->state_payload(s, context, buf, n, &n); if (rc) { free(buf); return rc; } }
  aios_id mem;
  rc = ops->mem_alloc(s, alloc, &mem);
  if (rc) { free(buf); return rc; }
  rc = ops->mem_write(s, mem, 0, buf, alloc);
  free(buf);
  if (rc) return rc;
  rc = ops->state_create(s, AIOS_STATE_CONTEXT, NULL, 0, out);
  if (rc) return rc;
  rc = ops->state_attach_memory(s, *out, mem);
  if (rc) return rc;
  ctx->n_bytes = alloc;
  return AIOS_OK;
}

static void tensor(aios_tensor* t, aios_id mem, uint8_t dtype, uint32_t rank, uint64_t d0, uint64_t d1) {
  memset(t, 0, sizeof *t);
  t->memory = mem; t->dtype = dtype; t->rank = rank; t->shape[0] = d0; t->shape[1] = d1;
}

static int add_op(aios_surface* s, const aios_surface_ops* ops, aios_id g, uint16_t code, const aios_tensor* in, uint32_t nin, const aios_tensor* out, uint32_t nout, uint64_t flops) {
  aios_op op;
  memset(&op, 0, sizeof op);
  op.code = code; op.device = AIOS_DEV_ANY; op.parallelism = 4; op.priority = 100; op.flops = flops;
  op.n_inputs = nin; op.inputs = in; op.n_outputs = nout; op.outputs = out;
  aios_id oid;
  return ops->graph_add_op(s, g, &op, &oid);
}

static int think(void* vctx, aios_surface* s, const aios_surface_ops* ops, aios_id perceived, aios_id* out_graph) {
  reflect_ctx* ctx = (reflect_ctx*)vctx;
  aios_id mems[4]; uint32_t count = 0;
  int rc = ops->state_memory(s, perceived, mems, 4, &count);
  if (rc) return rc;
  if (count == 0) { out_graph->hi = out_graph->lo = 0; return AIOS_OK; }
  aios_id bytes = mems[0];
  uint64_t n = 0;
  rc = ops->mem_size(s, bytes, &n);
  if (rc) return rc;
  ctx->n_bytes = n;

  /* Working tensors for this cycle. */
  if ((rc = ops->mem_alloc(s, 256 * 4, &ctx->hist))) return rc;
  if ((rc = ops->mem_alloc(s, 256 * 4, &ctx->dist))) return rc;
  if ((rc = ops->mem_alloc(s, 256 * 16 * 4, &ctx->w))) return rc;
  if ((rc = ops->mem_alloc(s, 16 * 4, &ctx->emb))) return rc;
  if ((rc = ops->mem_alloc(s, 16 * 4, &ctx->att))) return rc;
  /* A fixed projection: deterministic, hash-derived weights in [-1,1]. */
  float* w = (float*)malloc(256 * 16 * sizeof(float));
  if (!w) return AIOS_EXHAUSTED;
  for (int i = 0; i < 256 * 16; ++i) {
    uint32_t h = (uint32_t)i * 2654435761u ^ 0x9E3779B9u;
    h ^= h >> 15; h *= 0x85EBCA6Bu; h ^= h >> 13;
    w[i] = ((float)(h & 0xFFFF) / 32767.5f) - 1.0f;
  }
  rc = ops->mem_write(s, ctx->w, 0, w, 256 * 16 * sizeof(float));
  free(w);
  if (rc) return rc;

  aios_id g;
  if ((rc = ops->graph_create(s, &g))) return rc;
  aios_tensor tb, th, td, tw, te, ta;
  tensor(&tb, bytes, AIOS_U8, 1, n, 0);
  tensor(&th, ctx->hist, AIOS_F32, 1, 256, 0);
  tensor(&td, ctx->dist, AIOS_F32, 1, 256, 0);
  tensor(&tw, ctx->w, AIOS_F32, 2, 256, 16);
  tensor(&te, ctx->emb, AIOS_F32, 2, 1, 16);
  tensor(&ta, ctx->att, AIOS_F32, 2, 1, 16);
  aios_tensor td2; tensor(&td2, ctx->dist, AIOS_F32, 2, 1, 256);
  aios_tensor mm_in[2] = {td2, tw};
  if ((rc = add_op(s, ops, g, AIOS_OP_HISTOGRAM, &tb, 1, &th, 1, n))) return rc;
  if ((rc = add_op(s, ops, g, AIOS_OP_NORMALIZE, &th, 1, &td, 1, 512))) return rc;
  if ((rc = add_op(s, ops, g, AIOS_OP_MATMUL, mm_in, 2, &te, 1, 2 * 256 * 16))) return rc;
  if ((rc = add_op(s, ops, g, AIOS_OP_SOFTMAX, &te, 1, &ta, 1, 64))) return rc;
  *out_graph = g;
  return AIOS_OK;
}

static int act(void* vctx, aios_surface* s, const aios_surface_ops* ops, aios_id perceived, aios_id graph, uint64_t ops_run, int complete, aios_id* out) {
  reflect_ctx* ctx = (reflect_ctx*)vctx;
  (void)perceived;
  char text[1024];
  int len;
  if (is_zero(graph) || !complete) {
    len = snprintf(text, sizeof text, "reflect: nothing to compute (cycle %llu)", (unsigned long long)ctx->cycles + 1);
  } else {
    float dist[256], att[16], emb[16];
    int rc;
    if ((rc = ops->mem_read(s, ctx->dist, 0, dist, sizeof dist))) return rc;
    if ((rc = ops->mem_read(s, ctx->att, 0, att, sizeof att))) return rc;
    if ((rc = ops->mem_read(s, ctx->emb, 0, emb, sizeof emb))) return rc;
    double entropy = 0.0; int top = 0;
    for (int i = 0; i < 256; ++i) { if (dist[i] > 0) entropy -= dist[i] * log2((double)dist[i]); if (dist[i] > dist[top]) top = i; }
    int focus = 0; for (int i = 1; i < 16; ++i) if (att[i] > att[focus]) focus = i;
    len = snprintf(text, sizeof text,
      "reflect: %llu bytes, %llu ops on the kernel's compute fabric, entropy %.3f bits, dominant byte 0x%02x%s%c%s, focus lane %d (%.3f), embedding[0..3]=%.3f %.3f %.3f %.3f",
      (unsigned long long)ctx->n_bytes, (unsigned long long)ops_run, entropy, top,
      (top >= 32 && top < 127) ? " '" : "", (top >= 32 && top < 127) ? (char)top : ' ', (top >= 32 && top < 127) ? "'" : "",
      focus, att[focus], emb[0], emb[1], emb[2], emb[3]);
  }
  if (len < 0) len = 0;
  int rc = ops->state_create(s, AIOS_STATE_WORKING, text, (uint64_t)len, out);
  if (rc) return rc;
  if (!is_zero(ctx->att)) ops->state_attach_memory(s, *out, ctx->att);
  ctx->cycles++;
  return AIOS_OK;
}

static void destroy(aios_intelligence* self) { free(self->ctx); free(self); }

aios_intelligence* aios_intelligence_create(void) {
  aios_intelligence* m = (aios_intelligence*)calloc(1, sizeof *m);
  if (!m) return NULL;
  m->abi_version = AIOS_ABI_VERSION;
  m->name = "reflect";
  m->ctx = calloc(1, sizeof(reflect_ctx));
  m->perceive = perceive; m->think = think; m->act = act; m->destroy = destroy;
  return m;
}
