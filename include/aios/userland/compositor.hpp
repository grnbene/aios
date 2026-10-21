#pragma once
#include <cstdint>
#include "aios/hal/sync.hpp"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include "aios/core/result.hpp"
#include "aios/hal/keys.hpp"
#include "aios/kernel/kernel.hpp"
#include "aios/userland/canvas.hpp"
#include "aios/userland/key_bindings.hpp"
#include "aios/userland/line_edit.hpp"
#include "aios/userland/source_lang.hpp"
#include "aios/userland/sql_database.hpp"
#include "aios/userland/mail_client.hpp"
#include "aios/userland/contacts_client.hpp"
#include "aios/userland/calendar_client.hpp"
#include "aios/userland/face.hpp"
#include "aios/userland/patch_transaction.hpp"
#include "aios/userland/web.hpp"
#include "aios/userland/button_suite.hpp"
// The editor's LSP integration arrived in fe9334f1 without its own header,
// its implementation, or its drive tool — that commit says in its own message
// that it did not build. A missing header here fails cmake's GENERATE step,
// and a failed generate means every later `cmake --build` quietly runs the
// PREVIOUS binary while printing nothing that reads as an error. That cost
// several rounds of testing a stale build.
//
// So: use the real header when it appears, and until then declare exactly the
// surface the compositor calls, with a manager that hands back nothing. Every
// `if (win.lsp_client)` is then false and the feature is simply off, rather
// than the whole machine being unbuildable. Derived from the call sites, not
// invented — and it disappears the moment the real header is committed.
#if __has_include("aios/userland/lsp_client.hpp")
#include "aios/userland/lsp_client.hpp"
#else
#include <functional>
namespace aios::userland {
enum class SourceLang : uint8_t;
struct LspPosition { int line = 0; int character = 0; };
struct LspRange { LspPosition start, end; };
struct LspLocation { std::string path; LspRange range; };
struct LspDiagnostic { LspRange range; std::string message; int severity = 0; };
struct LspCompletionItem { std::string label, detail, documentation, insert_text; int kind = 0; };
struct LspHoverInfo { std::string contents; };
class LspClient {
 public:
  void did_open(const std::string&, SourceLang, const std::string&) {}
  void did_change(const std::string&, const std::string&, int = 1) {}
  void did_close(const std::string&) {}
  void poll_responses() {}
  void set_diagnostic_callback(std::function<void(const std::string&, const std::vector<LspDiagnostic>&)>) {}
  void request_hover(const std::string&, int, int, std::function<void(const LspHoverInfo&)>) {}
  void request_completion(const std::string&, int, int, std::function<void(const std::vector<LspCompletionItem>&)>) {}
  void request_definition(const std::string&, int, int, std::function<void(const std::vector<LspLocation>&)>) {}
  void request_references(const std::string&, int, int, std::function<void(const std::vector<LspLocation>&)>) {}
};
class LspClientManager {
 public:
  static LspClientManager& instance() { static LspClientManager m; return m; }
  std::shared_ptr<LspClient> get_or_start_client(SourceLang, const std::string&) { return nullptr; }
  static std::string server_for(SourceLang) { return ""; }
};
}  // namespace aios::userland
#endif

namespace aios::userland {

class SqlDatabase;
class TaskLedger;
// The browser pane's own state, defined in web_pane.hpp. Held here behind a
// pointer so the window manager does not have to know what a DOM is.
struct WebPane;

struct Rect {
  int x = 0;
  int y = 0;
  int width = 800;
  int height = 600;

  bool contains(int px, int py) const {
    return px >= x && px < x + width && py >= y && py < y + height;
  }
};

// One pane per thing the machine can actually answer for. There used to be a
// "Subsystem Matrix" of sixteen hard-coded TASK-0xx rows all reading Complete,
// and a "Reasoning Studio" holding two lines of prose about an ABI: neither
// asked the kernel anything, so neither is here.
enum class WindowType {
  Terminal,   // the command surface, the same one the serial console offers
  Tasks,      // entities and devices, from the fabrics
  Miner,      // the miner's own persisted ledger
  Hardware,   // what the HAL says this machine is
  Editor,     // native C++ code & text editor
  Database,   // Navicat-style MariaDB/MySQL studio & data grid
  Browser,    // the machine's own web engine, painting into this pane
  Mail,       // native C++ Apple Mail client & message viewer
  Contacts,   // native C++ Apple Contacts / Address Book
  Calendar,   // native C++ Apple Calendar / iCal & agenda
  FaceApp,    // native Face application runner & interactive app desk
  Inspector,  // universal in-desktop behavioral inspector & diagnostic studio
  Custom
};

enum class HitRegion {
  None,
  Content,
  TitleBar,
  CloseBtn,
  MinimizeBtn,
  MaximizeBtn,
  ResizeGrip
};

// One step the editor can walk back to. The whole buffer is kept rather than a
// description of the change: a file this size costs little to copy, and an
// undo that replays edits is an undo that can be subtly wrong.
struct EditSnapshot {
  std::vector<std::string> lines;
  int cursor_line = 0;
  int cursor_col = 0;
};

// A file the editor has open. Switching tabs parks the live buffer here and
// takes the other one out, so a half-finished edit survives looking at
// something else -- which is the entire reason tabs exist.
struct EditorBuffer {
  std::string path;
  std::vector<std::string> lines;
  bool dirty = false;
  int cursor_line = 0;
  int cursor_col = 0;
  int view_top = 0;
  int view_left = 0;
  // How the file ended its lines and whether it ended with one. Saving a file
  // that came in with CRLF as LF rewrites every line of somebody's diff.
  std::string eol = "\n";
  bool final_newline = true;
  // What the file is written in, and whether this tab is showing it rendered
  // rather than as source. Both belong to the buffer: switching tabs must not
  // carry one file's language, or its preview, over to another.
  SourceLang lang = SourceLang::PlainText;
  bool preview = false;
  // The bytes that were on disk when this buffer last agreed with the file —
  // at open, at reload, at save. The machine writes files too (the mind's
  // `edit`, a dream, a directive), and until now this pane could not tell:
  // it went on showing the old text and the next save wrote the whole buffer
  // back over what had been done, silently.
  uint64_t disk_mark = 0;
  bool disk_changed = false;   // noticed, and said so, since that mark
};

// One wrapped line of a message body, and whether it is quoted text.
struct MailDisplayLine {
  std::string text;
  bool is_quote = false;
};

struct WindowSurface {
  uint32_t window_id = 0;
  EntityID entity_id;
  std::string title;
  std::string badge;
  WindowType type = WindowType::Terminal;
  Rect bounds;
  Rect normal_bounds;
  bool focused = false;
  bool minimized = false;
  bool maximized = false;
  bool visible = true;
  int workspace = 0;
  bool pinned = false;
  std::vector<uint32_t> buffer;
  int scroll_y = 0;
  std::vector<std::string> lines;
  // Visual rows the transcript last wrapped to. The scrollback limit is a
  // count of ROWS, not of logical lines, and the two differ the moment an
  // answer is wider than the pane; clamping against lines.size() stopped the
  // wheel before the top of the buffer. Written by the renderer, which is the
  // only thing that knows the card's width.
  int wrapped_rows = 0;
  // The command line, with a cursor in it. It used to be a bare string that
  // could only be appended to and popped from the back, so there was no way
  // to go back and fix the start of a line.
  EditLine input;
  std::string status_text;
  bool custom_action_active = false;
  std::vector<std::string> history;
  int history_index = -1;
  int scrollback_offset = 0;

  // --- the native code & text editor (WindowType::Editor) --------------------
  // `lines` IS the file: what the host read off the substrate, what the caret
  // moves through, and what save writes back. Nothing on this pane is a
  // picture of an editor.
  std::string file_path;          // empty until something is opened
  std::string file_dir;           // what the browser column is listing
  std::vector<std::string> dir_entries;
  std::vector<bool> dir_is_dir;
  int dir_scroll = 0;
  bool file_dirty = false;
  int cursor_line = 0;            // caret, 0-based into `lines`
  int cursor_col = 0;             // caret, a BYTE offset into that line
  int view_top = 0;               // first line drawn
  int view_left = 0;              // first display column drawn, for long lines
  // The buffer has the keyboard by default. It used to be the command line,
  // so the pane opened with typing going somewhere that was not the file.
  bool cmdline_focus = false;

  // A file's bytes survive a round trip. Tabs stay tabs and UTF-8 stays UTF-8;
  // the renderer substitutes what the ASCII font cannot draw, and only the
  // renderer does. Saving a file the editor merely looked at used to rewrite
  // every tab as four spaces and every accented letter as '?'.
  std::string file_eol = "\n";
  bool file_final_newline = true;
  // The language the open file is in, decided by its name when it was opened.
  SourceLang file_lang = SourceLang::PlainText;
  // Markdown can be read as well as written: this shows it rendered.
  bool file_preview = false;
  // The active buffer's copies of the two above; stashed with the rest.
  uint64_t file_disk_mark = 0;
  bool file_disk_changed = false;
  uint64_t file_checked_ms = 0;    // when this pane last looked at the disk

  // Per-line syntax token cache. An entry is only reused when it was made from
  // the SAME text (hash + length) under the SAME incoming HighlightState, and
  // it carries the state that line leaves behind, so a cached line still
  // advances /* */, fence and <?php state for the lines below it. Typing,
  // opening another file of the same length, or a block comment opening
  // above all miss naturally -- nothing has to remember to clear it. (It used
  // to be keyed on line count alone and only cleared on a theme switch, so
  // typing kept the old colours and a same-length file reused another file's.)
  struct TokLine {
    bool valid = false;
    uint64_t hash = 0;
    size_t len = 0;
    HighlightState in{}, out{};
    std::vector<SourceSpan> spans;
  };
  std::vector<TokLine> tok_cache;

  // The selection anchor. -1 means there is no selection; the caret is the
  // other end, so a selection is always (anchor .. caret).
  int sel_line = -1;
  int sel_col = -1;

  // Walking back and forward through the buffer's own history.
  std::vector<EditSnapshot> undo_stack;
  std::vector<EditSnapshot> redo_stack;
  uint64_t last_edit_ms = 0;      // for coalescing a run of typing into one step

  // Everything this window has open, and which one is live.
  std::vector<EditorBuffer> buffers;
  int active_buffer = -1;

  // What find/replace last worked with, so "again" has something to repeat.
  std::string find_text;
  std::string replace_text;

  // --- LSP Code Intelligence Engine (AIOS-EDIT-01) ------------------------
  std::shared_ptr<LspClient> lsp_client;
  std::vector<LspDiagnostic> diagnostics;
  // Which document the client was told about, and what it last received, so
  // opening another file closes the old one and edits reach the server as
  // didChange instead of the server reasoning about the file as first opened.
  std::string lsp_path;            // the path did_open was sent for
  std::string lsp_tried_path;      // last path a server was looked for (once per file)
  int lsp_version = 0;
  uint64_t lsp_sent_hash = 0;      // FNV of the text the server has
  uint64_t lsp_dirty_since_ms = 0; // first unsent edit, for the debounce

