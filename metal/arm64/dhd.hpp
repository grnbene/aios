#pragma once
/* Broadcom / Cypress BCM43438 Dongle Host Driver (DHD) Protocol Layer (metal/arm64/dhd.hpp).
 * SDPCM framing, BCDC ioctl control channel, WLC wireless commands,
 * asynchronous event decoding, and 802.3 frame transport over SDIO Function 2.
 *
 * Every number and layout below is the firmware's contract (wlioctl.h /
 * brcmfmac fwil_types.h). They are not adjustable: a wrong ioctl number is
 * silently a DIFFERENT command, and the firmware answers it.
 */
#include <cstddef>
#include <cstdint>

/* SDPCM (SDIO Packet Communication Manager) Protocol Constants */
constexpr uint8_t SDPCM_CHANNEL_CONTROL = 0;
constexpr uint8_t SDPCM_CHANNEL_EVENT   = 1;
constexpr uint8_t SDPCM_CHANNEL_DATA    = 2;
constexpr uint8_t SDPCM_CHANNEL_GLOM    = 3;

constexpr uint8_t SDPCM_HEADER_LEN      = 12;
constexpr uint8_t BCDC_DATA_HEADER_LEN  = 4;
constexpr uint8_t SDPCM_DATA_OFFSET     = 16; /* 12-byte SDPCM + 4-byte BCDC */

/* Largest single SDPCM frame either direction. Scan results with IEs run to
 * a few KB; the firmware's own ioctl ceiling is 8 KB. */
constexpr size_t DHD_MAX_FRAME          = 8192;   /* brcmfmac BRCMF_DCMD_MAXLEN */

struct __attribute__((packed)) SdpcmHeader {
  uint16_t framelen;       /* Total length of transfer including SDPCM header */
  uint16_t framelen_check; /* ~framelen (1's complement) */
  uint8_t  sequence;       /* Packet sequence number */
  uint8_t  channel_flags;  /* Channel (0..3) in bits 0..3 */
  uint8_t  next_len;       /* Length of next packet (or 0) */
  uint8_t  data_offset;    /* Byte offset from start of header to payload */
  uint8_t  flow_control;   /* Flow control bits */
  uint8_t  credit;         /* Flow control credits returned by dongle */
  uint16_t reserved;       /* Reserved / padding */
};

/* BCDC (Broadcom Dongle Communication) Header for Control Channel 0 */
constexpr uint32_t BCDC_FLAG_SET        = 0x00000002; /* BCDC_DCMD_SET */
constexpr uint32_t BCDC_FLAG_ERROR      = 0x00000001; /* BCDC_DCMD_ERROR */
constexpr uint32_t BCDC_FLAG_ID_MASK    = 0xFFFF0000;
constexpr uint32_t BCDC_FLAG_ID_SHIFT   = 16;
constexpr uint32_t BCDC_FLAG_IF_SHIFT   = 12;

struct __attribute__((packed)) BcdcHeader {
  uint32_t cmd;    /* WLC Command ID */
  uint32_t len;    /* Length of payload */
  uint32_t flags;  /* bit 0 = error, bit 1 = SET, bits 12..15 = ifidx, bits 16..31 = request ID */
  uint32_t status; /* Return status code (0 = success) */
};

/* BCDC Data Header for Data Channel 2 (4 bytes) */
constexpr uint8_t BCDC_PROTO_VER        = 2;
constexpr uint8_t BCDC_FLAG_VER_SHIFT   = 4;

struct __attribute__((packed)) BcdcDataHeader {
  uint8_t flags;       /* bits 7..4: protocol version (2) */
  uint8_t priority;    /* Priority (0..7) */
  uint8_t flags2;      /* bits 3..0: ifidx */
  uint8_t data_offset; /* Extra payload offset, in 32-bit words */
};

