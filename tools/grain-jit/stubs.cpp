// qemu test build only: the sidecar C ABI (crypto/pdf/svg/tsp) is not linked;
// grain natives that use it report failure here. Native-vs-interpreted
// comparison runs against the same stubs on both sides.
extern "C" {
int aios_chain_btc_p2wpkh_address() { return -1; }
int aios_chain_eth_derive_address() { return -1; }
int aios_comms_parse_eml_json() { return -1; }
int aios_comms_parse_ical_json() { return -1; }
int aios_comms_parse_vcard_json() { return -1; }
int aios_comms_parse_rss_json() { return -1; }
int aios_cap_mint() { return -1; }
int aios_cap_attenuate() { return -1; }
int aios_svg_render_png() { return -1; }
int aios_pdf_render_html() { return -1; }
double aios_haversine_distance() { return 0; }
int aios_tsp_solve_2opt() { return -1; }
int aios_patch_check_dry_run() { return -1; }
void aios_crypto_sha256() {}
void aios_crypto_keccak256() {}
}