  // Auto-completion popup state
  bool lsp_completion_active = false;
  int lsp_completion_index = 0;
  std::vector<LspCompletionItem> lsp_completions;

  // Hover doc tooltip state
  bool lsp_hover_active = false;
  std::string lsp_hover_text;

  // References list popup state
  bool lsp_references_active = false;
  int lsp_references_index = 0;
  std::vector<LspLocation> lsp_references;

  // --- Multi-File Atomic Review & Transaction Engine (Unified Review Buffer) ---
  bool tx_review_active = false;
  TransactionManifest tx_manifest;
  int tx_focused_hunk = 0;       // 0-based global hunk index across all files
  int tx_focused_file = 0;       // 0-based file index
  int tx_scroll_y = 0;
  std::vector<TransactionReviewHit> tx_hit_map;

  // --- the MariaDB / MySQL studio (WindowType::Database) ---------------------
  // A result set the engine actually returned, kept as columns and rows so the
  // grid renders values instead of parsing a printed table back apart.
  std::vector<std::string> grid_columns;
  std::vector<std::vector<std::string>> grid_rows;
  std::vector<std::string> db_tables;   // the explorer, from the engine
  std::vector<std::string> db_schema;   // the selected table's columns
  std::string db_selected;
  // WHAT IS IN THE GRID: the table a click opened, `query` for a statement
  // typed at the prompt, `<table>.schema` for a Describe. The export names the
  // file after this, so a CSV cannot claim to be a table's rows while holding
  // that table's column list instead.
  std::string db_grid_label;
  std::string db_last_query;
  std::string db_status;                // what the engine said about the last run
  bool db_error = false;
  bool db_connected = false;
  int grid_scroll = 0;
  int tree_scroll = 0;
  // The table the grid is showing rows OF, when it is showing a table at all.
  // An edit needs to know which table and which key it is changing, and a
  // grid built from an arbitrary SELECT cannot answer that -- so editing is
  // offered for a table's own rows and refused for everything else.
  std::string db_grid_table;
  int grid_sort_col = -1;         // which column the grid is ordered by
  bool grid_sort_desc = false;
  int grid_selected_row = -1;
  // The cell being typed into, and what has been typed so far.
  int grid_edit_row = -1;
  int grid_edit_col = -1;
  EditLine grid_edit;
  // How many rows each table holds, alongside db_tables, so the explorer can
  // say how big a table is before you open it.
  std::vector<size_t> db_table_rows;

  // --- paging -------------------------------------------------------------
  // The grid asks the engine for ONE WINDOW of rows, never the whole table.
  // Opening a 873,551-row table used to mean SELECT * FROM prices -- every row
  // over the wire, into RAM, into a grid that can show thirty of them. These
  // are what make that a LIMIT/OFFSET instead.
  bool db_paged = false;            // this grid is a page of a table
  int db_page = 0;                  // which page, zero-based
  int db_page_size = 200;           // rows per page
  size_t db_total_rows = 0;         // rows the whole table holds
  std::string db_where;             // an extra predicate, from the filter box
  // The schema list is 58 tables long and the panel is not: these scroll and
  // narrow it.
  int db_tree_scroll = 0;
  EditLine db_table_filter;         // narrows the schema list as you type
  bool db_filter_focus = false;
  // Columns, sideways. 18 columns squeezed into one pane is 18 columns of
  // "756..", so the grid shows whole columns from db_col_first and scrolls by
  // column rather than shrinking every one of them into a lie.
  int db_col_first = 0;
  std::vector<int> db_col_w;        // a width the person dragged, per column
  int db_col_drag = -1;             // which divider is under the pointer, -1 none
  int db_col_drag_x0 = 0;
  int db_col_drag_w0 = 0;
  int grid_selected_col = -1;       // for the cell inspector under the grid
  int db_hover_tool = -1;           // which toolbar icon the pointer is on

  // Connection manager modal state (AIOS-DB-01)
  bool db_show_conn_mgr = false;
  std::string db_selected_profile_id = "local_embedded";
  DbConnectionProfile db_profile_edit;
  bool db_edit_new_profile = false;
  std::string db_active_database = "aios_db";
  std::vector<std::string> db_available_databases;
  int db_conn_field_focus = -1; // 0=Name, 1=Host, 2=Port, 3=User, 4=Password, 5=Database, 6=Socket, 7=SSH Host, 8=SSH User

  // Table designer modal state (AIOS-DB-02): a column-identity-tracked edit
  // grid, staged and applied as one unit via SqlDatabase::apply_table_design
  // rather than executing each keystroke - a half-typed column name should
  // never itself become a live ALTER TABLE.
  bool db_show_designer = false;
  std::string db_designer_table;
  int db_designer_tab = 0;  // 0=Columns, 1=Indexes, 2=Foreign Keys
  std::vector<ColumnEdit> db_designer_edits;
  int db_designer_focus_row = -1;
  int db_designer_focus_col = -1;  // 0=name field, 1=default-value field
  int db_designer_scroll = 0;
  // New Index / New FK forms (Indexes and Foreign Keys tabs apply
  // immediately on "Add", unlike the Columns tab - both already have
  // single-statement, non-destructive ALTER TABLE support, so there is
  // nothing to stage).
  std::string db_designer_new_index_name;
  std::string db_designer_new_index_cols;
  bool db_designer_new_index_unique = false;
  int db_designer_index_field_focus = -1;  // 0=name, 1=cols
  std::string db_designer_new_fk_name;
  std::string db_designer_new_fk_col;
  std::string db_designer_new_fk_ref_table;
  std::string db_designer_new_fk_ref_col;
  int db_designer_fk_field_focus = -1;  // 0=name,1=col,2=ref_table,3=ref_col

  // ASQL Remote Telemetry & Fleet Controller modal / view state
  bool db_show_telemetry = false;
  std::string db_telemetry_host;
  uint16_t db_telemetry_port = 0;
  std::string db_telemetry_status = "not connected";
  std::string db_telemetry_binlog_file;
  uint64_t db_telemetry_binlog_pos = 0;
  double db_telemetry_lag_ms = 0.0;
  double db_telemetry_ram_mb = 0.0;
  std::string db_telemetry_mode = "unknown";
  size_t db_telemetry_replicated_tables = 0;
  std::vector<std::pair<std::string, size_t>> db_telemetry_table_stats;
  int db_telemetry_scroll = 0;

  // --- the web engine's pane (WindowType::Browser) ---------------------------
  // A page, a history and a scroll position. Behind a shared_ptr because
  // `windows_` is a vector that grows, so a WindowSurface has to stay
  // copyable, and because the engine's state is far too big to live inline in
  // every window on the desktop.
  std::shared_ptr<WebPane> web;

  // --- AetherMail native client (WindowType::Mail) -----------------------
  std::shared_ptr<MailStore> mail_store;
  std::string mail_active_folder = "inbox";
  std::string mail_active_account_id = "all";
  // When this window last KICKED a sync, on the steady clock. 0 means it never
  // has, which is why opening Mail fetches straight away instead of waiting out
  // a first interval. Nothing here used to poll at all: the only way mail moved
  // was a person clicking Check Mail.
  uint64_t mail_last_poll_ms = 0;
  // The pane's view of the mailbox, rebuilt only when the query or the store
  // changes. get_messages() copies every matching message, so calling it once
  // a frame meant copying 49,513 messages per frame on a real archive -- the
  // desktop ran at 8 FPS with the Mail window open.
  std::vector<MailMessage> mail_list_cache;
  std::vector<MailFolder> mail_folders_cache;
  uint64_t mail_cache_generation = 0;
  std::string mail_cache_folder;
  std::string mail_cache_search;
  std::string mail_cache_account;
  int mail_cache_filter = -1;
  bool mail_cache_valid = false;
  // The open message and its wrapped body. get_message() is a linear scan of
  // the whole store and wrapping re-splits the entire body, and both ran once
  // a frame; on a 71,693-message archive that was most of the frame budget.
  MailMessage mail_detail_cache;
  bool mail_detail_found = false;
  std::vector<MailDisplayLine> mail_body_lines_cache;
  std::string mail_detail_cache_id;
  uint64_t mail_detail_cache_generation = 0;
  int mail_body_cache_width = -1;
  // The sender's own html, laid out by the machine's own engine. Built when an
  // html message is opened and thrown away when the selection moves.
  std::shared_ptr<web::Browser> mail_html;
  std::string mail_html_id;        // which message mail_html holds
  int mail_html_scroll = 0;
  int mail_html_width = 0;
  // Whether this pane is showing the html or the plain text alternative. A
  // person always gets to see what was actually written.
  bool mail_show_html = true;
  // Whether this pane fetches the remote pictures a message points at. On:
  // mail is written to be looked at, and a client that shows grey boxes is not
  // showing you your mail. The cost is that a remote image is a read receipt --
  // it tells the sender the message was opened, when, and from which address --
  // so this stays a switch rather than a fact, and the pane says which way it
  // is set. Script is a different question and stays off regardless: no email
  // needs it, so running it buys nothing to set against the risk.
  bool mail_allow_remote = true;
  int mail_theme_mode = 0;         // 0=Auto (follows active_theme()), 1=Dark, 2=Light
  int mail_sel_start_line = -1;
  int mail_sel_end_line = -1;
  bool mail_has_selection = false;
  std::string mail_selected_id = "msg-101";
  int mail_list_scroll = 0;
  int mail_body_scroll = 0;
  std::string mail_search_query;
  EditLine mail_search_edit;
  bool mail_search_focused = false;
  int mail_filter_mode = 0; // 0=All, 1=Unread, 2=Flagged
  bool mail_composing = false;
  std::string mail_compose_draft_id;
  EditLine mail_compose_to;
  EditLine mail_compose_cc;
  EditLine mail_compose_subject;
  std::vector<std::string> mail_compose_body_lines;
  int mail_compose_cursor_line = 0;
  int mail_compose_cursor_col = 0;
  int mail_compose_focus = 0; // 0=To, 1=Cc, 2=Subject, 3=Body

  // Account Settings Modal
  bool mail_account_settings_open = false;
  int mail_acc_preset_selected = 0; // 0=iCloud, 1=Gmail, 2=Outlook, 3=Fastmail, 4=Yahoo, 5=Custom
  bool mail_acc_show_advanced = false;
  EditLine mail_acc_name;
  EditLine mail_acc_email;
  EditLine mail_acc_imap_host;
  EditLine mail_acc_imap_port;
  EditLine mail_acc_smtp_host;
  EditLine mail_acc_smtp_port;
  EditLine mail_acc_user;
  EditLine mail_acc_pass;
  int mail_acc_focus = 0; // 0=Name, 1=Email, 2=IMAP Host, 3=IMAP Port, 4=SMTP Host, 5=SMTP Port, 6=Username, 7=Password
  std::string mail_acc_status;

