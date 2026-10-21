#pragma once
/* Bare-metal Network Device & IPv4 / DHCP / ARP Stack (metal/arm64/net.hpp).
 *
 * Implements:
 *   - Dual network interface support: eth0 (SMSC LAN9514 Ethernet) & wlan0 (BCM43438 Wi-Fi)
 *   - Automatic DHCP client state machine (Discover -> Offer -> Request -> Ack)
 *   - Dynamic ARP request / reply engine & peer cache
 *   - ICMP Echo (Ping) responder (sub-millisecond replies to incoming pings)
 *   - ICMP Ping diagnostic sender
 *   - Wi-Fi scanning, WPA2 association, and FAT32 credential persistence
 *   - Bidirectional lockless TX/RX ring buffers
 */
#include <cstddef>
#include <cstdint>

constexpr size_t NET_MAX_FRAME_SIZE = 1536;
constexpr size_t NET_RING_CAPACITY   = 64;

enum class NetIfType : uint8_t {
  None     = 0,
  Ethernet = 1,  /* eth0 (SMSC LAN9514) */
  Wifi     = 2   /* wlan0 (Broadcom BCM43438) */
};

enum class NetLinkState : uint8_t {
  Down        = 0,
  Up          = 1,
  Scanning    = 2,
  Associating = 3,
  Associated  = 4
};

enum class DhcpState : uint8_t {
  Init        = 0,
  Selecting   = 1,
  Requesting  = 2,
  Bound       = 3,
  Failed      = 4
};

struct NetFrame {
  uint8_t  data[NET_MAX_FRAME_SIZE];
  uint16_t len;
  uint16_t flags;
  uint64_t timestamp_ns;
};

struct NetRingStats {
  uint32_t capacity;
  uint32_t depth;
  uint64_t enqueued;
  uint64_t dequeued;
  uint64_t dropped;
};

struct NetScanEntry {
  uint8_t  bssid[6];
  char     ssid[33];
  uint16_t channel;
  int16_t  rssi;
  uint16_t capability;
};

struct NetDeviceStats {
  uint64_t tx_packets;
  uint64_t rx_packets;
  uint64_t tx_bytes;
  uint64_t rx_bytes;
  uint64_t tx_errors;
  uint64_t rx_errors;
  uint64_t tx_dropped;
  uint64_t rx_dropped;
};

struct IpConfig {
  bool      dhcp_enabled;
  bool      dhcp_bound;
  DhcpState dhcp_state;
  uint8_t   ip[4];
  uint8_t   netmask[4];
  uint8_t   gateway[4];
  uint8_t   dns[4];
  uint8_t   server_ip[4];
  uint32_t  lease_time_sec;
  uint64_t  lease_start_ns;
};

struct NetInterfaceInfo {
  bool           initialized;
  bool           link_up;
  NetIfType      type;
  char           name[16];
  uint8_t        mac[6];
  NetLinkState   link_state;
  IpConfig       ip_cfg;
  char           ssid[33];
  uint8_t        bssid[6];
  uint32_t       channel;
  int32_t        rssi;
  NetRingStats   tx_ring;
  NetRingStats   rx_ring;
  NetDeviceStats stats;
};

struct WifiSavedConfig {
  bool valid;
  char ssid[33];
  char psk[64];
  bool auto_connect;
  /* Optional ISO country/regulatory domain from wifi.cfg. Empty when the
   * file did not supply one - in that case we omit the setting rather than
   * hardcode a domain. */
  char country[4];
};

/* Core Subsystem Lifecycle */
bool net_init();
/* eth0 only - does not touch wlan0/dhd_init() at all. Exists because
 * net_init() always brings up both, and a WiFi bring-up attempt (five
 * power/clock variations, dozens of log lines) drowns out the much shorter
 * eth0 diagnostic output in the same log ring when you only wanted to
 * retry the wired side. */
bool net_init_eth_only();
bool net_is_up();
void net_poll();

/* Interface Queries */
const NetInterfaceInfo& net_eth_info();
const NetInterfaceInfo& net_wifi_info();
const NetInterfaceInfo& net_primary_info();
NetIfType net_primary_type();

/* DHCP & IP Management */
bool net_start_dhcp(NetIfType if_type);
bool net_is_dhcp_bound(NetIfType if_type);
bool net_set_static_ip(NetIfType if_type, const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4], const uint8_t dns[4]);

/* ICMP Ping */
bool net_ping(const uint8_t target_ip[4], uint32_t timeout_ms, uint32_t& out_rtt_us);

/* Frame Transmission and Reception */
bool net_transmit_if(NetIfType if_type, const void* frame, size_t len);
bool net_transmit(const void* frame, size_t len);

/* MAC Address Management */
bool net_get_mac(uint8_t mac[6]);
bool net_set_mac(const uint8_t mac[6]);

/* Wi-Fi Link & Management Commands */
bool net_scan(NetScanEntry* entries, size_t max_entries, size_t& out_count);
/* Latest scan results (background or user), without touching the radio.
 * *generation changes whenever a new result set lands. */
size_t net_wifi_last_scan(NetScanEntry* out, size_t max, uint32_t* generation);
bool net_associate(const char* ssid, const char* psk = nullptr);
bool net_disassociate();
bool net_send_test_frame();

