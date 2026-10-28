// What a page cannot do, said as an error where a program asks for it, not left out so that the name is "undefined".
#include <string>
#include <vector>

#include "aios/userland/grain.hpp"

namespace aios::userland::grain {

void install_mail_builtins(Vm& vm);     // grain_std_mail.cpp's, where it is built
void install_events_builtins(Vm& vm);   // grain_std_events.cpp's, called by install_http_builtins

namespace {

// One native per name, each raising its own message: <name>() is not available in a browser: <why>.
#define UNAVAILABLE(fn, name, why)                                                       \
  Value fn(Vm&, std::vector<Value>&, std::string& error) {                               \
    error = name "() is not available in a browser: " why;                               \
    return Value::nil();                                                                 \
  }

#define NO_PROCESS "a page cannot start programs"
#define NO_SOCKET "a page cannot open sockets; use http_get / http_post / http_request"
#define NO_DB "a page cannot speak the database's protocol (an HTTP gateway for it is not built yet)"
#define NO_MAIL "a page cannot send mail"
#define NO_EVENTS "a page cannot hold a server-sent event stream open from a program yet"

UNAVAILABLE(process_run_, "process_run", NO_PROCESS)

UNAVAILABLE(udp_socket_, "udp_socket", NO_SOCKET)
UNAVAILABLE(udp_bind_, "udp_bind", NO_SOCKET)
UNAVAILABLE(udp_send_, "udp_send", NO_SOCKET)
UNAVAILABLE(udp_send_to_, "udp_send_to", NO_SOCKET)
UNAVAILABLE(udp_recv_, "udp_recv", NO_SOCKET)
UNAVAILABLE(udp_recv_from_, "udp_recv_from", NO_SOCKET)
UNAVAILABLE(udp_port_, "udp_port", NO_SOCKET)
UNAVAILABLE(udp_close_, "udp_close", NO_SOCKET)
UNAVAILABLE(tcp_connect_, "tcp_connect", NO_SOCKET)
UNAVAILABLE(tcp_send_, "tcp_send", NO_SOCKET)
UNAVAILABLE(tcp_recv_, "tcp_recv", NO_SOCKET)
UNAVAILABLE(tcp_close_, "tcp_close", NO_SOCKET)

UNAVAILABLE(db_connect_, "db_connect", NO_DB)
UNAVAILABLE(db_query_, "db_query", NO_DB)
UNAVAILABLE(db_close_, "db_close", NO_DB)
UNAVAILABLE(db_connected_, "db_connected", NO_DB)
UNAVAILABLE(db_ping_, "db_ping", NO_DB)
UNAVAILABLE(db_select_db_, "db_select_db", NO_DB)
UNAVAILABLE(db_tables_, "db_tables", NO_DB)
UNAVAILABLE(db_columns_, "db_columns", NO_DB)
UNAVAILABLE(db_schema_, "db_schema", NO_DB)
UNAVAILABLE(db_transaction_, "db_transaction", NO_DB)
UNAVAILABLE(db_build_update_, "db_build_update", NO_DB)
UNAVAILABLE(live_, "live", NO_DB)

UNAVAILABLE(mail_send_, "mail_send", NO_MAIL)
UNAVAILABLE(events_, "events", NO_EVENTS)
UNAVAILABLE(events_close_, "events_close", NO_EVENTS)

}  // namespace

void install_process_builtins(Vm& vm) { vm.define_native("process_run", 1, 2, process_run_); }

void install_socket_builtins(Vm& vm) {
  vm.define_native("udp_socket", 0, 0, udp_socket_);
  vm.define_native("udp_bind", 2, 3, udp_bind_);
  vm.define_native("udp_send", 4, 4, udp_send_);
  vm.define_native("udp_send_to", 4, 4, udp_send_to_);
  vm.define_native("udp_recv", 1, 3, udp_recv_);
  vm.define_native("udp_recv_from", 1, 3, udp_recv_from_);
  vm.define_native("udp_port", 1, 1, udp_port_);
  vm.define_native("udp_close", 1, 1, udp_close_);
  vm.define_native("tcp_connect", 2, 3, tcp_connect_);
  vm.define_native("tcp_send", 2, 2, tcp_send_);
  vm.define_native("tcp_recv", 1, 3, tcp_recv_);
  vm.define_native("tcp_close", 1, 1, tcp_close_);
}

void install_db_builtins(Vm& vm) {
  vm.define_native("db_connect", 1, 5, db_connect_);
  vm.define_native("db_query", 2, 4, db_query_);
  vm.define_native("db_close", 1, 1, db_close_);
  vm.define_native("db_connected", 1, 1, db_connected_);
  vm.define_native("db_ping", 1, 1, db_ping_);
  vm.define_native("db_select_db", 2, 2, db_select_db_);
  vm.define_native("db_tables", 1, 2, db_tables_);
  vm.define_native("db_columns", 2, 2, db_columns_);
  vm.define_native("db_schema", 1, 1, db_schema_);
  vm.define_native("db_transaction", 2, 2, db_transaction_);
  vm.define_native("db_build_update", 4, 4, db_build_update_);
  vm.define_native("live", 2, 3, live_);
}

void install_mail_builtins(Vm& vm) { vm.define_native("mail_send", 1, 1, mail_send_); }
void install_events_builtins(Vm& vm) {
  vm.define_native("events", 2, 3, events_);
  vm.define_native("events_close", 1, 1, events_close_);
}

}  // namespace aios::userland::grain