  // --- AetherContacts native client (WindowType::Contacts) ----------------
  std::shared_ptr<ContactsStore> contacts_store;
  std::string contacts_active_group = "all";
  std::string contacts_selected_id = "ct-101";
  int contacts_list_scroll = 0;
  std::string contacts_search_query;
  EditLine contacts_search_edit;
  bool contacts_search_focused = false;
  bool contacts_editing = false;
  bool contacts_is_new = false;
  EditLine contacts_edit_first_name;
  EditLine contacts_edit_last_name;
  EditLine contacts_edit_company;
  EditLine contacts_edit_job_title;
  EditLine contacts_edit_email;
  EditLine contacts_edit_phone;
  EditLine contacts_edit_notes;
  int contacts_edit_focus = 0; // 0=First, 1=Last, 2=Company, 3=Title, 4=Email, 5=Phone, 6=Notes

  // --- AetherCalendar native client (WindowType::Calendar) ----------------
  std::shared_ptr<CalendarStore> calendar_store;
  int calendar_view_mode = 0; // 0=Month, 1=Week, 2=Day, 3=Agenda
  int calendar_view_year = 2026;
  int calendar_view_month = 9;
  int calendar_view_day = 13;
  // The month layout the last render drew: column of the 1st (0 = Monday),
  // days in the month, week rows. -1/0 until the calendar has been painted.
  int calendar_grid_first_col = -1;
  int calendar_grid_days = 0;
  int calendar_grid_rows = 0;
  std::string calendar_selected_event_id = "ev-101";
  int calendar_list_scroll = 0;
  std::string calendar_search_query;
  EditLine calendar_search_edit;
  bool calendar_search_focused = false;
  bool calendar_editing = false;
  bool calendar_is_new = false;
  EditLine calendar_edit_title;
  EditLine calendar_edit_location;
  EditLine calendar_edit_cat;
  EditLine calendar_edit_date;
  EditLine calendar_edit_time;
  EditLine calendar_edit_desc;
  int calendar_edit_focus = 0; // 0=Title, 1=Location, 2=Category, 3=Date, 4=Time, 5=Desc

  // --- Face Applications Desk (WindowType::FaceApp) -------------------------
  std::shared_ptr<face::Runtime> face_app_runtime;
  std::string face_app_path;
  std::string face_app_title;
  std::vector<std::string> face_app_list;
  int face_app_selected_index = 0;
  uint64_t face_app_disk_mark = 0;
  bool face_app_needs_reload = false;

  // --- Universal In-Desktop Inspector & Studio (WindowType::Inspector) ------
  std::string studio_target_name = "order_desk"; // e.g. "order_desk", "task_tracker", "system_monitor", "notes", "Mail", etc.
  int studio_active_tab = 0;          // 0=Tree & Bounds, 1=State & Store, 2=Drive & Test, 3=Telemetry & FPS, 4=Source & Reload
  int studio_tree_scroll = 0;
  int studio_state_scroll = 0;
  int studio_drive_scroll = 0;
  int studio_log_scroll = 0;
  int studio_selected_widget_idx = -1;
  int studio_selected_state_idx = -1;
  EditLine studio_state_edit;
  bool studio_state_editing = false;
  std::string studio_state_edit_key;
  std::vector<std::string> studio_drive_steps;
  int studio_drive_current_step = 0;
  bool studio_drive_running = false;
  std::vector<std::string> studio_drive_results;
  EditLine studio_drive_custom_cmd;
  bool studio_drive_cmd_focused = false;

  // What the window's own renderer draws into, and what actually reaches the
  // screen: the frame takes a 26px titlebar off the top and a 1px border on
  // each side, so the content is SMALLER than the window. Drawing into a
  // buffer the size of the whole window puts everything the renderer anchors
  // to its bottom edge — a terminal's prompt row, an app's buttons — 27px
  // below the last row anybody can see.
  int content_width() const { return bounds.width - 2 > 0 ? bounds.width - 2 : 1; }
  int content_height() const { return bounds.height - 27 > 0 ? bounds.height - 27 : 1; }
  size_t content_pixels() const { return static_cast<size_t>(content_width()) * static_cast<size_t>(content_height()); }
};

// The faces the desktop draws with. Real typefaces, rasterised by FreeType at
// build time into 8-bit coverage (metal/tools/mkfont.py) and blended by the
// compositor — not a 1-bit ROM font. Ui is the interface's voice, UiSmall its
// labels, Mono anything the machine typed or measured.
enum class Face { Ui, UiBold, UiSmall, Mono };

using OutputCallback = std::function<void(const std::string& line)>;
using CommandDispatcher = std::function<void(const std::string& cmd, OutputCallback on_output_line)>;

class HardwareCompositor {
 public:
  HardwareCompositor(uint32_t screen_width = 1280, uint32_t screen_height = 800);
  ~HardwareCompositor();

  // Core Window Management API (TASK-010)
  Result<uint32_t> create_window(const std::string& title, int x, int y, int w, int h, WindowType type = WindowType::Terminal);
  Result<void> move_window(uint32_t window_id, int x, int y);
  Result<void> resize_window(uint32_t window_id, int w, int h);
  Result<void> focus_window(uint32_t window_id);
  Result<void> minimize_window(uint32_t window_id);
  Result<void> maximize_window(uint32_t window_id);
  Result<void> close_window(uint32_t window_id);
  Result<void> bring_to_front(uint32_t window_id);
  Result<void> render_frame();

  using FocusChangeCallback = std::function<void(const EntityID& entity, bool focused)>;
  void set_on_focus_change(FocusChangeCallback cb) { on_focus_change_cb_ = std::move(cb); }
  void on_focus_change(const EntityID& entity, bool focused) {
    if (on_focus_change_cb_) on_focus_change_cb_(entity, focused);
  }

  // Desktop Environment Shell
  void init_default_workspace(Kernel& k);
  // Paint the canvas face directly. render_desktop() delegates here whenever
  // canvas mode is on, so one call paints whichever face is up.
  void render_canvas(Kernel& k);
  // The toast, the Stop control, the menu and the pointer, over either face.
  void draw_desktop_overlays(int toast_bottom);
  void render_desktop(Kernel& k);

  // Ergonomic Cockpit Operator Interface & Top Navigation
  enum class TopTab : int { Overview = 0, Workspaces = 1, FastTables = 2, Terminal = 3 };
  void set_top_tab(TopTab tab) { active_top_tab_ = tab; }
  TopTab top_tab() const { return active_top_tab_; }
  void draw_top_bar(Kernel* k = nullptr);
  void draw_center_dispatch_card(Kernel* k = nullptr);
  void execute_palette_action(int idx);

  // ---- The taskbar: the machine's furniture, on every section
  // One strip, always there, that owns launching. It used to be a dock of six
  // hardcoded tiles drawn only on the workspace, beside a nine-chip "quick
  // ignition" row on the Overview page and a three-chip workspace switcher on
  // the same page — three launchers for one machine, two of which vanished the
  // moment you were looking at anything else. Now every app on this machine is
  // a row in ONE table, the taskbar is drawn on whichever face is up, and the
  // sections are switched from it.
  //
  // Every rectangle measured from what goes in it, out of ONE function, so the
  // paint and the hit test cannot drift apart — the same rule the view bar and
  // the desk tiles follow.
  struct TaskbarGeom {
    int x = 0, y = 0, w = 0, h = 0;
    int tile = 0;                       // one app tile, square
    int app_y = 0;
    int app_x[16] = {0};
    int app_count = 0;
    // Each section pill is as wide as its OWN label. They were all cut to the
    // widest one, so "1", "2" and "3" were each as wide as "Start" and the row
    // read as four stretched buttons.
    int sec_x[10] = {0}, sec_w[10] = {0};
    int sec_h = 0, sec_y = 0;
    int sec_count = 0;
    // The windows open on this section, by name. The app tiles say what this
    // machine HAS; these say what is actually up, which is the thing a person
    // reads a taskbar for. Clicking one raises it.
    int win_count = 0;
    int win_x[12] = {0}, win_w[12] = {0}, win_y = 0, win_h = 0;
    uint32_t win_id[12] = {0};
    bool contains(int px, int py) const {
      return px >= x && px < x + w && py >= y && py < y + h;
    }
  };
  // Which wallpaper the face being looked at is on. The start page keeps its
  // own; a windowed section carries its own. ONE answer, so the painter, the
  // background picker and the shader cannot disagree about what is behind you.
  // The brand badge's width, MEASURED from the wordmark that goes in it. It
  // was a hardcoded 110 next to text that needs more, so "AIOS CORE" was drawn
  // straight through the badge's own right edge — and the click region used the
  // same 110, so the two could never agree either. One function, both callers.
  std::string fit_top_bar_text(const std::string& t, int room, Face face) const;
  int top_bar_badge_w() const;
  // The bar's own width, measured from its contents rather than written down.
  int top_bar_w() const;
  // Put the resident mind on another model. Writes AOS_MODEL into the
  // machine's aos.conf — the file the assay bridge reads when it starts — and
  // then restarts the bridge and rebinds it, so the change is live now and
  // survives the next boot. Returns false if there is no mind to rebind or the
  // bridge could not be started, and says so in a toast either way.
  bool set_mind_model(const std::string& model);
  // Put it on another PROVIDER: writes AOS_PROVIDER, resets AOS_MODEL to that
  // preset's default (the old provider's model must not ride along — that is
  // how claude-cli ends up invoked with a fireworks model id and 404s on every
  // turn), then restarts the bridge.
  bool set_mind_provider(const std::string& provider);
  // Ask shared/model-presets.cjs which providers this fleet offers and which
  // models the current one serves. Runs off the frame thread: listing a CLI
  // provider's models runs that CLI.
  void refresh_mind_lists();
  // Write one key into the machine's aos.conf — the file the assay bridge
  // reads when it starts, so this is what makes a choice survive a reboot.
  bool write_machine_conf(const std::string& key, const std::string& value);
  // Start a fresh bridge on whatever aos.conf now says and bind it to the
  // resident mind. Settings are written before this is called, never after.
  bool rebind_mind();
  // How large the interface is drawn, as a percentage: 100, 125 or 150. Kept
  // in the machine's aos.conf beside its other choices, so it survives a
  // reboot and can be set from a shell on a machine whose screen is too small
  // to read the control that sets it.
  int ui_scale() const;
  void set_ui_scale(int pct);
  void load_ui_scale();
  int face_scene() const;
  void set_face_scene(int scene);
  // ---- Alt+Tab: the switcher every desktop has and this one did not
  // Held Alt keeps the list up; each Tab steps through it; letting Alt go
  // raises whatever is selected. Order is most-recently-focused first, so a
  // single Tab is "the thing I was just in", which is what the gesture is for.
  void alt_tab_step(bool backwards);
  void alt_tab_commit();
  void alt_tab_cancel();
  void draw_alt_tab();
  bool alt_tab_open() const { return alt_tab_open_; }