/* Wi-Fi Configuration Persistence (FAT32 / SD Card / Memory) */
bool wifi_load_saved_config(WifiSavedConfig& cfg);
bool wifi_save_config(const char* ssid, const char* psk, bool auto_connect);
bool wifi_clear_saved_config();
const WifiSavedConfig& wifi_get_saved_config();
bool wifi_auto_connect_saved();

/* ----------------- TCP Protocol & Socket Subsystem ----------------- */
enum class TcpState : uint8_t {
  Closed       = 0,
  Listen       = 1,
  SynSent      = 2,
  SynReceived  = 3,
  Established  = 4,
  FinWait1     = 5,
  FinWait2     = 6,
  Closing      = 7,
  CloseWait    = 8,
  LastAck      = 9,
  TimeWait     = 10
};

constexpr uint8_t TCP_FLAG_FIN = 0x01;
constexpr uint8_t TCP_FLAG_SYN = 0x02;
constexpr uint8_t TCP_FLAG_RST = 0x04;
constexpr uint8_t TCP_FLAG_PSH = 0x08;
constexpr uint8_t TCP_FLAG_ACK = 0x10;
constexpr uint8_t TCP_FLAG_URG = 0x20;

constexpr size_t TCP_MAX_SOCKETS     = 16;
// The most a peer may have in flight to or from us without window scaling.
// It was 8 KB: at the hub's 45 ms round trip that capped the board at ~70
// KB/s each way, so a 500 KB answer held its one loop for 7 s and a 20 MB
// proof could not leave at all before the hub gave up on it.
constexpr size_t TCP_SOCKET_BUF_SIZE = 65535;
constexpr size_t TCP_MAX_BACKLOG     = 8;

struct TcpSocketInfo {
  int          id;
  TcpState     state;
  uint8_t      remote_ip[4];
  uint16_t     remote_port;
  uint16_t     local_port;
  size_t       rx_pending;
  size_t       tx_pending;
  bool         is_listening;
  bool         is_reset;
  bool         is_peer_fin;
};

/* TCP Socket Operations */
int      net_tcp_connect(const uint8_t remote_ip[4], uint16_t port, uint32_t timeout_ms = 8000);
int      net_tcp_listen(uint16_t port, size_t backlog = TCP_MAX_BACKLOG);
int      net_tcp_accept(int listen_sock, uint32_t timeout_ms = 0);
bool     net_tcp_unlisten(uint16_t port);
bool     net_tcp_is_listening(uint16_t port);

int      net_tcp_send(int sock, const void* data, size_t len, uint32_t timeout_ms = 8000);
int      net_tcp_recv(int sock, void* buf, size_t max_len, uint32_t timeout_ms = 0);
bool     net_tcp_send_all(int sock, const void* data, size_t len, uint32_t timeout_ms = 8000);
bool     net_tcp_recv_all(int sock, void* buf, size_t len, uint32_t timeout_ms = 8000);

void     net_tcp_close(int sock);
void     net_tcp_abort(int sock);
bool     net_tcp_is_alive(int sock);
bool     net_tcp_is_established(int sock);
size_t   net_tcp_pending(int sock);
TcpState net_tcp_state(int sock);
bool     net_tcp_peer(int sock, uint8_t out_ip[4], uint16_t& out_port);
size_t   net_tcp_get_all_info(TcpSocketInfo* out_info, size_t max_info);
const char* tcp_state_str(TcpState st);

/* String formatters */
void ip_to_str(const uint8_t ip[4], char* buf, size_t max_len);
void mac_to_str(const uint8_t mac[6], char* buf, size_t max_len);

/* Event Log (recent status-change / driver history, for the desktop UI's
 * per-interface log panels - a smaller, always-visible echo of what would
 * otherwise only exist on the serial console). Drivers append their own
 * kprintf/kputs lines here too, so a failure like "dongle firmware not
 * responding" is visible on the framebuffer, not just over UART. */
constexpr size_t NET_LOG_CAPACITY = 12;
constexpr size_t NET_LOG_LINE_LEN = 72;

void net_log_eth(const char* msg);
void net_log_wifi(const char* msg);
/* Fills out_lines[0..count) with pointers to log text, NEWEST first.
 * Returns the number filled (<= max_lines). Pointers are valid until the
 * next net_log_* call for that interface. */
size_t net_eth_log(const char** out_lines, size_t max_lines);
size_t net_wifi_log(const char** out_lines, size_t max_lines);

/* Shell / Diagnostic Reports */
void net_report();
void wifi_report();

/* UDP sockets: open (0 = an ephemeral port; <0 = in use / reserved / table
 * full), send (bytes, or <0: bad socket, too big for one frame, no link, no
 * ARP answer), recv (bytes, 0 = nothing within timeout_ms, <0 bad socket).
 * Datagrams to this machine's own address stay local. */
int      net_udp_open(uint16_t port);
uint16_t net_udp_port(int sock);
void     net_udp_close(int sock);
int      net_udp_send(int sock, const uint8_t ip[4], uint16_t port, const void* data, size_t len);
int      net_udp_recv(int sock, void* buf, size_t max_len, uint8_t from_ip[4], uint16_t& from_port, uint32_t timeout_ms);

/* One live breadcrumb line, broadcast on udp/42425 (nc -ulk 42425). */
void net_breadcrumb(const char* msg);
/* Send every queued frame on eth0 now (never from inside a USB transfer). */
void net_breadcrumb_flush();
void net_burst_test(unsigned n, size_t payload);