/* Standard WLC ioctl Command IDs (wlioctl.h) */
constexpr uint32_t WLC_GET_MAGIC        = 0;
constexpr uint32_t WLC_GET_VERSION      = 1;
constexpr uint32_t WLC_UP               = 2;
constexpr uint32_t WLC_DOWN             = 3;
constexpr uint32_t WLC_GET_PROMISC      = 9;
constexpr uint32_t WLC_SET_PROMISC      = 10;
constexpr uint32_t WLC_GET_RATE         = 12;
constexpr uint32_t WLC_GET_INFRA        = 19;
constexpr uint32_t WLC_SET_INFRA        = 20;
constexpr uint32_t WLC_GET_AUTH         = 21;
constexpr uint32_t WLC_SET_AUTH         = 22;
constexpr uint32_t WLC_GET_BSSID        = 23;
constexpr uint32_t WLC_SET_BSSID        = 24;
constexpr uint32_t WLC_GET_SSID         = 25;
constexpr uint32_t WLC_SET_SSID         = 26;
constexpr uint32_t WLC_GET_CHANNEL      = 29;
constexpr uint32_t WLC_SET_CHANNEL      = 30;
constexpr uint32_t WLC_GET_RADIO        = 37;
constexpr uint32_t WLC_SET_RADIO        = 38;
constexpr uint32_t WLC_GET_PASSIVE_SCAN = 48;
constexpr uint32_t WLC_SET_PASSIVE_SCAN = 49;
constexpr uint32_t WLC_SCAN             = 50;
constexpr uint32_t WLC_SCAN_RESULTS     = 51;
constexpr uint32_t WLC_DISASSOC         = 52;
constexpr uint32_t WLC_GET_COUNTRY      = 83;
constexpr uint32_t WLC_SET_COUNTRY      = 84;
constexpr uint32_t WLC_GET_REVINFO      = 98;
constexpr uint32_t WLC_GET_RSSI         = 127;
constexpr uint32_t WLC_SET_WSEC         = 134;
constexpr uint32_t WLC_SET_WPA_AUTH     = 165;
constexpr uint32_t WLC_GET_MACADDR      = 185;
constexpr uint32_t WLC_SET_MACADDR      = 186;
constexpr uint32_t WLC_GET_VAR          = 262;
constexpr uint32_t WLC_SET_VAR          = 263;
constexpr uint32_t WLC_SET_WSEC_PMK     = 268;

constexpr uint32_t BCM43438_DHD_MAGIC   = 0x14E46C77; /* WLC_IOCTL_MAGIC, wlioctl.h */

/* WLC Data Structures */
struct __attribute__((packed)) WlcSsid {
  uint32_t len;
  char ssid[32];
};

/* wl_scan_params: the SSID filter comes FIRST. bss_type is a signed byte:
 * 0 infrastructure, 1 independent, 2 any. A zeroed bssid means "only this
 * BSSID 00:00:00:00:00:00"; a broadcast scan needs ff:ff:ff:ff:ff:ff. */
struct __attribute__((packed)) WlcScanParams {
  WlcSsid  ssid;
  uint8_t  bssid[6];
  int8_t   bss_type;
  int8_t   scan_type;   /* 0 active, 1 passive */
  int32_t  nprobes;     /* -1 = firmware default */
  int32_t  active_time;
  int32_t  passive_time;
  int32_t  home_time;
  int32_t  channel_num; /* 0 = all channels */
  uint16_t channel_list[1];
};

/* wl_bss_info v109, 128 bytes, laid out with the natural alignment the
 * firmware's ARM compiler gives it. The pads are the alignment holes. */
struct __attribute__((packed)) WlcBssInfo {
  uint32_t version;
  uint32_t length;
  uint8_t  bssid[6];
  uint16_t beacon_period;
  uint16_t capability;
  uint8_t  ssid_len;
  char     ssid[32];
  uint8_t  pad0;
  uint32_t rateset_count;
  uint8_t  rateset_rates[16];
  uint16_t chanspec;
  uint16_t atim_window;
  uint8_t  dtim_period;
  uint8_t  pad1;
  int16_t  rssi;
  int8_t   phy_noise;
  uint8_t  n_cap;
  uint8_t  pad2[2];
  uint32_t nbss_cap;
  uint8_t  ctl_ch;
  uint8_t  pad3[3];
  uint32_t reserved32;
  uint8_t  flags;
  uint8_t  reserved[3];
  uint8_t  basic_mcs[16];
  uint16_t ie_offset;
  uint8_t  pad4[2];
  uint32_t ie_length;
  int16_t  snr;
  uint8_t  pad5[2];
};
static_assert(sizeof(WlcBssInfo) == 128, "wl_bss_info v109 is 128 bytes");