  TaskbarGeom taskbar_geom() const;
  // The rectangle a window may actually occupy. The top bar and the taskbar
  // are drawn OVER the windows, so a window laid out against the raw screen
  // ends up with its bottom rows under the dock -- a maximised shell's own
  // prompt row sat under it. Measured from the two strips themselves, not
  // from insets written down in five places that no longer matched them.
  Rect work_area() const;
  void draw_taskbar();
  // A press on the taskbar, wherever it came from. Returns true when the strip
  // took it, so the face underneath does not also act on it.
  bool taskbar_click(int px, int py);
  // The one top bar, hit-tested on whichever face is up.
  bool top_bar_click(int px, int py);
  // Is an application of this kind actually up on the section being looked at?
  // The dock used to light its first three tiles with `i < 3` — a running light
  // that was a constant, so it said Terminal, Editor and MariaDB were running
  // on a machine that had just booted with nothing open at all.
  bool app_running(WindowType type) const;

  void open_command_palette();
  void close_command_palette();
  void toggle_command_palette();
  bool command_palette_open() const { return palette_open_; }
  void update_palette_filter();
  void draw_command_palette();
  bool command_palette_click(int x, int y);

  // Window Snapping & Ergonomic Tiling
  void snap_window_left(uint32_t win_id = 0);
  void snap_window_right(uint32_t win_id = 0);
  void snap_window_maximize(uint32_t win_id = 0);
  void tile_windows(int columns = 2);
  void close_focused_window();
  void close_all_windows();

  // Context-Centric Workspaces & Virtual Spaces
  enum class CockpitWorkspace : int {
    Blank = 0,
    Code = 1,
    Comms = 2,
    Admin = 3,
    Space4 = 4,
    Space5 = 5,
    Space6 = 6,
    Space7 = 7,
    Space8 = 8,
    Space9 = 9
  };
  void switch_workspace(CockpitWorkspace ws);
  void switch_workspace(int ws_index);
  CockpitWorkspace current_workspace() const { return current_workspace_; }
  int current_workspace_index() const { return static_cast<int>(current_workspace_); }
  void pin_window(uint32_t win_id, bool pinned = true);
  void move_window_to_workspace(uint32_t win_id, int ws);

  // App creation / retrieval on demand
  uint32_t ensure_app_window(WindowType type);

  // Drawing primitives on buffer
  void clear_buffer(std::vector<uint32_t>& buf, uint32_t color, int w, int h);
  void draw_rect_to(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, int w, int h, uint32_t color);
  void draw_rect_outline_to(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, int w, int h, uint32_t color);
  void draw_rounded_rect_to(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, int w, int h, int radius, uint32_t color);
  void draw_gradient_v_to(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, int w, int h, uint32_t c1, uint32_t c2);
  void draw_app_icon(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, WindowType type, int size = 16);
  void draw_named_icon(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, const std::string& name, int size = 16);
  // Blend one pixel: the only way coverage-based text and antialiased edges can
  // exist on an opaque framebuffer.
  void blend_to(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, uint32_t color, int alpha);
  // Blend one pixel the way CSS says to: straight over in sRGB, no linear-light
  // detour. `blend_to` above is gamma-correct, which is right for the COVERAGE
  // of an antialiased edge -- a half-covered pixel really is half the light --
  // and wrong for an rgba() fill, which the spec composites on the 8-bit values
  // as given. The difference is invisible on mid greys and enormous at the
  // bottom: 3% white over #070707 is 15 in sRGB and 50 through linear light, so
  // a dark theme's hairline dividers and tinted panels come out as flat slabs
  // of grey. Anything painting a declared alpha -- a fill, a border, a shadow,
  // an `opacity` -- wants this one.
  void blend_srgb_to(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, uint32_t color, int alpha);
  void draw_text_to(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, const std::string& text, uint32_t color, Face face = Face::Ui);
  int text_width(const std::string& text, Face face) const;
  // The "> Launch … Ctrl+P" ignition chip's real width, label and hint each
  // measured in their own face -- the one thing that has to stay in sync
  // between painting the chip and knowing where the tab strip may safely
  // start, or a wider label silently overlaps the first tab the way a fixed
  // 72px gap once overlapped "Ctrl+P" onto "Launch" itself.
  int launch_chip_width() const;
  // The MariaDB connection manager's Connect/Test/Save/Delete/Close row, laid
  // out from real measured label widths instead of five independently-guessed
  // fixed offsets (78, 141, 204, 267 ...) that assumed every label was no
  // wider than "Test" -- "Connect" and "Delete" weren't, and both painted
  // clipped against their own button edge while "Close" ran past the
  // dialog's border. Paint and the click handler call this so a hit always
  // lands under what is actually drawn.
  struct DbConnMgrButtons { int x[5]; int w[5]; int y; int h; };
  DbConnMgrButtons db_conn_mgr_buttons(int mx, int mw, int my, int mh) const;
  int line_height(Face face) const;
  int glyph_advance(char c, Face face) const;
  // A field label, in the console's voice: upper case, tracked out, and the
  // faintest ink on the surface. The bitmap font has one size, so tracking and
  // ink are the whole of the type hierarchy here — without them a label and its
  // value are the same word twice.
  void draw_label_to(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, const std::string& text, uint32_t color, int tracking = 1);
  int label_width(const std::string& text, int tracking = 1) const;
  // A stadium (fully-rounded) pill, which is what every chip and status badge
  // in the console is.
  void draw_pill_to(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, int w, int h, uint32_t fill, uint32_t edge);
  // A card's edge: a hairline rectangle with a SMALL corner radius. Drawing one
  // with draw_pill_to instead sets the radius to half the height, which on a
  // 300px-tall card sweeps a 150px arc across the middle of its contents.
  void draw_round_outline_to(std::vector<uint32_t>& buf, int buf_w, int buf_h, int x, int y, int w, int h, int radius, uint32_t edge);
  // Darken what is already on screen, for a real drop shadow: the framebuffer
  // is opaque, so a "translucent" black rect would just be a black rect.
  void shade_screen(int x, int y, int w, int h, int percent);
  // A soft shadow under a ROUNDED rect. Darkens by how far each pixel lies
  // outside the shape, so the halo turns the corners with it. Everything here
  // used to cast one by darkening nested RECTANGLES around a rounded panel,
  // which puts a hard-edged black box behind something with round corners —
  // and windows got three hand-rolled lines instead, lit on two sides only.
  // `spread` is how far it reaches, `strength` how dark directly under the
  // edge, `drop` how far down it is offset.
  void shadow_rounded(int x, int y, int w, int h, int radius, int spread, int strength, int drop);
  // The desktop's own background, built once per mode: the console's --ambient,
  // two soft radial washes over --bg. Rebuilding it per frame at 1920x1080 is
  // four million transcendental calls; blitting it is a memcpy.
  void build_background();
  // Rounded window corners, which is the console's single most recognisable
  // move. The frame is drawn square and then rounded off afterwards, so the
  // pixels outside the arc have to be put back to whatever was under the
  // window — another window, the desktop, anything. capture_corners() takes
  // that copy BEFORE the window is drawn and round_frame() restores from it.
  std::vector<uint32_t> capture_corners(int x, int y, int w, int h, int radius) const;
  void round_frame(int x, int y, int w, int h, int radius, uint32_t frame, const std::vector<uint32_t>& saved);

  // Hit testing
  HitRegion hit_test(int x, int y, uint32_t& out_window_id);
  int resize_edges_at(const WindowSurface& w, int x, int y) const;

  // Backend-neutral input. Every backend feeds these five calls and nothing
  // else: SDL on a hosted machine, the bare-metal PS/2 driver on the metal,
  // whatever comes later. All the window-manager behaviour — focus, drag,
  // resize, titlebar buttons, dock and launcher clicks, the terminal's action
  // chips, typing, history — lives behind them, so no backend can end up with
  // a desktop that behaves differently from another's.
  void input_mouse_motion(int x, int y);
  void input_mouse_button(int x, int y, bool down, int button = aios::hal::MouseLeft);
  void input_mouse_wheel(int notches);        // + is up, one unit per notch
  // Sideways, which a trackpad sends as its own axis and a mouse sends as
  // Shift+wheel. + is right. SDL has always delivered wheel.x and this
  // desktop threw it away, so a table wider than its pane could only be
  // walked with the pager buttons in its footer.
  void input_mouse_wheel_x(int notches);
  void input_text(const std::string& text);
  bool input_key(int key);                    // aios::hal::Key; false → leave the desktop
  // Whether the modifier that means "by the word" is down. A backend sets it
  // before feeding a key; the line editor above reads it. Kept apart from the
  // key code because Alt+Left and Left are the same key with the same code.
  void set_word_modifier(bool held) { word_wise_ = held; }
  // Shift is a modifier, not a key: Shift+Left arrives as Left. The editor
  // reads this to know whether an arrow moves the caret or drags a selection.
  void set_shift_modifier(bool held) { shift_held_ = held; }
  void submit_command(const std::string& cmd);
  // Whether a command this desktop dispatched is still running, and how to
  // stop it. A person watching the mind head down the wrong road has to be
  // able to say so; the desktop shows a Stop control while this is true.
  bool command_running() const { return command_running_.load(); }
  const std::string& running_command() const { return running_command_; }
  void set_interrupt(std::function<void()> stop) { interrupt_ = std::move(stop); }
  // Called about once a second from the frame loop. This machine has no
  // background threads, so anything periodic has to be pumped by whoever owns
  // a loop, and on a desktop machine that is this one.
  void set_ticker(std::function<void()> tick) { ticker_ = std::move(tick); }
  // Carry out one of the machine's named actions. Public because it is what a
  // binding IS: the table maps a chord to one of these, and nothing else in
  // this class decides what a key does.
  bool run_ui_action(UiAction a);
  // Take a bindings file as read from the run directory. Called by the shared
  // canvas poller, which is the only clock BOTH substrates have — the hosted
  // frame loop is behind AIOS_HAS_SDL2, so polling there meant a bare-metal
  // machine wrote a bindings file that nothing ever read back.
  void apply_key_bindings_text(const std::string& text);
  // Put a url on this machine's own web pane: raise it, leave the start page,
  // and fetch. What an application's window IS on this machine — the app runs
  // as a supervised service and its interface is served over http, so opening
  // one means pointing the engine in this kernel at it.
  void open_url(Kernel& k, const std::string& url);
  const KeyBindings& bindings() const { return keys_; }
  // Asks whatever is running to stop. Safe when nothing is.
  void stop_command();
  // Where the Stop control is, so a backend or a drive harness can aim at the
  // same rectangle the paint uses. Zero-width when nothing is running.
  struct StopGeom { int x, y, w, h; };
  StopGeom stop_geom() const;
  // Where the studio's own targets are on screen, for the same reason: a
  // backend or a drive harness aims at the rectangles the paint uses instead
  // of guessing at them. Zero-width when the studio is not up.
  struct StudioGeom {
    bool ok = false;
    int chip_y = 0;                   // the chip row's centre line
    int tree_x = 0, tree_y = 0;       // the first table in the explorer
    int tree_row_h = 18;
    int header_y = 0;                 // the grid's header row
    int cell_x = 0, cell_y = 0;       // the top-left data cell
    int row_h = 20;
    std::vector<int> column_w;        // the grid's columns, left to right
  };
  StudioGeom studio_geom() const;
  // The same, for the editor's text area: where its first drawn row starts and
  // how wide one monospace column is, so a harness can aim a drag at real text.
  struct EditorGeom {
    bool ok = false;
    int text_x = 0, text_y = 0;
    int row_h = 20;
    int char_w = 8;
  };
  EditorGeom editor_geom() const;
  // One cell of whatever the studio's grid is showing, for a harness that
  // needs to know what a sort or an edit actually did.
  std::string grid_cell(int row, int column) const;
  // Send the start page's prompt to the resident mind and clear the field.
  void canvas_submit_prompt();
  // What a desk tile does when it is clicked, and the desk's own menu.
  void desk_launch(TileKind kind);
  // A press on a row of the desk that acts on this machine — revoking a
  // capability, or putting the machine back to a snapshot. True if the click
  // was spent there. Armed by the first click, taken by the second.
  bool desk_action(int px, int py);
  // Where a command typed at this desktop goes. run_desktop* set it for their
  // own backend; a backend that drives the compositor itself sets it here.
  void set_dispatcher(CommandDispatcher d) { dispatcher_ = std::move(d); }

