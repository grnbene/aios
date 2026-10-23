#ifndef AIOS_SIDECAR_C_ABI_H
#define AIOS_SIDECAR_C_ABI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Status Codes
 * ------------------------------------------------------------------------- */
#define AIOS_OK 0
#define AIOS_ERR_INVALID 1
#define AIOS_ERR_NOT_FOUND 2
#define AIOS_ERR_DENIED 3
#define AIOS_ERR_BUFFER_TOO_SMALL 4
#define AIOS_ERR_EXECUTION 5

/* -------------------------------------------------------------------------
 * 1. Blockchain & Transaction Construction Pipeline
 * ------------------------------------------------------------------------- */

/* Evaluate Bitcoin script stack against a script pubkey */
int aios_chain_eval_script(const uint8_t* script_sig, size_t sig_len,
                           const uint8_t* script_pubkey, size_t pk_len,
                           int* out_is_valid);

/* Derive bech32 SegWit (p2wpkh) Bitcoin address from 33-byte compressed pubkey */
int aios_chain_btc_p2wpkh_address(const uint8_t pubkey[33], int testnet,
                                  char* out_addr, size_t max_len);

/* Construct and sign a Bitcoin SegWit p2wpkh spend transaction */
int aios_chain_btc_create_p2wpkh_tx(const uint8_t prev_txid[32], uint32_t prev_vout,
                                    uint64_t amount_sats, const char* dest_addr,
                                    uint64_t fee_sats, const uint8_t privkey[32],
                                    int testnet, uint8_t* out_tx, size_t* out_len);

/* Build and sign an Ethereum transaction (EIP-1559 or legacy) and return raw hex & txhash */
int aios_chain_eth_sign_tx(uint64_t nonce, uint64_t gas_limit, const uint8_t to[20],
                           const uint8_t* value_be, size_t val_len,
                           const uint8_t* data, size_t data_len, uint64_t chain_id,
                           const uint8_t privkey[32], uint8_t* out_raw_tx, size_t* out_len,
                           char* out_txhash_hex);

/* Derive checksummed 0x Ethereum address from private key */
int aios_chain_eth_derive_address(const uint8_t privkey[32], char* out_addr, size_t max_len);

/* -------------------------------------------------------------------------
 * 2. High-Throughput Communications & Feeds Ingestion Suite
 * ------------------------------------------------------------------------- */

/* Parse RFC 5322 MIME email (.eml) into JSON string */
int aios_comms_parse_eml_json(const char* raw_eml, char* out_json, size_t max_len);

/* Parse RFC 5545 iCalendar (.ics) into JSON string of events */
int aios_comms_parse_ical_json(const char* ics_data, char* out_json, size_t max_len);

/* Parse RFC 6350 vCard (.vcf) into JSON string of contact cards */
int aios_comms_parse_vcard_json(const char* vcf_data, char* out_json, size_t max_len);

/* Parse RSS / Atom XML feeds into JSON string of items */
int aios_comms_parse_rss_json(const char* xml_data, char* out_json, size_t max_len);

/* -------------------------------------------------------------------------
 * 3. Cryptographic Object-Capability Authorization Ledger
 * ------------------------------------------------------------------------- */

/* Capabilities are signed tokens from the sidecar CapabilityGuard. A cap id is
 * the hex of the 48-byte token (AIOS_CAP_ID_LEN chars), bound to subject and
 * resource, HMAC-signed with AIOS_CAPABILITY_KEY when set (tokens then verify in
 * any process sharing the key), else with a random per-process key.
 * Rights are the guard's bits: 1 read, 2 write, 4 execute, 8 delete,
 * 16 delegate, 32 admin. */
#define AIOS_CAP_ID_LEN 96

/* Mint a capability for subject on resource with the given rights. actor_id is
 * required but advisory: holding the key is the authority. */
int aios_cap_mint(const char* actor_id, const char* subject_id, const char* resource,
                  uint16_t rights, char* out_cap_id, size_t max_len);

/* *out_valid = 1 when cap_id is genuine, unexpired, unrevoked, bound to this
 * subject and resource, and holds every required right; else 0. */
int aios_cap_verify(const char* cap_id, const char* subject_id, const char* resource,
                    uint16_t required_rights, int* out_valid);

/* Derive a child holding a subset of the parent's rights. The parent must hold
 * delegate (16); AIOS_ERR_DENIED otherwise, or when asking for rights it lacks. */
int aios_cap_attenuate(const char* parent_cap_id, uint16_t narrowed_rights,
                       char* out_child_cap_id, size_t max_len);

/* Revoke every capability minted so far for subject on resource (and children). */
int aios_cap_revoke(const char* subject_id, const char* resource);

/* -------------------------------------------------------------------------
 * 4. Binary Entity Checkpoint & Hot-State Snapshotting
 * ------------------------------------------------------------------------- */

/* Package an entity payload into an AOKC v2 binary snapshot container */
int aios_checkpoint_serialize(const char* entity_id, const uint8_t* payload, size_t len,
                              uint8_t* out_ckpt, size_t* out_len);

