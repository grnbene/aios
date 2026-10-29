// What each console command does, and each of its subcommands: the one place that text lives. `help`, `help <cmd>`,
// `man <cmd>` and docs/reference.md all read it, and `scripts/gen-reference.py --check` fails when a handler in
// cli/ accepts a subcommand this table does not describe (or this table describes one no handler accepts).
//
// The format is strict so the generator can read it: one command per `{"name", "SECTION", "line", "what", {` line,
// one subcommand per `{"words", "usage", "what"},` line. `words` is the subcommand and its aliases, separated by |.
#include "command_docs.hpp"

#include <cstring>

namespace aios::cli {

const std::vector<CommandDoc>& command_docs() {
  static const std::vector<CommandDoc> docs = {
  // ---- SYSTEM & ENVIRONMENT
  {"info", "SYSTEM & ENVIRONMENT", "info", "This machine: kernel, root, uptime, host, memory fabric, entities, devices", {
  }},
  {"sysstat", "SYSTEM & ENVIRONMENT", "sysstat", "System statistics and health in one view", {
  }},
  {"devices", "SYSTEM & ENVIRONMENT", "devices", "Compute devices: CPU, GPU, NPU and remote ones", {
  }},
  {"storage", "SYSTEM & ENVIRONMENT", "storage", "Block devices and where the root volume is", {
  }},
  {"network", "SYSTEM & ENVIRONMENT", "network", "Network interfaces, addresses and link state", {
  }},
  {"net", "SYSTEM & ENVIRONMENT", "net check|repair", "Measure packet loss, and try to lower it", {
    {"check", "net check [ms]", "packet loss and receive load over [ms] (700 by default), and each interface"},
    {"repair|correct", "net repair", "widen the receive refill window, then measure again and report the difference"},
  }},
  {"load", "SYSTEM & ENVIRONMENT", "load | host load", "Measure load, set a quota in proportion to it, measure again", {
  }},
  {"whoami", "SYSTEM & ENVIRONMENT", "whoami | id", "The console's entity, and the kernel it runs on", {
  }},
  {"identity", "SYSTEM & ENVIRONMENT", "identity [set <text>]", "What this machine says it is", {
    {"set", "identity set <text>", "restate it; kept in the store"},
  }},
  {"subsystems", "SYSTEM & ENVIRONMENT", "subsystems [-v]", "What the hardware can do, asked of the kernel now", {
    {"-v|full", "subsystems -v", "also the devices behind each answer"},
  }},
  {"bus", "SYSTEM & ENVIRONMENT", "bus list|peer", "PCI devices, and copies between them", {
    {"list", "bus list", "PCI devices and their memory windows"},
    {"peer", "bus peer <src-slot> <dst-slot> [bytes]", "copy between two devices directly"},
  }},
  {"attest", "SYSTEM & ENVIRONMENT", "attest status|pcrs|quote", "TPM attestation", {
    {"status", "attest status", "whether a TPM is present and usable"},
    {"pcrs", "attest pcrs [n...]", "PCR values"},
    {"quote", "attest quote [n...] [nonce=<text>]", "a signed quote over PCRs"},
  }},
  {"watchdog", "SYSTEM & ENVIRONMENT", "watchdog", "Health checks run, recoveries, heap use and recent events", {
  }},
  {"reboot", "SYSTEM & ENVIRONMENT", "reboot", "Restart the machine", {
  }},
  {"poweroff", "SYSTEM & ENVIRONMENT", "poweroff", "Shut down the machine", {
  }},
  {"sleep", "SYSTEM & ENVIRONMENT", "sleep <sec>", "Pause for that many seconds", {
  }},
  // ---- AUTHORITY & SECURITY
  {"authority", "AUTHORITY & SECURITY", "authority [mine|residents|log|refusals]", "Capabilities held, and what was granted or refused", {
    {"mine", "authority mine", "what the console holds"},
    {"residents", "authority residents", "what each resident entity holds"},
    {"log", "authority log [n]", "the last n grants and uses"},
    {"refusals", "authority refusals", "what was refused, and why"},
  }},
  {"cap", "AUTHORITY & SECURITY", "cap grant|request|delegate|revoke|list|find|check", "Capabilities", {
    {"grant", "cap grant <e> <kind> <id|*> <rights> [ttl_s] [scope_bytes]", "give an entity rights over a resource"},
    {"request", "cap request <kind> <id|*> <rights> [ttl_s] [confirm=<id>]", "ask the kernel for rights"},
    {"delegate", "cap delegate <e> <cap> <to> <rights> [ttl_s]", "pass on part of a capability"},
    {"revoke", "cap revoke <cap>", "revoke a capability and what was delegated from it"},
    {"list", "cap list [e]", "capabilities held, by everyone or one entity"},
    {"mine", "cap mine", "what the console holds"},
    {"find", "cap find <e> <kind> [id|*]", "the capability an entity holds for a resource"},
    {"check", "cap check <e> <kind> <id|*> <right>", "whether an entity holds a right; fails if not"},
  }},
  {"quota", "AUTHORITY & SECURITY", "quota list|set|clear|reset", "Time quotas per entity", {
    {"list|show", "quota list", "each bounded entity: slice, budget, used, period, stops"},
    {"set", "quota set <e> budget=<ns> [slice=<ns>] [period=<ns>]", "bound an entity's time"},
    {"clear", "quota clear <e>", "remove an entity's quota"},
    {"reset", "quota reset", "reopen every accounting window"},
  }},
  {"gateway", "AUTHORITY & SECURITY", "gateway serve|token|route|status", "Token-gated HTTP gateway for remote compute", {
    {"status", "gateway status", "port, TLS and this node's load"},
    {"token|mint", "gateway token [holder=H] [scope=N] [duration_s=S]", "mint a token for a holder"},
    {"route", "gateway route <req.json|json> [token=T]", "route one request as the gateway would"},
    {"serve", "gateway serve [port=7310] [public=1] [secret=S] [tls=1 cert=PATH key=PATH] [cors=ORIGIN]", "run the gateway"},
  }},
  {"asqlcap", "AUTHORITY & SECURITY", "asqlcap mint|attenuate|show|user", "ASQL credentials and login users", {
    {"mint", "asqlcap mint tables=db.t,db.* rights=read[,write,...] [ttl=<s>] [rows=<n>]", "a credential signed with this machine's capability key (1 hour by default)"},
    {"attenuate", "asqlcap attenuate <credential> [tables=..] [rights=..] [ttl=<s>] [rows=<n>]", "narrow a credential; needs no key"},
    {"show", "asqlcap show <credential>", "its rights, tables, expiry and row cap"},
    {"user", "asqlcap user add <name> password=<pw> tables=db.* [rights=..] | password <name> password=<pw> | del <name> | list", "users who log in with a password; the running server reads the same file"},
  }},
  // ---- ENTITIES & MEMORY FABRIC
  {"entity", "ENTITIES & MEMORY FABRIC", "entity create|list|show|relate|terminate", "Entities", {
    {"create", "entity create <label> [parent]", "a new entity"},
    {"list", "entity list", "every entity"},
    {"show", "entity show <e>", "one entity"},
    {"relate", "entity relate <a> <b> <kind>", "record a relation between two"},
    {"terminate", "entity terminate <e>", "end an entity"},
  }},
  {"mem", "ENTITIES & MEMORY FABRIC", "mem alloc|read|write|migrate|share|release|list|compact", "Memory regions", {
    {"alloc", "mem alloc <e> <bytes> [system|shared|persistent]", "allocate a region"},
    {"write", "mem write <e> <mem> <offset> <text|hex:..>", "write into it"},
    {"read", "mem read <e> <mem> <offset> <n>", "read from it"},
    {"migrate", "mem migrate <e> <mem> <type>", "move it to another memory type"},
    {"share", "mem share <e> <mem> <to> <rights>", "share it with another entity"},
    {"release", "mem release <e> <mem>", "free it"},
    {"list", "mem list [e]", "regions, for one entity or all"},
    {"compact|gc", "mem compact", "free memory whose owner no longer exists"},
  }},
  {"state", "ENTITIES & MEMORY FABRIC", "state create|set|get|attach|keep|list", "State objects", {
    {"create", "state create <e> <working|persistent|context|relationship|execution> [text]", "a new state object"},
    {"set", "state set <e> <s> <text>", "replace its contents"},
    {"get", "state get <e> <s>", "read it"},
    {"attach", "state attach <e> <s> <mem>", "attach a memory region to it"},
    {"keep", "state keep <e> <s>", "keep it in the root, so it is there after a restart"},
    {"list", "state list [e]", "state objects, for one entity or all"},
  }},
  {"kv", "ENTITIES & MEMORY FABRIC", "kv set|get|del|list|clear", "Key-value store kept in state objects", {
    {"set", "kv set <key> [value]", "set a key"},
    {"get", "kv get <key>", "read a key"},
    {"del|delete|remove|rm", "kv del <key>", "remove a key"},
    {"list|ls", "kv list", "every key"},
    {"clear", "kv clear", "remove every key"},
  }},
  {"checkpoint", "ENTITIES & MEMORY FABRIC", "checkpoint <e>", "Save an entity's state to a file", {
  }},
  {"restore", "ENTITIES & MEMORY FABRIC", "restore <path>", "Bring back an entity from a checkpoint", {
  }},
  {"rewind", "ENTITIES & MEMORY FABRIC", "rewind <path>", "Put a running entity back to a checkpoint", {
  }},
  // ---- COMPUTE & GRAPH EXECUTION
  {"tensor", "COMPUTE & GRAPH EXECUTION", "tensor create|show", "Typed views of memory", {
    {"create", "tensor create <e> <f32|f64|i32|i64|u8> <shape> [values...]", "a tensor in new memory"},
    {"show", "tensor show <e> <mem> <dtype> <shape>", "read memory as a tensor"},
  }},
  {"compute", "COMPUTE & GRAPH EXECUTION", "compute kernels|serve", "Compute kernels, and the compute/SQL server (ASQL)", {
    {"kernels", "compute kernels", "the kernels each device runs"},
    {"serve", "compute serve [port=7311] [lanes=N] [token=T] [public=1] [sql=1] [mysql_port=P] [cdc_host=H cdc_db=DB ...]", "serve compute; with sql=1, ASQL: a MySQL-speaking database that can mirror a MariaDB (see docs/asql.md)"},
  }},
  {"sat", "COMPUTE & GRAPH EXECUTION", "sat devices|solve|fleet", "SAT solver", {
    {"devices", "sat devices", "devices that can run the solver"},
    {"solve", "sat solve FILE [on=SEL] [conflicts=N] [check_on=SEL]", "solve a DIMACS formula: a model, or proof it has none"},
    {"fleet", "sat fleet FILE on=SEL,SEL,... [budget=N] [depth=D]", "split a formula across several devices or members"},
  }},
  {"graph", "COMPUTE & GRAPH EXECUTION", "graph create|op|plan|partition|run|show", "Compute graphs", {
    {"create", "graph create <e>", "a new graph"},
    {"op", "graph op <e> <g> <opcode> [in=..] [out=..] [scalar=x] [par=n] [prio=n] [dev=cpu] [after=<op>]", "add an operation"},
    {"plan", "graph plan <e> <g>", "where each operation would run"},
    {"partition", "graph partition <e> <g> [link=<bytes/s>]", "split it across devices for a given link speed"},
    {"run", "graph run <e> <g> [steps]", "run it"},
    {"show", "graph show <g>", "its operations"},
  }},
  // ---- CHANNELS & OBJECT STORE
  {"channel", "CHANNELS & OBJECT STORE", "channel create|find|admit|subscribe|publish|consume|list", "State channels (ring buffers, pub/sub)", {
    {"create", "channel create <e> <capacity> <slot_bytes> [transport=network:<addr>]", "a new channel"},
    {"find", "channel find <owner>", "the channel an entity owns"},
    {"admit", "channel admit <e> <ch> <to> <rights>", "let another entity use it"},
    {"subscribe", "channel subscribe <e> <ch>", "subscribe to it"},
    {"publish", "channel publish <e> <ch> <state>", "publish a state into it"},
    {"consume", "channel consume <e> <ch>", "take the next item"},
    {"list", "channel list", "every channel"},
  }},
  {"sql", "CHANNELS & OBJECT STORE", "sql query|console|serve|import|push|sync|tables", "The machine's SQL engine", {
    {"query|exec", "sql query \"<sql>\"", "run a statement"},
    {"pull|import", "sql import <file.sql> | file=<path> | sql=\"<statements>\" [customer=<cid>]", "run statements from a file or the line"},
    {"push|dump|export", "sql dump [customer=<cid>] [table=tasks]", "a table as SQL statements"},
    {"sync", "sql sync [customer=<cid>]", "sync the task ledger with its SQL tables"},
    {"tables|list", "sql tables", "every table"},
    {"serve", "sql serve [port=3307] [bind=<addr>] [max_connections=N]", "answer MySQL clients"},
    {"console|repl|cli", "sql console", "an interactive SQL prompt"},
  }},
  {"ingress", "CHANNELS & OBJECT STORE", "ingress receive|ingest|log|verify", "Payloads received from outside, kept with a hash", {
    {"receive|file|upload", "ingress receive file=<path> [customer=<cid>] [target=<name>]", "take in a file"},
    {"ingest|data|payload", "ingress ingest customer=<cid> [filename=<name>] payload=\"<data>\"", "take in data given on the line"},
    {"log|list|status", "ingress log [customer=<cid>]", "what was received"},
    {"verify", "ingress verify <payload_id>", "check a payload against its hash"},
  }},
  {"feed", "CHANNELS & OBJECT STORE", "feed ingest|list", "RSS and Atom feeds into the store", {
    {"ingest|fetch", "feed ingest <url> [as=$e]", "read a feed and store its items"},
    {"list|ls", "feed list [url]", "stored items, for one feed or all"},
  }},
  {"store", "CHANNELS & OBJECT STORE", "store put|get|query|similar|related|history|list|status", "The object store", {
    {"put", "store put <e> <text> [derived=<obj>] [rel=<obj>:<kind>] [k=v]...", "store an object"},
    {"get", "store get [<e>] <obj> [show=<chars>]", "an object and what it says"},
    {"query", "store query [<e>] <aql_statement>", "an AQL query"},
    {"similar", "store similar <text…> [k]", "the k objects closest in meaning"},
    {"related", "store related <obj>", "objects related to one"},
    {"history", "store history <obj>", "an object's versions"},
    {"snapshot", "store snapshot <e> <state>", "store a copy of a state object"},
    {"meta", "store meta <key> [value]", "objects with that metadata"},
    {"list", "store list", "every object"},
    {"status|blocks", "store status", "objects held in memory and in sealed blocks, and the cache"},
    {"seal", "store seal", "move active objects into sealed blocks"},
    {"index", "store index [bench]", "the similarity index: size, path a query takes, and its measured accuracy; bench times it"},
  }},
  // ---- INTELLIGENCE & DISTRIBUTED
  {"ask", "INTELLIGENCE & DISTRIBUTED", "ask [mind=<resident>] [cycles=N] <request>", "Hand a request to a resident mind", {
  }},
  {"mind", "INTELLIGENCE & DISTRIBUTED", "mind status|set|bind|swap|cycle|unbind", "The mind an entity runs", {
    {"status|info", "mind status", "the provider and model in use"},
    {"set|provider|model", "mind set <provider> [model] | mind provider <p> | mind model <m>", "change the console's provider or model"},
    {"bind", "mind bind <e> echo|plugin <path>|extern <cmd...>|wire|local|qwen [<url>] [<model>]|jepa [out=<ch>]", "give an entity a mind"},
    {"swap", "mind swap <e> ...", "replace an entity's mind while it runs"},
    {"cycle", "mind cycle <e> [n]", "run n cycles"},
    {"unbind", "mind unbind <e>", "take its mind away"},
  }},
  {"infer", "INTELLIGENCE & DISTRIBUTED", "infer load|status|ask|serve", "Run a local Qwen2 checkpoint", {
    {"load", "infer load model=<dir> [adapter=<dir>] [context=N]", "load a checkpoint"},
    {"status", "infer status", "what is loaded"},
    {"ask", "infer ask <text> [temperature=T] [top_p=P] [max_tokens=N] [seed=S]", "generate an answer"},
    {"serve", "infer serve [port=8000] [name=spark] [public=1 key_file=F]", "an OpenAI-style completions API"},
  }},
  {"qwen", "INTELLIGENCE & DISTRIBUTED", "qwen [local] status|ask", "A Qwen model, on a server or in this process", {
    {"status", "qwen status", "the server it would use, and whether it answers"},
    {"ask|prompt|query", "qwen ask <text>", "ask it"},
    {"local|cuda|in-process", "qwen local status|ask <text>", "the same, using the in-tree model (infer)"},
  }},
  {"lm", "INTELLIGENCE & DISTRIBUTED", "lm status|train|tokenize|check|ask", "The built-in small language model", {
    {"status", "lm status", "its size, training and corpus"},
    {"train", "lm train [steps] [lr] [corpus=<file>] [save_every=N] [engine=scalar]", "train it"},
    {"tokenize", "lm tokenize <text>", "how it splits text"},
    {"check", "lm check [n] [sabotage=<slot>]", "check its gradients"},
    {"ask|say", "lm ask <request>", "ask it"},
  }},
  {"jepa", "INTELLIGENCE & DISTRIBUTED", "jepa status|record|train|tick|...", "The world model that learns this machine", {
    {"status", "jepa status", "steps trained, samples held, loss, and the levers it can pull"},
    {"record", "jepa record [count=N]", "record real transitions: read the machine, act, read again"},
    {"synthesize|gen", "jepa synthesize [count=N]", "generate synthetic transitions (not measured)"},
    {"train", "jepa train [steps=N]", "train on the transitions held"},
    {"daemon", "jepa daemon", "record and train in a loop, to a bound"},
    {"tick", "jepa tick [count=N]", "one step of sensing and acting"},
    {"predict", "jepa predict", "the next state it predicts"},
    {"policy", "jepa policy", "the action its policy picks now"},
    {"shm|region", "jepa shm", "the shared-memory region it reads and writes"},
    {"latency", "jepa latency [count=N]", "time per step"},
    {"bench", "jepa bench [steps] [batch]", "the same work on the CPU and an accelerator"},
    {"sweep", "jepa sweep", "whether the measure the reward reads responds to each lever"},
    {"gpu", "jepa gpu [off]", "run the predictor's matrix products on an accelerator, or not"},
    {"template|layout|ui", "jepa layout", "the layout its action vector proposes"},
    {"export", "jepa export <path>", "write the model and policy to files"},
    {"import", "jepa import <path>", "read them back"},
    {"reboot-resume", "jepa reboot-resume", "reload what it learned (run at every start)"},
  }},
  {"fleet", "INTELLIGENCE & DISTRIBUTED", "fleet hub|join|members|run|status|...", "Machines working as one", {
    {"status|telemetry|load", "fleet status", "each member's load, jobs and kernels"},
    {"hub", "fleet hub <port> <cluster-key|@FILE|env:NAME> [bind=..] [members=<file>] [attest=require]", "be the fleet's meeting point and relay"},
    {"identity", "fleet identity", "this machine's public key, for the hub's roster"},
    {"join", "fleet join <hub-host> <hub-port> <cluster-key> [retry] [member-key=..] [attest=1] [hub-key=..]", "join through a hub"},
    {"members", "fleet members", "who is in the fleet"},
    {"run", "fleet run <peer-id-prefix> <command...|@FILE>", "run a command on a member"},
    {"observe", "fleet observe <hub-host> <hub-port> <key> [status-file=..] [interval-ms=4000]", "join read-only and keep writing the fleet's state to a JSON file"},
    {"slots", "fleet slots [n]", "how many jobs this machine runs at once"},
    {"watermarks", "fleet watermarks <high> <low>", "the load fractions at which work moves away or is taken in"},
    {"plan", "fleet plan", "where work would move (nothing is done)"},
    {"tick", "fleet tick", "make one round of those moves"},
    {"watch", "fleet watch [ticks] [ms]", "run the scheduler for a number of ticks"},
    {"replicate", "fleet replicate [node-index]", "send recovery points to a peer"},
    {"replicas", "fleet replicas", "recovery points this machine holds for others"},
    {"log", "fleet log", "the scheduler's decisions"},
    {"sql", "fleet sql [writes=on|off]", "whether members may write through this machine's SQL engine"},
    {"db", "fleet db", "databases the members host"},
  }},
  {"node", "INTELLIGENCE & DISTRIBUTED", "node serve|connect|list|peers|memory|migrate", "Direct links between kernels", {
    {"serve", "node serve <port> <token|@FILE|env:NAME> [bind=127.0.0.1|0.0.0.0]", "accept links"},
    {"connect", "node connect <host> <port> <token|@FILE|env:NAME>", "link to another kernel and use its devices"},
    {"list", "node list", "links this machine made"},
    {"peers", "node peers", "links made to this machine"},
    {"memory", "node memory [<node-index>]", "which linked node new remote memory goes to"},
    {"migrate", "node migrate <e> [node-index] [keep]", "move a running entity to a linked kernel"},
  }},
  {"p2p", "INTELLIGENCE & DISTRIBUTED", "p2p status|peers|ping|store|find-value|find-node|mdns", "LAN discovery and a DHT", {
    {"status", "p2p status", "this node's DHT state"},
    {"peers", "p2p peers [wait=6000]", "listen for LAN beacons and list who was heard"},
    {"ping", "p2p ping <node_id> <ip> [port]", "ping a node"},
    {"store", "p2p store key=<k> value=<v>", "store a value in the DHT"},
    {"find-value", "p2p find-value <key>", "look up a value"},
    {"find-node", "p2p find-node <target_id>", "the nodes closest to an id"},
    {"mdns", "p2p mdns announce [every=S] [count=N] | parse <payload> | stop", "mDNS beacons"},
  }},
  {"voice", "INTELLIGENCE & DISTRIBUTED", "voice speak|listen|hear|converse|status|voices", "Speech in and out", {
    {"speak|say", "voice speak <text...> [out=<file.wav>]", "say it, or write it to a file"},
    {"listen", "voice listen [dur=<sec>]", "record from the microphone and transcribe"},
    {"hear|transcribe", "voice hear <file.wav>", "transcribe a recording"},
    {"chat|converse", "voice converse [prompt=<text>]", "a spoken exchange with the mind"},
    {"status", "voice status", "what this machine can do with sound"},
    {"voices", "voice voices", "voices available"},
    {"shm|pull", "voice shm", "read the state and action vectors the world model shares in memory"},
  }},
  // ---- APPLICATIONS & SHELL
  {"serve", "APPLICATIONS & SHELL", "serve [port=7480] [all-ips=1]", "This machine's app host; runs until stopped, its apps with it", {
  }},
  {"app", "APPLICATIONS & SHELL", "app list|deploy|status|logs|start|stop|...", "Apps: grain apps on an app host, and packaged services", {
    {"list|status", "app list", "installed apps and their state, and what is packaged"},
    {"install", "app install <name>", "install a packaged app"},
    {"add", "app add <name> \"<command>\" url=<url> [dir=<path>] [env=K=V] [atboot]", "register a service as an app"},
    {"start|stop", "app start|stop <name> [on <member>|ALL]", "start or stop an app"},
    {"remove|uninstall", "app remove <name>", "remove an app"},
    {"open|show", "app open <name>", "its interface in this machine's web pane"},
    {"deploy", "app deploy <dir> [at HOST | on <member>|ALL]", "a grain app's new version: kept, started, checked; rolled back if it fails"},
    {"logs", "app logs <app> [-n N] [-f]", "a grain app's log"},
    {"rollback", "app rollback <app>", "the version before this one"},
    {"versions", "app versions <app>", "the versions it has run"},
    {"restart", "app restart <app>", "restart a grain app"},
    {"set|unset", "app set <app> KEY=VALUE... | app unset <app> KEY...", "this machine's settings for it"},
    {"secret", "app secret <app> <NAME>", "a secret, read from stdin and kept sealed on that machine"},
    {"settings|secrets", "app settings|secrets <app>", "the names set for it"},
    {"check", "app check <dir>", "the manifest and every file compile"},
    {"pack", "app pack <dir>", "pack it into one bundle"},
    {"run", "app run <dir>", "run it here in the foreground"},
    {"hosts", "app hosts", "the hosts `at NAME` knows"},
    {"add-host", "app add-host <name> <address[:port]> key=<key|@file>", "remember a host"},
    {"help", "app help", "the grain app commands in full"},
  }},
  {"grain", "APPLICATIONS & SHELL", "grain run|eval|repl|dis|serve|ps", "The grain language", {
    {"run", "grain run <file.gr> [args...]", "run a program"},
    {"eval", "grain eval <source>", "run one line and print its value"},
    {"repl", "grain repl", "an interactive prompt"},
    {"dis", "grain dis <file.gr>", "the bytecode the compiler made"},
    {"serve", "grain serve <file.gr> [--port N]", "a program's ui.pane in a browser"},
    {"ps", "grain ps [--json]", "grain programs, app hosts and ASQL engines running here"},
    {"help|-h", "grain help", "these"},
  }},
  {"display", "APPLICATIONS & SHELL", "display status|show|describe|desk|auto", "What the desktop shows", {
    {"status|list", "display status", "the scene shown and who chose it"},
    {"show|scene|set", "display show <ocean|cyber|orb|pyramid|anything>", "show a scene; a name it does not know is composed from the words"},
    {"describe|description", "display describe", "the description of the scene shown"},
    {"desk|panels", "display desk", "put the desk (ledger, authority, rewind, brief) back on the display"},
    {"auto|release", "display auto", "let go of the scene chosen"},
  }},
  {"scene", "APPLICATIONS & SHELL", "scene [name]", "The scene the desktop paints", {
  }},
  {"keys", "APPLICATIONS & SHELL", "keys list|actions|bind|unbind|reset", "Key bindings", {
    {"list|status", "keys", "every binding"},
    {"actions", "keys actions", "the actions a key can be bound to"},
    {"bind", "keys bind <chord> <action>", "bind a key (F1, Ctrl+R, Escape, Space, or one character)"},
    {"unbind", "keys unbind <chord>", "remove a binding"},
    {"reset|default|defaults", "keys reset", "back to the defaults"},
  }},
  {"js", "APPLICATIONS & SHELL", "js <program> | js run <file> | js features", "The built-in JavaScript engine", {
    {"features|support", "js features", "what the engine supports"},
    {"run|file", "js run <path>", "run a file"},
  }},
  {"browser", "APPLICATIONS & SHELL", "browser <url>|follow|back|click|type|dom", "The built-in web browser", {
    {"follow", "browser follow <n>", "follow link n"},
    {"back|forward|reload", "browser back|forward|reload", "move through the history"},
    {"page", "browser page", "the page as text again"},
    {"click|tap", "browser click <x> <y>", "click at a point"},
    {"type|key", "browser type <x> <y> <text> | browser key <x> <y> <Enter|Escape|Backspace>", "type into the field at a point"},
    {"eval|js", "browser eval <expression>", "run script on the page"},
    {"net|requests", "browser net", "requests the page's scripts made"},
    {"log|console", "browser log", "what the scripts ran and printed"},
    {"dom|tree", "browser dom", "the document tree"},
    {"boxes|layout", "browser boxes", "the layout boxes"},
  }},
  {"gui", "APPLICATIONS & SHELL", "gui [desktop [--tui]]", "The desktop", {
    {"desktop|native|run", "gui desktop [--tui]", "start it; --tui in the terminal"},
  }},
  {"face", "APPLICATIONS & SHELL", "face reference|look|text|open|source", "Face, the UI language", {
    {"reference|language|help", "face reference", "the language reference"},
    {"look", "face look <name|file> [type:… key:… click:x,y action:…]…", "render it, optionally after input"},
    {"text", "face text <name|file>", "render it as text"},
    {"open|show", "face open <name|file>", "open it on the desktop"},
    {"source", "face source <name|file>", "its source"},
  }},
  {"dream", "APPLICATIONS & SHELL", "dream <what you want> | dream list|show|open|remove", "Make an app from a description", {
    {"list", "dream list", "apps made this way"},
    {"kinds", "dream kinds", "the kinds of records they keep"},
    {"show", "dream show <name>", "what was asked, and what it keeps"},
    {"look", "dream look <name> [input…]", "render it"},
    {"open|revisit", "dream open <name>", "open it"},
    {"face|source", "dream face <name>", "its face source"},
    {"remove", "dream remove <name>", "delete it"},
  }},
  {"organ", "APPLICATIONS & SHELL", "organ list|grow|call|source|remove", "C++ extensions written, compiled and loaded on request", {
    {"list", "organ list", "organs loaded"},
    {"grow|make", "organ grow <name> <what it must do…>", "write, compile and load one"},
    {"call", "organ call <name> <request…>", "call it"},
    {"source", "organ source <name>", "its source"},
    {"remove", "organ remove <name>", "unload and delete it"},
  }},
  {"mail", "APPLICATIONS & SHELL", "mail list|search|read|send|sync|account", "Mail", {
    {"list|ls", "mail list [folder]", "messages in a folder (inbox by default)"},
    {"search|find|query", "mail search [--server] <query>", "search (from:, subject:, body:, has:attachment); --server asks the server"},
    {"read|cat|view|show", "mail read <message-id>", "read a message"},
    {"send", "mail send <to> <subject> <body>", "send a message"},
    {"flag|star", "mail flag <message-id>", "flag or unflag a message"},
    {"delete|rm|trash", "mail delete <message-id>", "delete a message"},
    {"archive", "mail archive <message-id>", "archive a message"},
    {"folders|boxes", "mail folders", "mailboxes"},
    {"status", "mail status", "accounts and counts"},
    {"sync|check", "mail sync", "check for new mail"},
    {"vacuum|gc", "mail vacuum [--days N] [--cap MB]", "drop stored message bodies older than N days or over the cap"},
    {"account|accounts|acc", "mail account list|add|preset|set-pass|test|remove|set-default", "mail accounts"},
  }},
  {"google", "APPLICATIONS & SHELL", "google auth|status|sync|watch|probe|logout", "A Google account's mail, calendar, contacts and notes", {
    {"auth|login|signin", "google auth [you@gmail.com] [--manual] | google auth finish <address>", "sign in (OAuth)"},
    {"status", "google status", "accounts signed in"},
    {"sync", "google sync [mail|calendar|contacts|notes ...] [account=]", "sync now"},
    {"watch", "google watch [every=300]", "keep syncing"},
    {"probe", "google probe", "whether this machine can reach Google, without signing in"},
    {"logout|revoke|signout", "google logout [account=]", "sign out and revoke the token"},
  }},
  {"contacts", "APPLICATIONS & SHELL", "contacts list|search|show|add|delete", "Address book", {
    {"list|ls", "contacts list [group]", "contacts"},
    {"search|find", "contacts search <query>", "search"},
    {"show|read|get", "contacts show <id>", "one contact"},
    {"add|new", "contacts add <first> <last> <email> [phone] [company]", "add one"},
    {"delete|rm", "contacts delete <id>", "delete one"},
    {"favorite|fav", "contacts favorite <id>", "mark or unmark a favorite"},
    {"groups", "contacts groups", "groups"},
    {"export", "contacts export", "as vCard"},
    {"status", "contacts status", "counts"},
  }},
  {"calendar", "APPLICATIONS & SHELL", "calendar list|show|add|delete", "Calendar", {
    {"list|ls|agenda", "calendar list", "events"},
    {"show|get", "calendar show <event-id>", "one event"},
    {"add|new", "calendar add <title> <YYYY-MM-DD> <HH:MM> [duration_mins] [category] [location]", "add one"},
    {"delete|rm", "calendar delete <event-id>", "delete one"},
    {"categories|cats", "calendar categories", "categories"},
    {"export", "calendar export", "as iCalendar"},
    {"status", "calendar status", "counts"},
  }},
  {"task", "APPLICATIONS & SHELL", "task create|list|status|show|ledger|speak", "Task ledger", {
    {"create|add|new", "task create customer=<cid> name=<name> payload=<payload> [priority=<p>]", "a new task"},
    {"list|ls", "task list [customer=<cid>] [status=<st>]", "tasks"},
    {"status|update", "task status <task_id> <PENDING|IN_PROGRESS|COMPLETED|FAILED|CANCELLED> [details=<text>]", "change a task's status"},
    {"show|get", "task show <task_id>", "one task"},
    {"ledger|log|history", "task ledger [customer=<cid>] [task_id=<id>]", "every change made"},
    {"speech-add|voice-add", "task speech-add text=<what to say> [customer=<cid>] [priority=..]", "a task from speech (text spoken, then heard back)"},
    {"speech-update|voice-update", "task speech-update <task_id> text=<what to say>", "update a task from speech"},
    {"speak|tts", "task speak <task_id>", "read a task aloud"},
    {"speak-list|tts-list", "task speak-list", "read the list aloud"},
  }},
  {"horizon", "APPLICATIONS & SHELL", "horizon watch|scan|show|distil|brief", "Watch outside sources for what touches this machine's work", {
    {"watch|add", "horizon watch <name> <url>", "a source to watch"},
    {"unwatch|remove", "horizon unwatch <name>", "stop watching it"},
    {"sources", "horizon sources", "what is watched"},
    {"interests|cares", "horizon interests", "what it measures the sources against"},
    {"scan|look", "horizon scan [keep=6]", "read the sources now"},
    {"show|list", "horizon show [max=20]", "what was found"},
    {"distil|distill", "horizon distil", "have the mind read what was found and write a brief"},
    {"brief", "horizon brief [n]", "the latest briefs"},
  }},
  {"directive", "APPLICATIONS & SHELL", "directive list|add|remove|enable|disable|run", "Commands run on a schedule", {
    {"list|show", "directive list", "standing directives"},
    {"add|give", "directive add <name> <every> as=$e <command…>", "run a command every 30s, 5m, 1h, 1d, or once"},
    {"remove|withdraw", "directive remove <name>", "remove one"},
    {"enable|disable|pause", "directive enable|disable <name>", "resume or pause one"},
    {"run|now", "directive run [name]", "run what is due, or one by name, now"},
  }},
  {"sense", "APPLICATIONS & SHELL", "sense <url>", "Read a page: what it says and where it links; kept in the store", {
  }},
  {"chain", "APPLICATIONS & SHELL", "chain | btc peers|headers|balance|decode|broadcast|rpc", "Bitcoin client (and Ethereum transactions)", {
    {"peers|peer", "chain peers [network]", "connect to peers and show who answered"},
    {"headers", "chain headers [batches=N]", "download and verify headers"},
    {"trace", "chain trace", "the messages one peer sends back after a headers request"},
    {"rpc", "chain rpc <url> <method> [params...]", "a JSON-RPC call"},
    {"mine|hashrate", "chain mine", "this machine's hash rate against the network's target"},
    {"submit", "chain submit <url> <raw transaction hex>", "submit a transaction through a node"},
    {"balance|payout", "chain balance [eth=<rpc-url>] [btc=<explorer-url>]", "the payout addresses' balances"},
    {"eth", "chain eth <rpc-url> to=<addr> [value=<wei>] [submit]", "sign an Ethereum transaction, and send it with submit"},
    {"decode", "chain decode <raw transaction hex>", "decode a transaction"},
    {"broadcast|send", "chain broadcast <raw transaction hex> [network]", "send a transaction to peers"},
  }},
  {"wallet", "APPLICATIONS & SHELL", "wallet status|new|import|send|sync|history", "Bitcoin wallet", {
    {"status|info|balance", "wallet status", "address, balance and outputs"},
    {"new|generate", "wallet new [force=1]", "a new key"},
    {"import", "wallet import <wif> [passphrase=<p>] [force=1]", "import a key"},
    {"export|wif", "wallet export", "the key as WIF"},
    {"node", "wallet node [url]", "the node it syncs from"},
    {"watch", "wallet watch <address>", "watch an address"},
    {"utxo", "wallet utxo <txid> <vout> <sats> <scriptpubkey-hex>", "add an output by hand"},
    {"sync", "wallet sync", "outputs from the node"},
    {"send|transfer", "wallet send <recipient> <amount_sats> [feerate=<sat/vB>] passphrase=<p>", "sign and send"},
    {"history", "wallet history", "transactions it signed"},
    {"release", "wallet release <txid>", "free outputs a transaction reserved"},
  }},
  {"contract", "APPLICATIONS & SHELL", "contract create|solve", "Hash-locked contracts (HTLC)", {
    {"create|htlc", "contract create hash=<sha256 hex> [lock=<height>]", "a hash-locked contract to the wallet's key"},
    {"solve|run|btc|real|watch", "contract solve [watch N] [threads=N] [space=N] [gpu] [network=<net>]", "search for a preimage (local, not on any chain)"},
  }},
  {"tx", "APPLICATIONS & SHELL", "tx plan|stage|commit|abort|apply", "Multi-file patches applied all at once or not at all", {
    {"plan", "tx plan <patch>", "what a patch would change"},
    {"stage", "tx stage <patch>", "stage it"},
    {"review", "tx review", "the staged diff, hunk by hunk"},
    {"show|list|status", "tx show", "what is staged"},
    {"commit", "tx commit", "apply what is staged"},
    {"abort", "tx abort", "drop it"},
    {"apply", "tx apply <patch>", "stage and commit in one step"},
    {"help|--help", "tx help", "the steps"},
  }},
  {"daemon", "APPLICATIONS & SHELL", "daemon list|register|start|stop|status", "Background services", {
    {"list", "daemon list", "services and their state"},
    {"register|define", "daemon register <name> \"<command>\" [after=a,b] [restart=never|on-failure|always] [dir=..] [log=..] [env=K=V] [atboot]", "define a service"},
    {"start", "daemon start <name>", "start it"},
    {"stop", "daemon stop <name>", "stop it"},
    {"restart", "daemon restart <name>", "restart it"},
    {"status", "daemon status <name>", "one service"},
    {"supervise", "daemon supervise [seconds]", "restart what died, for that long"},
    {"forget", "daemon forget <name>", "stop it and remove its definition"},
  }},
  {"build", "APPLICATIONS & SHELL", "build [test|gate|<args...>]", "Build this tree, run the drive tests, or run the gates", {
    {"test", "build test", "scripts/drive.sh"},
    {"gate", "build gate", "harness-smell, zero-host audit and authority gate"},
  }},
  {"git", "APPLICATIONS & SHELL", "git init|clone|add|commit|log|status|push|pull", "Git", {
    {"init", "git init <name|dir>", "a new repository"},
    {"use|cd|open", "git use <name|dir>", "the repository later commands act on"},
    {"repos|list", "git repos", "registered repositories"},
    {"ls-remote|remote-refs", "git ls-remote <url> [key=<path>]", "a remote's refs"},
    {"clone", "git clone <url> [name|dir] [key=<path>] [depth=N] [ref=<name>]", "clone over SSH or HTTPS"},
    {"branch|branches", "git branch [name [at]] | git branch -d <name>", "list, create or delete branches"},
    {"switch|checkout|co", "git switch <branch|commit> [discard=1]", "switch"},
    {"fetch|pull|push", "git fetch|pull|push", "sync with the remote"},
    {"add|stage", "git add <path>", "stage a file"},
    {"commit|ci", "git commit <message...>", "commit"},
    {"log|history", "git log", "history"},
    {"status|st", "git status", "what changed"},
    {"ls-files|index", "git ls-files", "the index"},
    {"cat-file|cat|show", "git cat-file <oid>", "an object"},
    {"fsck|verify", "git fsck", "check the repository"},
    {"help|--help|-h", "git help", "the git commands"},
  }},
  {"ssh", "APPLICATIONS & SHELL", "ssh <user@host> <cmd> | keygen | known", "SSH client", {
    {"keygen|key", "ssh keygen [path]", "a new key pair"},
    {"known|hosts", "ssh known", "hosts this machine has met"},
    {"help|--help|-h", "ssh help", "usage"},
  }},
  {"npm", "APPLICATIONS & SHELL", "npm install|view <package>", "npm packages", {
    {"view|info|versions", "npm view <package>", "a package's versions and metadata"},
    {"install|i|add", "npm install [<package>[@range]] [dir=<path>] [registry=<url>] [deps=0]", "install"},
  }},
  {"pip", "APPLICATIONS & SHELL", "pip install|view <package>", "Python packages, checked against their SHA-256", {
    {"view|info|versions", "pip view <package>", "a package's versions and metadata"},
    {"install|i|add", "pip install <package>[==version] [dir=<path>] [python=3.12] [platform=x86_64] [deps=0]", "install"},
  }},
  {"cargo", "APPLICATIONS & SHELL", "cargo vendor|view <crate>", "Rust crates, checked against their checksums", {
    {"view|info|versions", "cargo view <crate>", "a crate's versions"},
    {"vendor|add|fetch", "cargo vendor <crate>[@version] [dir=<path>] [deps=0]", "download into a directory"},
  }},
  {"go", "APPLICATIONS & SHELL", "go get|list|mod verify <module>", "Go modules, checked against the checksum database", {
    {"list|view|versions", "go list <module>", "a module's versions"},
    {"get|download|vendor", "go get <module>[@version] [dir=<path>] [deps=0] [sums=0]", "download"},
    {"verify", "go mod verify [dir=<path>]", "check downloaded modules"},
  }},
  {"elf", "APPLICATIONS & SHELL", "elf <path>", "An ELF file's headers, and what running it would need", {
  }},
  {"host", "APPLICATIONS & SHELL", "host <command...>", "Run a program on the host OS (or $AIOS_HOST_TOOL)", {
    {"load", "host load", "the same as `load`"},
  }},
  {"install", "APPLICATIONS & SHELL", "install status|dualboot|disk|remove", "Install on bare metal", {
    {"status|info", "install status", "what is installed where"},
    {"dualboot|host|here", "install dualboot", "beside the OS already here"},
    {"disk|target|flash", "install disk <block_device>", "onto a whole disk"},
    {"remove|uninstall", "install remove", "remove it"},
  }},
  {"set", "APPLICATIONS & SHELL", "set NAME", "Keep the last output as $NAME", {
  }},
  {"echo", "APPLICATIONS & SHELL", "echo <text>", "Print text", {
  }},
  {"expect", "APPLICATIONS & SHELL", "expect <text>", "Fail unless the last output contains the text", {
  }},
  {"expect-error", "APPLICATIONS & SHELL", "expect-error <text> <cmd...>", "Fail unless the command fails with that text", {
  }},
  {"help", "APPLICATIONS & SHELL", "help [cmd]", "This list, or one command in full", {
  }},
  };
  return docs;
}

namespace {
const std::pair<const char*, const char*> kAliases[] = {
#include "command_aliases.inc"
};
}  // namespace

const CommandDoc* find_command_doc(const std::string& name) {
  std::string want = name;
  for (const auto& [alias, to] : kAliases)
    if (want == alias) { want = to; break; }
  for (const auto& d : command_docs())
    if (want == d.name) return &d;
  return nullptr;
}

void print_command_help(Out& o, const CommandDoc& d) {
  o << o.style(Cyan()) << d.line << o.style(Reset()) << "\n  " << d.what << "\n";
  std::string aliases;
  for (const auto& [alias, to] : kAliases)
    if (std::strcmp(to, d.name) == 0) aliases += std::string(aliases.empty() ? "" : ", ") + alias;
  if (!aliases.empty()) o << o.style(Dim()) << "  also: " << aliases << o.style(Reset()) << "\n";
  for (const auto& s : d.subs) {
    o << "  " << o.style(Cyan()) << s.usage << o.style(Reset()) << "\n      " << s.what;
    const std::string w = s.words;
    if (w.find('|') != std::string::npos) o << o.style(Dim()) << "  (" << w << ")" << o.style(Reset());
    o << "\n";
  }
}

void print_help(Out& o) {
  o << o.style(Header()) << "AIOS Command Surface" << o.style(Reset()) << "\n";
  std::string section;
  for (const auto& d : command_docs()) {
    if (section != d.section) {
      section = d.section;
      o << "\n" << o.style(Bold()) << section << o.style(Reset()) << "\n";
    }
    // A usage too long for its column goes on a line of its own, the description under it.
    if (std::strlen(d.line) < 40) o << "  " << o.style(Cyan()) << W{40} << d.line << o.style(Reset()) << d.what << "\n";
    else o << "  " << o.style(Cyan()) << d.line << o.style(Reset()) << "\n  " << W{40} << "" << d.what << "\n";
  }
  o << "\n  " << o.style(Cyan()) << W{40} << "pwd, cd, ls, cat, grep ..." << o.style(Reset())
    << "Familiar shell commands (`man` lists them)\n";
  o << "  " << o.style(Cyan()) << W{40} << "quit | exit" << o.style(Reset()) << "End the session\n";
  o << "\n`help <cmd>` shows a command's subcommands.\n";
}

}  // namespace aios::cli