  // ---- the drawing surface ------------------------------------------------
  // A black sheet over the whole display that a finger, a stylus or a mouse
  // paints on. It is a FACE, like the start page and the workspace: while it
  // is up the pointer belongs to it and nothing underneath is hit-tested, so a
  // stroke across the screen cannot also drag a window it happened to cross.
  //
  // The ink is not this file's decision. draw_set_color() is what the mind
  // reaches through — from the run directory, from `aios draw color …`, from
  // anything else that can leave a line in a file — and each new stroke takes
  // whatever colour is set WHEN IT BEGINS, so a mind can change its mind
  // between strokes and a picture can be more than one colour.
  void draw_mode_set(bool on);
  bool draw_mode() const { return draw_mode_; }
  // ARGB, as every colour in this file is. Alpha is ignored; the framebuffer
  // is opaque.
  void draw_set_color(uint32_t argb);
  uint32_t draw_color() const { return draw_color_; }
  // Back to the built-in sequence, which is what an unasked stroke uses.
  void draw_color_auto();
  bool draw_color_is_auto() const { return !draw_color_set_; }
  void draw_set_width(int px);
  int draw_width() const { return draw_width_; }
  void draw_clear();
  // Take the last stroke back off the sheet. The stroke list is the record of
  // what was drawn, so an undo is a pop and a repaint — which is the whole
  // reason the strokes are kept alongside the pixels.
  void draw_undo();
  // ---- the palette --------------------------------------------------------
  // The inks have NAMES, because the side that picks them writes sentences,
  // not hex. `palette rose`, `palette 4` and `palette next` all mean something
  // a mind can say, and draw_ink_name() says back what is loaded now so the
  // choice is legible in `aios draw status` instead of being a number.
  static size_t draw_palette_size();
  static const char* draw_palette_name(size_t i);
  static uint32_t draw_palette_color(size_t i);
  // By name ("rose"), by index ("4"), or "next" to step the sequence on.
  // false = nothing in the palette answers to that.
  bool draw_select_ink(const std::string& name_or_index);
  // The name of the ink the NEXT stroke will use, or its hex when the mind set
  // a colour the palette has no name for.
  std::string draw_ink_name() const;
  size_t draw_stroke_count() const { return draw_strokes_.size(); }
  size_t draw_point_count() const {
    size_t n = 0;
    for (const auto& s : draw_strokes_) n += s.pts.size();
    return n;
  }
  // One pixel of the sheet, in ARGB — how a harness (or the machine itself)
  // can tell that a stroke actually landed without a display to look at.
  uint32_t draw_pixel(int x, int y) const;
  // Whatever arrived in the run directory, applied: one directive per line,
  // `color #rrggbb` / `width N` / `on` / `off` / `clear`. Returns what it did
  // not understand, so the mind that wrote a bad line is told.
  //
  // `settings_only` is the first pass at boot: a standing choice of ink is
  // exactly that — still true — and has to be in force before the first stroke,
  // while `on` and `clear` are things that HAPPENED and must not happen again
  // because the machine was restarted.
  std::string apply_draw_directives(const std::string& text, bool settings_only = false);
  // The pointer, in screen pixels. A backend that has a touch device of its
  // own feeds these three directly — SDL's fingers do — and everything else
  // arrives through input_mouse_*, which hands the drawing surface the same
  // three calls. One path, so a finger and a mouse cannot draw differently.
  void draw_pointer_down(int x, int y);
  void draw_pointer_move(int x, int y);
  void draw_pointer_up(int x, int y);
  // The two on-screen controls, as rectangles in screen pixels, so a harness
  // can press them the same way a thumb does. Sized for a thumb, not a cursor.
  struct DrawControlRect { int x = 0, y = 0, w = 0, h = 0; };
  DrawControlRect draw_undo_rect() const;
  DrawControlRect draw_clear_rect() const;
  // The third one: the machine's own shutter, through the same door, so a
  // thumb can keep a drawing with no keyboard anywhere near the glass.
  DrawControlRect draw_keep_rect() const;

  // ---- the machine's own shutter ------------------------------------------
  // A screenshot taken from INSIDE aios. Every pixel on this display was
  // composited into framebuffer_ by this file, so a picture of the desktop is
  // not something that has to be done TO the machine from outside with an X
  // grab or a camera: it is what the machine already holds, encoded by its own
  // PNG writer and written through its own host. That is the whole reason it
  // also works headless and on the bare-metal image, where there is nothing
  // outside to ask.
  //
  // request_screenshot() BOOKS one for the end of the frame being painted — a
  // key or a thumb arrives between frames, when the framebuffer still holds
  // the previous one, and a screenshot of the previous frame is a screenshot
  // of the wrong thing. render_desktop serves it once the frame is finished.
  // An empty path means <root>/shots/shot-<stamp>.png, chosen by the machine.
  void request_screenshot(const std::string& path = std::string());
  // Take one NOW, from a caller that knows the frame is finished (the headless
  // script does). Returns where it went, or empty if it could not be written.
  std::string take_screenshot(Kernel& k, const std::string& path = std::string());
  // Where the last one landed, so the side that asked can be told.
  const std::string& last_screenshot() const { return last_shot_; }

  // The screen the desktop is composited for. On bare metal that is whatever
  // mode the loader actually set, which is not known until boot.
  void set_screen_size(uint32_t w, uint32_t h);
  // Without threads a command runs on the frame loop; with them it runs beside
  // it and the desktop stays responsive while it works.
  void set_async_commands(bool on) { async_commands_ = on; }
  // The canvas — the machine's own lifecycle, no windows — is the face this
  // desktop shows by default; F1 switches to the windowed workspace and back.
  void set_canvas_mode(bool on) { canvas_mode_ = on; }
  bool canvas_mode() const { return canvas_mode_; }
  // The desk, for a harness that has to check where a tile ENDED UP rather
  // than trust that the click it sent did what it looked like it did.
  // What the top bar offers, in the order it reads, and which pane is current.
  // The F-keys ARE this list's positions, so a harness can prove that the key
  // a start-page card prints on its badge raises the pane that card is for --
  // the two used to be kept in step by hand, and drifted.
  struct TopBar {
    std::vector<std::string> labels;
    std::string current;            // empty when nothing has focus
  };
  TopBar top_bar() const;

  const CanvasState& canvas_state() const { return canvas_state_; }
  // What the open menu offers, and where a named item is DRAWN — so a harness
  // clicks the row the painter put there rather than counting rows itself.
  // The row geometry lives with the code that draws it, in context_menu.cpp.
  std::vector<std::string> context_menu_items() const { return menu_.open ? menu_.items : std::vector<std::string>(); }
  bool context_menu_item_at(const std::string& item, int* x, int* y) const;
  void set_console(Surface* console) { console_ = console; }
  Surface* console() const { return console_; }
  // A url to put on the web pane as soon as the desktop is up — how a dream is
  // shown on a desktop that was started to show it, and how a headless run
  // gets a page in front of the framebuffer it is about to dump.
  void set_start_url(const std::string& url) { start_url_ = url; }
  // Scripted input for a run with no person at it: "frame:type:text",
  // "frame:key:Enter|ArrowLeft|…", "frame:click:x,y". Delivered when the
  // frame counter reaches that frame, in order.
  void add_scripted_input(const std::string& step) { scripted_.push_back(step); }
  // No window system at all: paint frames into the framebuffer and dump. The
  // hosted GUI backend falls into the text UI when SDL cannot open a display,
  // and a text UI cannot show whether the glass is right.
  bool run_desktop_headless(Kernel& k, CommandDispatcher dispatcher = nullptr);
  void set_headless(bool on) { headless_ = on; }
  // Fullscreen at the display's real mode (the default: this is an OS, not an
  // app), or a window of the size the constructor was given.
  void set_fullscreen(bool on) { fullscreen_ = on; }
  // Write frame N to a PPM and leave — how a screen is checked without eyes on it.
  void set_dump(const std::string& path, int after_frames) { dump_path_ = path; dump_after_frames_ = after_frames; }
  int mouse_x() const { return mouse_x_; }
  int mouse_y() const { return mouse_y_; }

  // Window content renderers
  void render_window_content(WindowSurface& win, Kernel& k);
  void render_terminal_window(WindowSurface& win, Kernel& k);
  void render_tasks_window(WindowSurface& win, Kernel& k);
  void render_miner_window(WindowSurface& win, Kernel& k);
  void render_hardware_window(WindowSurface& win, Kernel& k);
  void render_editor_window(WindowSurface& win, Kernel& k);
  // The same pane, showing a Markdown file as it reads rather than as it is
  // typed. Only ever reached for a language that has a preview.
  void render_editor_preview(WindowSurface& win, int w, int h, int body_y, int body_h,
                             int box_x, int box_w);
  // Multi-File Edit Transactions & Atomic Diff Review (Unified Review Buffer)
  void render_editor_tx_review(WindowSurface& win, int w, int h, int body_y, int body_h,
                               int box_x, int box_w, Kernel& k);
  bool editor_tx_key(WindowSurface& win, int key, Kernel* k);
  bool editor_tx_click(WindowSurface& win, int mx, int my, Kernel* k);
  void editor_open_tx_review(WindowSurface& win, const TransactionManifest& tx);
  void editor_close_tx_review(WindowSurface& win);
  int draw_markdown_runs(WindowSurface& win, int w, int h, int x, int y, int avail,
                         const std::vector<MarkdownRun>& runs, Face base, uint32_t base_ink);
  void render_database_window(WindowSurface& win, Kernel& k);
  // The browser's pixels are drawn in web_pane.cpp, not here: this pane is an
  // application the window manager hosts, the same as the editor and the
  // studio, and the engine behind it has no business in a compositor.
  void render_browser_window(WindowSurface& win, Kernel& k);
  // Point a browser pane at a url and bring it forward. `browser <url>` at the
  // command surface comes through here, so typing it and clicking a link end
  // up in the same place.
  bool browser_open(Kernel& k, const std::string& url);