/* Validate and inspect an AOKC snapshot header, checksum, and payload length */
int aios_checkpoint_validate(const uint8_t* ckpt_data, size_t len,
                             int* out_valid, uint32_t* out_version, size_t* out_body_len);

/* -------------------------------------------------------------------------
 * 5. Headless Vector SVG & Image Rasterizer
 * ------------------------------------------------------------------------- */

/* Rasterize an SVG XML string directly into a PNG image binary */
int aios_svg_render_png(const char* svg_xml, int width, int height,
                        uint32_t current_color_argb, uint8_t* out_png, size_t* out_len);

/* Rasterize an SVG XML string into raw 32-bit ARGB pixel buffer */
int aios_svg_render_rgba(const char* svg_xml, int width, int height,
                         uint32_t current_color_argb, uint32_t* out_argb, size_t max_pixels);

/* -------------------------------------------------------------------------
 * 6. Headless Vector PDF Layout Engine
 * ------------------------------------------------------------------------- */

/* Render HTML & CSS into a compliant PDF 1.4 binary stream */
int aios_pdf_render_html(const char* html, const char* css, int viewport_w,
                         uint8_t* out_pdf, size_t* out_len);

/* -------------------------------------------------------------------------
 * 7. Combinatorial TSP & Fleet Dispatch
 * ------------------------------------------------------------------------- */

/* Compute exact Haversine great-circle distance in meters between two coordinates */
double aios_haversine_distance(double lat1, double lng1, double lat2, double lng2);

/* Solve a closed Traveling Salesperson tour over lat/lng pairs: point 0 is the
 * start and the tour returns to it. Nearest-neighbour seed, then 2-opt and
 * Or-opt (erp::route_improve). out_order receives all `count` indices, each
 * once, starting with 0; *out_distance_meters is the closed tour length. */
int aios_tsp_solve_2opt(const double* coords_latlng, int count,
                        int* out_order, double* out_distance_meters);

/* -------------------------------------------------------------------------
 * 8. Sandboxed Bytecode VM (Grain)
 * ------------------------------------------------------------------------- */

/* Execute Grain source code within strict instruction step limits */
int aios_grain_run(const char* source_code, uint64_t step_limit,
                   int64_t* out_int_result, double* out_real_result,
                   char* out_output_buf, size_t max_out_len,
                   char* out_err_buf, size_t max_err_len);

/* -------------------------------------------------------------------------
 * 9. Multi-File Patch Transaction Pre-flight Check
 * ------------------------------------------------------------------------- */

/* Dry-run a unified diff against the files on disk, relative to root_dir
 * (NULL or "" = the working directory). *out_can_apply = 1 only when every
 * hunk's context and deleted lines match the file as it is now; on a mismatch
 * it is 0, the call still returns AIOS_OK, and out_err says where. A diff that
 * does not parse returns AIOS_ERR_INVALID. Nothing is written. */
int aios_patch_check_dry_run_ex(const char* unified_diff, const char* root_dir,
                                int* out_hunk_count, int* out_file_count, int* out_can_apply,
                                char* out_err, size_t max_err_len);

/* As _ex against the working directory, with no error text. */
int aios_patch_check_dry_run(const char* unified_diff, int* out_hunk_count,
                             int* out_file_count, int* out_can_apply);

/* -------------------------------------------------------------------------
 * 10. Freestanding Native Cryptography Primitives
 * ------------------------------------------------------------------------- */

void aios_crypto_sha256(const uint8_t* data, size_t len, uint8_t out[32]);
void aios_crypto_keccak256(const uint8_t* data, size_t len, uint8_t out[32]);
int  aios_crypto_secp256k1_sign(const uint8_t privkey[32], const uint8_t msg_hash[32],
                                uint8_t out_sig[64], int* out_recid);
int  aios_crypto_secp256k1_recover(const uint8_t sig[64], int recid,
                                   const uint8_t msg_hash[32], uint8_t out_pubkey[65]);
void aios_crypto_ed25519_sign(const uint8_t privkey[32], const uint8_t* data, size_t len,
                              uint8_t out_sig[64]);
int  aios_crypto_ed25519_verify(const uint8_t pubkey[32], const uint8_t* data, size_t len,
                                const uint8_t sig[64]);

/* -------------------------------------------------------------------------
 * 11. Binary Telemetry & Trace Inspection ABI
 * ------------------------------------------------------------------------- */

/* Record a fast binary telemetry span directly via C-ABI */
int aios_telemetry_record(uint64_t trace_id, uint64_t span_id, uint32_t service_id,
                          uint16_t event_type, uint16_t status_code, uint32_t duration_us,
                          uint32_t payload_int, const char* tag);

/* Query full trace event execution path for an agent task */
int aios_telemetry_get_trace_json(uint64_t trace_id, char* out_json, size_t max_len);

/* Compute rolling window statistics for Git Agent operations */
int aios_telemetry_git_stats(uint64_t window_seconds, 
                             uint64_t* out_ops_count, 
                             uint64_t* out_errors,
                             double* out_avg_us, 
                             uint32_t* out_p99_us);

#ifdef __cplusplus
}
#endif

#endif /* AIOS_SIDECAR_C_ABI_H */