struct __attribute__((packed)) WlcScanResults {
  uint32_t buflen;
  uint32_t version;
  uint32_t count;
  WlcBssInfo bss_info[1];
};

/* Broadcom asynchronous event, AFTER dhd_rx_poll has stripped the Ethernet
 * and BCM event-frame headers and converted every field from network byte
 * order to host order. */
struct __attribute__((packed)) BcmEventMsg {
  uint16_t version;
  uint16_t flags;      /* bit 0 on E_LINK: link is up */
  uint32_t event_type;
  uint32_t status;
  uint32_t reason;
  uint32_t auth_type;
  uint32_t datalen;
  uint8_t  addr[6];
  char     ifname[16];
  uint8_t  ifidx;
  uint8_t  bsscfgidx;
};

/* Event Types (brcmf_fweh_event_code) */
constexpr uint32_t BCM_E_SET_SSID       = 0;
constexpr uint32_t BCM_E_JOIN           = 1;
constexpr uint32_t BCM_E_AUTH           = 3;
constexpr uint32_t BCM_E_DEAUTH         = 5;
constexpr uint32_t BCM_E_DEAUTH_IND     = 6;
constexpr uint32_t BCM_E_ASSOC          = 7;
constexpr uint32_t BCM_E_DISASSOC       = 11;
constexpr uint32_t BCM_E_DISASSOC_IND   = 12;
constexpr uint32_t BCM_E_LINK           = 16;
constexpr uint32_t BCM_E_PSK_SUP        = 46;
constexpr uint32_t BCM_E_SCAN_COMPLETE  = 54;
constexpr uint32_t BCM_E_ESCAN_RESULT   = 69;

struct DhdInfo {
  bool initialized;
  uint32_t magic;
  uint32_t version;
  uint8_t mac[6];
  uint8_t tx_seq;
  uint8_t rx_seq;
  uint8_t tx_credits;
  uint32_t ioctl_count;
  uint32_t ioctl_errors;
  uint32_t tx_packets;
  uint32_t rx_packets;
  uint32_t tx_bytes;
  uint32_t rx_bytes;
  uint32_t rx_events;
  uint32_t rx_errors;
  uint32_t rx_dropped; /* frames that arrived mid-ioctl with no queue slot left */
};

/* DHD Protocol Layer Lifecycle */
bool dhd_init();
bool dhd_is_ready();
const DhdInfo& dhd_info();

/* Low-level ioctl and iovar dispatch */
bool dhd_ioctl(uint32_t cmd, void* buf, uint32_t len, bool write);
bool dhd_get_var(const char* name, void* buf, uint32_t len);
bool dhd_set_var(const char* name, const void* buf, uint32_t len);
bool dhd_set_var_int(const char* name, int32_t val);

/* High-level WLC Wireless Management */
bool dhd_up();

/* Discovery: report the interface name the firmware actually bound, so
 * callers never stamp a literal "wlan0". Returns false if unknown. */
bool dhd_get_ifname(char out[16]);
bool dhd_down();
bool dhd_get_mac(uint8_t mac[6]);
bool dhd_set_mac(const uint8_t mac[6]);
bool dhd_get_ssid(char ssid[33]);
bool dhd_set_ssid(const char* ssid);
bool dhd_get_bssid(uint8_t bssid[6]);
bool dhd_get_channel(uint32_t& channel);
bool dhd_set_channel(uint32_t channel);
/* rssi is left at 0 and false returned when the dongle does not answer;
 * there is no default signal level. */
bool dhd_get_rssi(int32_t& rssi);
bool dhd_set_promisc(bool enable);
bool dhd_set_country(const char* country_code);
bool dhd_disassoc();
bool dhd_scan(const char* ssid = nullptr);
bool dhd_get_scan_results(void* out_buf, size_t max_len);
bool dhd_set_passphrase(const char* passphrase);


/* Bidirectional SDIO Function 2 packet transport */
bool dhd_tx_frame(const void* frame, size_t len);
bool dhd_rx_poll(void* out_frame, size_t max_len, size_t& out_len, uint8_t& out_channel);

void dhd_report();