  // Native C++ AetherMail client rendering & actions
  void render_mail_window(WindowSurface& win, Kernel& k);
  // Sync open Mail windows once a frame, regardless of what is on screen.
  void pump_mail_windows();
  // Work a click asked for that must NOT run while state_mutex_ is held --
  // anything that raises a pane, because run_ui_action/ensure_app_window/
  // bring_to_front all take that lock and it is not recursive. The click
  // handlers run inside the lock; this runs once it is released.
  void defer_after_click(std::function<void()> fn) { pending_click_ = std::move(fn); }
  std::function<void()> pending_click_;
  // The mail pane's cached view. Rebuilds only when the folder, search, filter,
  // account or the store's generation has changed since it was last built.
  void refresh_mail_cache(WindowSurface& win);
  // The open message and its wrapped body, rebuilt only when the selection,
  // the store or the pane width changes.
  void refresh_mail_detail(WindowSurface& win, int wrap_px);
  // Lay out the open message's html, and paint it into the detail rect.
  void refresh_mail_html(WindowSurface& win, Kernel& k, const MailMessage& m, int view_w);
  void paint_mail_html(WindowSurface& win, int ox, int oy, int view_w, int view_h);
  void mail_open_compose(WindowSurface& win, const std::string& to = "", const std::string& subject = "", const std::string& body = "");
  void mail_reply(WindowSurface& win, bool reply_all = false);
  void mail_forward(WindowSurface& win);
  void mail_send_compose(WindowSurface& win);
  void mail_cancel_compose(WindowSurface& win);
  void mail_open_accounts(WindowSurface& win);
  void mail_save_account(WindowSurface& win);
  void mail_test_account(WindowSurface& win);
  void mail_apply_preset(WindowSurface& win, int preset_idx);

  // Native C++ AetherContacts client rendering & actions
  void render_contacts_window(WindowSurface& win, Kernel& k);
  void contacts_open_card_editor(WindowSurface& win, bool new_card = false);
  void contacts_save_card_editor(WindowSurface& win);
  void contacts_cancel_card_editor(WindowSurface& win);
  void contacts_delete_selected(WindowSurface& win);
  void contacts_toggle_fav_selected(WindowSurface& win);
  void contacts_click(WindowSurface& win, int rel_x, int rel_y, std::string& to_run);

  // Native C++ AetherCalendar client rendering & actions
  void render_calendar_window(WindowSurface& win, Kernel& k);
  void calendar_open_event_editor(WindowSurface& win, bool new_event = false);
  void calendar_save_event_editor(WindowSurface& win);
  void calendar_cancel_event_editor(WindowSurface& win);
  void calendar_delete_selected(WindowSurface& win);
  void calendar_nav_date(WindowSurface& win, int delta_months);
  void calendar_click(WindowSurface& win, int rel_x, int rel_y, std::string& to_run);

  // Native Face Applications Desk
  void face_app_load(WindowSurface& win, const std::string& path, Kernel& k);
  void render_face_app_window(WindowSurface& win, Kernel& k);
  void face_app_switch(WindowSurface& win, int index, Kernel& k);
  void face_app_reload(WindowSurface& win, Kernel& k);
  void face_app_click(WindowSurface& win, int rel_x, int rel_y);

  // Native Universal In-Desktop Inspector & Studio
  void render_inspector_window(WindowSurface& win, Kernel& k);
  void inspector_click(WindowSurface& win, int rel_x, int rel_y, std::string& to_run);
  void inspector_run_drive_step(WindowSurface& win);
  void inspector_run_all_drive_steps(WindowSurface& win);
  void inspector_reset_drive(WindowSurface& win);

  // The studio speaks to the SQL engine IN PROCESS. The grid draws the columns
  // and rows the engine handed back; it does not re-read the engine's printed
  // table, and it does not draw a fixed set of pretend rows.
  void db_connect(WindowSurface& win, Kernel& k);
  void db_refresh(WindowSurface& win, Kernel& k);
  void db_run_query(WindowSurface& win, Kernel& k, const std::string& sql);
  void db_open_table(WindowSurface& win, Kernel& k, const std::string& table);
  // The current result set as CSV, written through the host so it lands on
  // whatever this machine's substrate calls storage.
  Result<std::string> db_export_csv(WindowSurface& win, Kernel& k);

  // --- the grid as a place you change things, not just look at them --------
  // Order the rows already fetched by one column. Sorting what is on screen is
  // honest for a grid this size and keeps the statement that produced it.
  // The grid's column widths. Hit-testing and drawing MUST agree about these,
  // so both ask this rather than each working them out.
  void db_column_widths(const WindowSurface& win, int grid_w, std::vector<int>* out) const;
  // One page of the selected table: LIMIT/OFFSET around the current sort and
  // filter, never the whole table.
  void db_load_page(WindowSurface& win, Kernel& k);
  void db_set_page(WindowSurface& win, Kernel& k, int page);
  // The tables the explorer is showing, after the filter box has had its say.
  std::vector<size_t> db_visible_tables(const WindowSurface& win) const;
  void db_sort_grid(WindowSurface& win, int column);
  // Start, commit or abandon typing in one cell. A commit writes an UPDATE
  // against the row's primary key, so it changes exactly the row you edited.
  void db_begin_edit(WindowSurface& win, int row, int column);
  void db_commit_edit(WindowSurface& win, Kernel& k);
  void db_cancel_edit(WindowSurface& win);
  // Whether this grid is a table's own rows, which is what makes editing,
  // inserting and deleting meaningful.
  bool db_grid_is_table(const WindowSurface& win) const;
  void db_insert_row(WindowSurface& win, Kernel& k);
  void db_delete_selected_row(WindowSurface& win, Kernel& k);
  void db_show_ddl(WindowSurface& win, Kernel& k);
  void db_open_designer(WindowSurface& win, Kernel& k);
  void db_designer_apply(WindowSurface& win, Kernel& k);
  void db_designer_add_index(WindowSurface& win, Kernel& k);
  void db_designer_add_fk(WindowSurface& win, Kernel& k);
  void db_refresh_telemetry(WindowSurface& win, Kernel& k);
  void db_telemetry_action(WindowSurface& win, Kernel& k, const std::string& action);

  // The editor reads and writes real files through the host, so it behaves the
  // same on a hosted kernel and on the bare metal.
  bool editor_open(WindowSurface& win, Kernel& k, const std::string& path);
  // `force` writes over a file that changed under this buffer. Without it a
  // save onto a changed file is refused and says so — once. The Save chip and
  // `save` do not force; `save!` does.
  bool editor_save(WindowSurface& win, Kernel& k, const std::string& path, bool force = false);
  // What is on disk right now, as one number. Cheap, and exact for the sizes an
  // editor holds: a file that changed and came back the same length still
  // changes this.
  static uint64_t disk_mark_of(const ByteBuffer& bytes);
  // Look at the file this pane has open and notice if the machine has written
  // it since. Throttled; called from the frame loop.
  void editor_watch_file(WindowSurface& win, Kernel& k);
  void editor_browse(WindowSurface& win, Kernel& k, const std::string& dir);
  void editor_insert(WindowSurface& win, const std::string& text);
  bool editor_edit_key(WindowSurface& win, int key);
  // Keep the window's language server in step with its file: (re)attach when
  // the file changes, send didChange for edits (debounced unless `flush`).
  // Returns the client, or nullptr when no server serves this file.
  std::shared_ptr<LspClient> editor_lsp_sync(WindowSurface& win, bool flush);
  // "no language server for C++ (clangd is not on PATH)" and the like.
  std::string editor_lsp_absent(const WindowSurface& win) const;
  void editor_command(WindowSurface& win, Kernel& k, const std::string& line);

  // --- history ---------------------------------------------------------
  // Remember the buffer before a change. `coalesce` folds a run of ordinary
  // typing into one step, so undo walks back by edit rather than by keystroke.
  void editor_push_undo(WindowSurface& win, bool coalesce);
  bool editor_undo(WindowSurface& win);
  bool editor_redo(WindowSurface& win);

  // --- selection -------------------------------------------------------
  bool editor_has_selection(const WindowSurface& win) const;
  void editor_clear_selection(WindowSurface& win);
  void editor_begin_selection(WindowSurface& win);
  void editor_select_all(WindowSurface& win);
  // The selected text, and the range it covers in (line, byte-col) order.
  std::string editor_selection_text(const WindowSurface& win) const;
  void editor_selection_range(const WindowSurface& win, int* l0, int* c0, int* l1, int* c1) const;
  bool editor_delete_selection(WindowSurface& win);

  // --- terminal selection -----------------------------------------------
  struct TerminalRowInfo {
    std::string text;
    size_t line_idx;
    size_t col_offset;
    uint32_t colour;
  };
  std::vector<TerminalRowInfo> build_terminal_rows(const std::vector<std::string>& lines, int room) const;
  bool terminal_has_selection(const WindowSurface& win) const;
  void terminal_clear_selection(WindowSurface& win);
  void terminal_begin_selection(WindowSurface& win);
  void terminal_select_all(WindowSurface& win);
  std::string terminal_selection_text(const WindowSurface& win) const;
  void terminal_selection_range(const WindowSurface& win, int* l0, int* c0, int* l1, int* c1) const;
  bool terminal_point_to_caret(const WindowSurface& win, int rel_x, int rel_y, int* line, int* col) const;

  // --- find and replace ------------------------------------------------
  bool editor_find(WindowSurface& win, const std::string& needle, bool forward, bool from_caret);
  int editor_replace_all(WindowSurface& win, const std::string& from, const std::string& to);

  // --- whole-line editing ----------------------------------------------
  void editor_duplicate_line(WindowSurface& win);
  void editor_delete_line(WindowSurface& win);
  void editor_move_line(WindowSurface& win, int delta);
  void editor_indent_selection(WindowSurface& win, bool out);

  // --- tabs ------------------------------------------------------------
  // Park the live buffer in `buffers` and take another one out.
  void editor_stash_active(WindowSurface& win);
  void editor_activate_buffer(WindowSurface& win, int index);
  int editor_buffer_for_path(const WindowSurface& win, const std::string& path) const;
  void editor_close_buffer(WindowSurface& win, int index);

  // What the two applications are holding right now, so the canvas and the
  // windowed workspace can never disagree about the same file or the same
  // query.
  WorkstationState workstation();
  // The console's card — a hairline edge, radius 10, on --panel-2, with an
  // upper-case tracked heading — which is the surface every value sits on.
  void draw_card(WindowSurface& win, int w, int h, int x, int y, int cw, int ch, const std::string& heading);
  // Is the pointer over this rect of the window's CONTENT? Hover is half of
  // what makes a control look like a control; without it every chip and button
  // on the desktop is a painting of one.
  bool mouse_over_content(const WindowSurface& win, int x, int y, int w, int h) const;

  // Desktop runners
  bool run_desktop(Kernel& k, CommandDispatcher dispatcher = nullptr, bool force_tui = false);
  bool run_desktop_gui(Kernel& k, CommandDispatcher dispatcher = nullptr);
  bool run_desktop_tui(Kernel& k, CommandDispatcher dispatcher = nullptr);

  WindowSurface* focused_window();
  std::vector<WindowSurface>& windows() { return windows_; }
  const std::vector<WindowSurface>& windows() const { return windows_; }
  WindowSurface* find_window(uint32_t id) {
    for (auto& w : windows_) if (w.window_id == id) return &w;
    return nullptr;
  }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  size_t window_count() const { return windows_.size(); }
  const std::vector<uint32_t>& framebuffer() const { return framebuffer_; }
  // Leave the pointer out of the frame: the embedder draws it on the glass
  // (pointer_shape() is the same arrow). Default on.
  void set_software_pointer(bool on) { software_pointer_ = on; }
  // Somewhere for the desktop to go back to: a fourth row in the Power panel
  // (e.g. the Pi's "Dashboard"). Unset, the panel has its three.
  void set_leave_action(std::string label, std::string note, std::function<void()> action) {
    leave_label_ = std::move(label); leave_note_ = std::move(note); leave_action_ = std::move(action);
  }
  // The pointer as rows of 'X' (outline), '.' (fill), ' ' (clear); returns the row count.
  static int pointer_shape(const char* const** rows);
  // A frame reached the display (the bare-metal loop blits it itself, so the
  // SDL present path that measures the rate never runs there).
  // Measured cost of the last frame, for hosts that paint and present
  // themselves (the Studio's timing readout read 0 us on those).
  bool software_pointer_ = true;
  std::string leave_label_, leave_note_;
  std::function<void()> leave_action_;
  void note_frame_timing(float paint_ms, float present_ms) { last_paint_ms_ = paint_ms; last_present_ms_ = present_ms; }
  void note_frame_presented(uint64_t now_ms) {
    if (canvas_state_.fps_last_ms && now_ms > canvas_state_.fps_last_ms) {
      const float inst = 1000.0f / static_cast<float>(now_ms - canvas_state_.fps_last_ms);
      canvas_state_.fps = canvas_state_.fps > 0.0f ? canvas_state_.fps + 0.15f * (inst - canvas_state_.fps) : inst;
    }
    canvas_state_.fps_last_ms = now_ms;
  }

  void set_toast(const std::string& msg, int frames = 180) {
    toast_message_ = msg;
    toast_timer_ = frames;
    mark_dirty();
  }

  // Damage & dirty tracking for optimal frame scheduling
  void mark_dirty() { ui_dirty_ = true; }
  bool is_ui_dirty() const {
    if (ui_dirty_) return true;
    if (toast_timer_ > 0) return true;
    if (command_running_.load()) return true;
    if (canvas_state_.answer_running) return true;
    if (is_dragging_ || is_resizing_ || text_dragging_) return true;
    return false;
  }
  void clear_dirty() { ui_dirty_ = false; }

  // The power control. One control in the top bar opens a popover with the
  // three things a machine can actually be told to do, and every rectangle in
  // it — the control, the panel, each row — is MEASURED from the strings that
  // go in it, so a label can never be clipped by a width someone guessed.
  // The renderer and the hit test both call this, which is why a click lands
  // on the row a person is looking at.
  struct PowerGeom {
    int btn_x = 0, btn_y = 0, btn_w = 0, btn_h = 0;
    // The control the panel belongs to. The workspace's own button by default;
    // the start page sets its own, because the start page's power control is a
    // different pill in a different place and a panel that points somewhere
    // else does not read as belonging to the thing that opened it.
    int anchor_x = 0, anchor_w = 0;
    int panel_x = 0, panel_y = 0, panel_w = 0, panel_h = 0;
    int pad = 0, row_h = 0, rows = 0;
    // The i'th row's rect inside the panel.
    int row_y(int i) const { return panel_y + pad + i * row_h; }
    int row_x() const { return panel_x + pad; }
    int row_w() const { return panel_w - 2 * pad; }
  };
  PowerGeom power_geom() const;
  // Draw the open panel into the framebuffer. Both faces of this machine call
  // it — the workspace over its windows, the start page over its cards — so
  // there is ONE power panel and not two that can drift apart.
  void draw_power_menu();
  // The blank frame of a sleeping machine, painted on either face.
  void paint_asleep();
  // Open it, pointing at the control at this rect. The start page hands its
  // own pill's rect; the workspace passes nothing and gets its top-bar button.
  void power_menu_open_at(int anchor_x, int anchor_w, int below_y);
  bool power_menu_open() const { return power_menu_open_; }

  // ---- The three views, and the strip at the top edge that picks between them
  // Which face is up, said once. canvas_mode_ and the canvas's hide_panels are
  // two bools that between them name three views; asking them separately in
  // five places is how a switcher ends up disagreeing with what is on screen.
  DeskView current_view() const;
  void set_view(DeskView v);
  // The background this view is on, and putting it on another one. The scene
  // lives with the canvas state for all three views because that is what gets
  // saved with the desk.
  int view_scene(DeskView v) const { return canvas_state_.scene(v); }
  void set_view_scene(DeskView v, int scene);
  // The switcher itself: hidden until the pointer reaches the very top of the
  // screen, then drawn over whichever face is up.
  bool view_bar_open() const { return view_bar_open_; }
  // Every rectangle in it, measured from the labels that go in them — the same
  // rule the power panel follows, so renaming a view moves its chip and the
  // click with it.
  struct ViewBarGeom {
    int x = 0, y = 0, w = 0, h = 0;         // the panel
    int chip_h = 0;
    int view_y = 0, scene_y = 0;
    int view_x[3] = {0, 0, 0}, view_w[3] = {0, 0, 0};
    // The five presets, plus one more: whether the machine may show a scene
    // it composed itself rather than one of the presets.
    int scene_x[6] = {0, 0, 0, 0, 0, 0}, scene_w[6] = {0, 0, 0, 0, 0, 0};
    // How large the interface is drawn.
    int text_y = 0;
    int text_x[3] = {0, 0, 0}, text_w[3] = {0, 0, 0};
    bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  };
  ViewBarGeom view_bar_geom() const;
  void draw_view_bar();

  // ---- An application, on the face you are standing on
  // Every app this machine can open is a url, and a url is shown by ONE pane
  // that lived only on the workspace: opening anything threw you off the face
  // you were looking at, which is not what "open" should mean. The pane can be
  // drawn into a tile on the canvas as well. It is the SAME pane and the same
  // window — the tile's body IS that window's content rect, in screen pixels —
  // so the page does not exist twice and cannot disagree with itself.
  WindowSurface* app_pane_window();
  // Put an app on the current face: the tile comes out, sized and placed if it
  // has never been shown, and the pane goes to the url.
  void show_app_pane(Kernel& k, const std::string& url);
  // Just the tile: out, floating, sized and placed if it has never been shown.
  void open_app_tile();
  void paint_app_tile(std::vector<uint32_t>& buf, int iw, int ih, Kernel& k, const TileRect& r, int scale);
  // Open one identity on the face you are standing on: the inspector tile comes
  // out, sized and placed the first time, showing what this kernel holds about
  // that id.
  void show_inspect(const std::string& kind, const std::string& id);
  // Where the pane is on the canvas right now, if it is up at all: the tile's
  // body in PAINT coordinates. False when no app is open on this face.
  bool app_tile_body(const CanvasLayout& lay, int* x, int* y, int* w, int* h) const;
  // True if the press was spent on the switcher.
  bool view_bar_click(int px, int py);
  // The pointer moved: open the strip at the top edge, close it once the
  // pointer has left it. Called from input_mouse_motion on every face.
  void view_bar_track(int px, int py);
  // True if the click was spent on the panel (choosing a row, or closing it),
  // so neither face has to repeat that hit test.
  bool power_menu_took_click(int x, int y);
  bool asleep() const { return asleep_; }
  // Run option i of the panel (0 Restart, 1 Power off, 2 Sleep).
  void power_choose(int index);
  // Blank the screen and stop dispatching input to windows until something
  // wakes it; any pointer motion, any click or any key comes back to the live
  // desktop. Returns true if this call is the one that did the waking.
  void power_sleep();
  bool power_wake();

 private:
  struct PaletteItem {
    std::string id;
    std::string badge;
    uint32_t badge_bg = 0;
    uint32_t badge_fg = 0;
    std::string title;
    std::string category;
    std::string subtitle;
    std::string shortcut;
    std::function<void()> action;
    int score = 0;
  };

  // A right-click menu: what it offers depends on what is under the pointer,
  // and picking an entry acts on the window it was opened over.
  struct ContextMenu {
    bool open = false;
    int x = 0, y = 0;
    uint32_t window_id = 0;
    // Which desk tile the menu was opened over, into CanvasState::tiles, or -1
    // for the bare canvas. A menu that offers to fold a tile away has to know
    // WHICH tile, and the pointer has moved on by the time an item is picked.
    int tile_index = -1;
    std::vector<std::string> items;   // an empty string is a separator
    int hover = -1;
  };
  void open_context_menu(int x, int y);
  bool context_menu_click(int x, int y);
  void context_menu_invoke(const std::string& item);
  void draw_context_menu();

  // The workspace opens on its overview -- what this kernel is, how much of the
  // machine is in use, the workspaces and the apps with the keys that raise
  // them. It used to open on Fast Tables with the launcher up, so leaving the
  // start page landed on a search field over four rows of numbers that belong
  // to no table on this machine.
  TopTab active_top_tab_ = TopTab::Overview;
  bool palette_open_ = false;
  EditLine palette_input_;
  int palette_selected_idx_ = 0;
  std::vector<PaletteItem> palette_all_items_;
  std::vector<PaletteItem> palette_filtered_items_;
  CockpitWorkspace current_workspace_ = CockpitWorkspace::Blank;

  uint32_t width_;
  uint32_t height_;
  bool word_wise_ = false;
  bool shift_held_ = false;
  // A selection being dragged out in the editor's text, and the window it
  // belongs to. Distinct from is_dragging_, which moves a whole window.
  bool text_dragging_ = false;
  uint32_t text_drag_win_ = 0;
  ContextMenu menu_;
  bool canvas_mode_ = true;
  bool fullscreen_ = true;
  Surface* console_ = nullptr;
  CanvasState canvas_state_;
  std::string dump_path_;
  int dump_after_frames_ = 3;
  std::vector<WindowSurface> windows_;
  std::vector<uint32_t> framebuffer_;
  uint32_t active_window_id_ = 0;
  uint32_t next_window_id_ = 1;
  std::string toast_message_;
  int toast_timer_ = 0;
  bool ui_dirty_ = true;
  bool power_menu_open_ = false;
  // Revealed by the pointer touching the top edge, closed when it leaves the
  // strip. A click on a chip is spent there and reaches no face underneath.
  bool view_bar_open_ = false;
  // Which edges a resize drag has hold of, as L/R/T/B bits. A window could
  // only ever be resized by a 16x16 patch at its bottom-right corner; every
  // other desktop grabs any edge or any corner.
  enum ResizeEdge : int { EdgeL = 1, EdgeR = 2, EdgeT = 4, EdgeB = 8 };
  int resize_edges_ = 0;
  // Where the window was when the resize began, so dragging a left or top edge
  // moves that edge instead of the whole window.
  int resize_from_x_ = 0, resize_from_y_ = 0, resize_from_w_ = 0, resize_from_h_ = 0;
  int resize_grab_x_ = 0, resize_grab_y_ = 0;
  // Double-click on a title bar maximises, which is the other thing every
  // desktop does and this one did not.
  uint64_t last_title_click_ms_ = 0;
  uint32_t last_title_click_win_ = 0;
  bool ui_scale_loaded_ = false;
  bool alt_tab_open_ = false;
  int alt_tab_index_ = 0;
  // The ids the switcher is stepping through, most-recently-focused first.
  // bring_to_front already moves a window to the end of windows_, so that
  // vector IS the z-order and reading it backwards IS the focus order — there
  // is no second list here to fall out of step with it.
  std::vector<uint32_t> alt_tab_order_;
  // Whether the keyboard belongs to the app pane on the canvas. A page with a
  // text field in it is no use if every keystroke goes to the machine's prompt.
  bool app_pane_focused_ = false;
  // An app asked for before the first paint. The desk is loaded from its saved
  // file BY the painter, and that file says the app tile is closed — so a url
  // that arrived first (`desktop --open`, a dream putting itself on the glass at
  // boot) had its tile opened and then shut again a frame later by the load.
  bool app_pane_pending_ = false;
  // The scene the workspace's own ground was last built from: background_ is a
  // full-screen raster that is blitted, not recomputed, so it has to be rebuilt
  // when the workspace is put on a different background.
  int background_scene_ = -1;
  // Scene painting for the workspace keeps its OWN caches. The canvas paints at
  // half scale on a dense panel and the workspace at full, and one shared beach
  // cache between them would be re-rendered on every single frame.
  CanvasState background_scratch_;
  // Where the panel points. 0 width means "the workspace's own button".
  int power_anchor_x_ = 0, power_anchor_w_ = 0, power_anchor_below_ = 0;
  bool asleep_ = false;
  int mouse_x_ = 0;
  int mouse_y_ = 0;
  std::vector<uint32_t> background_;
  bool mouse_down_ = false;
  bool is_dragging_ = false;
  bool is_resizing_ = false;
  uint32_t active_drag_win_ = 0;
  int drag_offset_x_ = 0;
  int drag_offset_y_ = 0;
  bool async_commands_ = true;
  std::atomic<bool> command_running_{false};
  std::string running_command_;
  std::function<void()> interrupt_;
  std::function<void()> ticker_;
  FocusChangeCallback on_focus_change_cb_;
  // Which composed description the device is currently holding, so a scene is
  // uploaded when it changes and not once a frame.
  // Reads the machine's own load off /proc, statvfs and the GPU driver, at
  // most once a second. See render_canvas.
  // Where the load-meter switch was last drawn, so the hit test is measured
  // from the drawing rather than written down a second time.
  int load_toggle_x_ = -1, load_toggle_y_ = -1;
  MachineLoadSampler machine_load_sampler_;
  // What the last frame actually cost, in milliseconds: the CPU overlay paint
  // and the present. Surfaces that used to print invented constants read these.
  float last_paint_ms_ = 0.0f;
  float last_present_ms_ = 0.0f;
  // When the canvas ledger last swept the registries; it runs on a cadence,
  // not once a frame. See render_canvas.
  uint64_t canvas_sweep_last_ms_ = 0;
  uint64_t uploaded_composed_generation_ = 0;
  // The keyboard, as a table this machine can be told to change. Re-read from
  // the run directory on a clock, the way the scene and the face are, because
  // the console that edits it is a different process from this one.
  KeyBindings keys_ = KeyBindings::defaults();
  std::string keys_text_;
  uint64_t keys_checked_ms_ = 0;
  std::string browse_text_;
  bool browse_primed_ = false;
  std::string draw_text_;
  bool draw_primed_ = false;
  // The one finger a stroke belongs to. A touchscreen reports several at once
  // and only the first one down is the nib, or a second hand resting on the
  // glass takes the stroke over mid-line.
  int64_t touch_finger_ = 0;
  bool touch_finger_down_ = false;
  // The sheet. Two representations on purpose: `draw_surface_` is the retained
  // pixels, which is what makes a stroke persist across frames for nothing per
  // frame but a memcpy; `draw_strokes_` is what was actually drawn, which is
  // what lets the sheet be repainted at a new size instead of being thrown
  // away every time the window is resized.
  struct DrawPoint { int x = 0, y = 0; };
  struct DrawStroke {
    uint32_t color = 0xFFFFFFFF;
    int width = 6;
    std::vector<DrawPoint> pts;
  };
  std::vector<DrawStroke> draw_strokes_;
  std::vector<uint32_t> draw_surface_;
  uint32_t draw_surface_w_ = 0, draw_surface_h_ = 0;
  bool draw_mode_ = false;
  bool draw_drawing_ = false;          // a finger is down and painting
  int draw_last_x_ = 0, draw_last_y_ = 0;
  uint32_t draw_color_ = 0xFF57D2FF;   // what the next stroke will use
  bool draw_color_set_ = false;        // ...because the mind said so
  int draw_width_ = 7;
  size_t draw_auto_index_ = 0;         // walks the default sequence
  // Which on-screen control a press landed on, so the RELEASE on the same
  // control is what acts — a press that slides off is a cancel, the way every
  // button a thumb has ever used behaves.
  enum class DrawControl { None, Undo, Clear, Keep };
  DrawControl draw_pressed_ = DrawControl::None;
  // A screenshot booked for the end of the frame, and where the last one went.
  bool shot_pending_ = false;
  std::string shot_path_;        // where it was asked to go; empty = machine picks
  std::string last_shot_;
  uint32_t shot_serial_ = 0;
  void service_screenshot(Kernel& k);
  // Paint into the retained sheet. A segment is a line of discs so a fast drag
  // is a stroke and not a row of dots — the pointer is sampled at the frame
  // rate and a hand crosses a 1920px screen in a handful of samples.
  void draw_disc(int cx, int cy, int radius, uint32_t color);
  void draw_segment(int x0, int y0, int x1, int y1, int width, uint32_t color);
  // The curve through four consecutive samples, painted between the middle
  // two. A pointer is sampled at the frame rate, so the raw samples of a
  // drawn heart are tens of pixels apart and joining them straight gives a
  // polygon. Catmull-Rom because it passes THROUGH its points: the line still
  // goes exactly where the finger went, it just stops taking corners the
  // finger did not take.
  void draw_curve(const DrawPoint& p0, const DrawPoint& p1,
                  const DrawPoint& p2, const DrawPoint& p3,
                  int width, uint32_t color);
  // One whole stroke onto the sheet, smoothed — the one rasteriser a repaint
  // and a live drag share, so undo/resize cannot redraw a picture differently
  // from the way it was drawn.
  void draw_raster_stroke(const DrawStroke& s);
  // A whole stroke the MIND drew, rather than a finger: the points arrive at
  // once instead of one per frame. It goes onto the same stroke list a finger
  // fills, in the current ink and nib, so undo, clear, repaint-on-resize and
  // the screenshot cannot tell the two apart — which is the point. Without
  // this the machine could choose a colour but had no way to make a mark, so
  // "draw me a heart" needed a hand on the glass.
  void draw_commit_stroke(std::vector<DrawPoint> pts);
  // Segment i of a stroke (pts[i] -> pts[i+1]), smoothed with its neighbours.
  void draw_raster_segment(const DrawStroke& s, size_t i);
  DrawControl draw_control_at(int x, int y) const;
  void draw_ensure_surface();
  void draw_repaint_surface();
  void render_draw_face();
  std::string start_url_;
  std::vector<std::string> scripted_;
  bool headless_ = false;
  // Everything a console in another process leaves in the run directory for
  // this desktop to act on. Called once a frame from render_desktop, which is
  // the only point that runs on BOTH faces and BOTH substrates — putting this
  // in the canvas painter meant it stopped the moment anyone left the start
  // page, so an app that raised the web pane could never be replaced.
  void poll_run_directory(Kernel& k);
  void draw_stop_control();
  aios::hal::Mutex state_mutex_;
  // The first window of a kind, for the launchers and the function keys. 0 when
  // this desktop has none.
  uint32_t first_of_type(WindowType t);
  // The terminal keystrokes go to. The caller must hold state_mutex_; null when
  // no terminal has focus.
  WindowSurface* focused_terminal();
  std::vector<std::string> command_history_;
  int history_index_ = -1;
  CommandDispatcher dispatcher_;
  // The kernel the last frame was drawn for. Input arrives without one — a
  // click is not a frame — and the two applications have to reach the
  // substrate from inside a click.
  Kernel* kernel_ = nullptr;
  // A click inside either application's content, in window-content coordinates.
  // Both run with state_mutex_ held; anything they want the shell to run comes
  // back out through `to_run`.
  void editor_click(WindowSurface& w, int rel_x, int rel_y, std::string& to_run);
  // Where a point inside the editor's text area lands in the buffer. Clicking
  // and dragging both ask this, so a drag ends exactly where a click would.
  bool editor_point_to_caret(const WindowSurface& w, int rel_x, int rel_y, int* line, int* col) const;
  void db_click(WindowSurface& w, int rel_x, int rel_y, std::string& to_run);
  // The studio's engine, opened once and kept: SqlDatabase::init() reloads
  // every table off disk, which is not something to do per keystroke.
  std::unique_ptr<TaskLedger> sql_ledger_;
  std::unique_ptr<SqlDatabase> sql_;
  SqlDatabase* sql_engine(Kernel& k);
};

}  // namespace aios::userland
