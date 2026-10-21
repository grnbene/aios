// Drives the console's subsystems on a real kernel root: in-memory conversation search, zero-
// polling timeline and state sync, and local command intent parsing, with timings.

#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <cmath>
#include <cassert>
#include <cstring>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <unistd.h>

#include "aios/kernel/kernel.hpp"
#include "aios/storage/object_store.hpp"
#include "aios/storage/aql.hpp"
#include "aios/sidecar/shm_abi.hpp"
#include "aios/sidecar/event_fabric.hpp"
#include "aios/state/state.hpp"
#include "aios/intelligence/language_model.hpp"
#include "aios/userland/web.hpp"
#include "aios/userland/glyph_engine.hpp"
#include "aios/channel/peer_discovery.hpp"

using namespace aios;
using namespace aios::storage;
using namespace aios::sidecar;
using namespace aios::intelligence;
using namespace aios::userland::web;
using namespace aios::channel;

static uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

int main() {
    std::cout << "======================================================================\n";
    std::cout << "    AIOS 5-SUBSYSTEM ASSAY CONSOLE INTEGRATION BENCHMARK & PROOF      \n";
    std::cout << "   REAL HARDWARE EXECUTION - ZERO MOCK DATA - FULLY GROUNDED PROOF    \n";
    std::cout << "======================================================================\n\n";

    int checks_passed = 0;
    int checks_failed = 0;
    int total_checks = 0;

    // Same bug this file's sibling untapped-engines-bench.cpp had: pass_check
    // had no failure branch, so "SUMMARY: N/N CHECKS EXECUTED, 0 FAILURES"
    // was the only possible printout short of an assert aborting the whole
    // process with no report at all. check() takes the real condition.
    auto check = [&](bool ok, const std::string& name, const std::string& detail) {
        total_checks++;
        if (ok) {
            checks_passed++;
            std::cout << "  [PASS] " << name << ": " << detail << "\n";
        } else {
            checks_failed++;
            std::cout << "  [FAIL] " << name << ": " << detail << "\n";
        }
    };

    // ──────────────────────────────────────────────────────────────────────────
    // [1] AIOS-CONSOLE-01: In-Memory Semantic Conversation & Transcript Search
    // ──────────────────────────────────────────────────────────────────────────
    std::cout << "─── [1] AIOS-CONSOLE-01: In-Memory Conversation & Turn AQL Search ────\n";
    {
        char root_dir[] = "/tmp/aios-console-aql-XXXXXX";
        assert(mkdtemp(root_dir) != nullptr);

        KernelConfig kcfg;
        kcfg.root = root_dir;
        auto k_res = Kernel::boot(kcfg);
        assert(k_res.ok());
        Kernel& k = **k_res;
        Surface surf = k.as(k.self());

        const int num_turns = 1000;
        auto t0 = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < num_turns; ++i) {
            StoredObject turn;
            turn.metadata.emplace_back("kind", "conversation.turn");
            turn.metadata.emplace_back("role", (i % 2 == 0) ? "user" : "assistant");
            turn.metadata.emplace_back("conv_id", "conv_" + std::to_string(i % 20));
            turn.metadata.emplace_back("step", std::to_string(i));
            
            std::string text;
            if (i % 10 == 0) {
                turn.metadata.emplace_back("topic", "deployment_chi");
                text = "Dispatched rolling deployment to chi-il-1 on port 7311 for ASQL daemon.";
                turn.embedding = text_embedding("deployment on chi-il-1 port 7311");
            } else if (i % 10 == 1) {
                turn.metadata.emplace_back("topic", "kernel_memory");
                text = "Unified MemoryFabric allocated 64MB buffer in shared memory ring.";
                turn.embedding = text_embedding("memory fabric allocation shared memory");
            } else {
                turn.metadata.emplace_back("topic", "general_chat");
                text = "Conversation step " + std::to_string(i) + " executed normal tool interactions.";
                turn.embedding = text_embedding("conversation step tool execution normal");
            }
            turn.payload = ByteBuffer(text.begin(), text.end());
            auto store_res = surf.store(turn);
            assert(store_res.ok());
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double ingest_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        // The loop above already hard-asserts each store_res.ok(), which is a
        // legitimate setup precondition (nothing downstream is meaningful if
        // ingest didn't happen) -- ingest completing at all is the real claim.
        check(true, "1,000 Conversation Turns Ingested into In-Memory ObjectStore",
              std::to_string(ingest_ms) + " ms (" + std::to_string(static_cast<int>(num_turns / (ingest_ms / 1000.0))) + " turns/sec)");

        // Execute Hybrid AQL Search
        std::string query_str = "SELECT * FROM objects WHERE meta.role = \"user\" NEAR embedding_of(\"deployment on chi-il-1 port 7311\") LIMIT 5";
        auto t_q0 = std::chrono::high_resolution_clock::now();
        auto query_res = execute_aql(surf, query_str);
        auto t_q1 = std::chrono::high_resolution_clock::now();
        double search_us = std::chrono::duration<double, std::micro>(t_q1 - t_q0).count();

        bool all_user_role = true;
        for (const auto& o : query_res.objects) {
            if (o.meta("role") != "user") { all_user_role = false; break; }
        }
        bool search_ok = query_res.success && !query_res.objects.empty() && all_user_role;

        check(search_ok, "Hybrid AQL Turn Search (Metadata + Semantic Vector)",
              search_ok ? std::to_string(search_us) + " µs (Matched " + std::to_string(query_res.objects.size()) + " turns in RAM)"
                        : "success=" + std::to_string(query_res.success) + " matches=" + std::to_string(query_res.objects.size()) +
                              " all_role_user=" + std::to_string(all_user_role));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // [2] AIOS-CONSOLE-02: Zero-Polling Live Timeline & State Generation Sync
    // ──────────────────────────────────────────────────────────────────────────
    std::cout << "\n─── [2] AIOS-CONSOLE-02: Zero-Polling Timeline & State Generation Sync ──\n";
    {
        char root_dir[] = "/tmp/aios-console-state-XXXXXX";
        assert(mkdtemp(root_dir) != nullptr);

        KernelConfig kcfg;
        kcfg.root = root_dir;
        auto k_res = Kernel::boot(kcfg);
        assert(k_res.ok());
        Kernel& k = **k_res;

        EntityID owner = *k.spawn("console_worker");
        EventFabric fabric;
        uint32_t topic_id = fabric.register_topic("timeline.stream", 4);
        assert(topic_id > 0);

        auto state_res = k.states().create(owner, StateKind::Working, now_ns(), ByteSpan{reinterpret_cast<const uint8_t*>("INIT"), 4});
        assert(state_res.ok());
        StateID sid = *state_res;

        const int num_events = 20000;
        bool all_sets_ok = true, all_publishes_ok = true;
        auto t0 = std::chrono::high_resolution_clock::now();
        for (uint64_t gen = 1; gen <= num_events; ++gen) {
            std::string payload = "{\"step\":" + std::to_string(gen) + ",\"status\":\"running\",\"tool\":\"bash\"}";
            auto set_res = k.states().set_payload(sid, ByteSpan{reinterpret_cast<const uint8_t*>(payload.data()), payload.size()}, now_ns());
            if (!set_res.ok()) all_sets_ok = false;
            uint64_t seq = fabric.publish("timeline.stream", payload.data(), payload.size(), static_cast<uint32_t>(gen));
            if (seq == 0) all_publishes_ok = false;
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        double ops_sec = num_events / (elapsed_ms / 1000.0);

        const StateObject* s_final = k.states().get(sid);
        // The event-ring publish() calls above ARE the only thing exercising
        // ingestion into the topic ring -- there was previously a SEPARATE
        // "Lock-Free Topic Event Ring Ingestion" pass_check claiming "zero
        // syscalls, live subscriber push" with nothing behind it: no syscall
        // count taken, no subscriber ever registered to confirm a push
        // reached it. Folded into the one claim this file actually tests
        // (every publish() returned a non-zero sequence number) rather than
        // inventing a second, separately-verified capability that wasn't.
        bool state_stream_ok = all_sets_ok && all_publishes_ok &&
            s_final != nullptr && s_final->generation == (uint64_t)num_events;

        check(state_stream_ok, "Monotonic State Generation Stream + Topic Ring Publish (20,000 updates)",
              state_stream_ok
                  ? std::to_string(ops_sec) + " ops/sec (" + std::to_string(elapsed_ms / num_events * 1000000.0) + " ns/update)"
                  : "sets_ok=" + std::to_string(all_sets_ok) + " publishes_ok=" + std::to_string(all_publishes_ok) +
                        " final_generation=" + (s_final ? std::to_string(s_final->generation) : "null"));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // [3] AIOS-CONSOLE-03: Local Intent & Command Parsing Without Cloud LLM
    // ──────────────────────────────────────────────────────────────────────────
    std::cout << "\n─── [3] AIOS-CONSOLE-03: Local Command Intent Runtime ─────────────────\n";
    {
        // encode()/decode() read the merge table and vocabulary train() builds;
        // called untrained (as this was), vocab_ is empty and decode() has
        // nothing to map ids back to bytes with -- it isn't that BPE roundtrip
        // is broken, it's that nothing here ever trained the tokenizer before
        // asking it to decode.
        ByteBpeTokenizer tokenizer;
        std::vector<std::string> corpus = {
            "/restart runner on chi-il-1 --force", "/restart runner on devops",
            "check memory and run garbage collection", "deploy latest commit to all nodes",
            "/stop runner on chi-il-1", "check status of chi-il-1", "/restart runner"
        };
        tokenizer.train(corpus, 512);

        std::string sample_cmd = "/restart runner on chi-il-1 --force";
        auto tokens = tokenizer.encode(sample_cmd);
        auto decoded = tokenizer.decode(tokens);
        bool tokenizer_ok = tokenizer.trained() && decoded == sample_cmd;

        check(tokenizer_ok, "Byte-BPE Tokenizer Command Roundtrip (trained on a small corpus first)",
              tokenizer_ok ? std::to_string(tokens.size()) + " tokens, vocab=" + std::to_string(tokenizer.vocab_size()) +
                                  ", merges=" + std::to_string(tokenizer.merge_count()) + ", for '" + sample_cmd + "'"
                            : "trained=" + std::to_string(tokenizer.trained()) + " decoded='" + decoded + "' expected='" + sample_cmd + "'");

        auto t0 = std::chrono::high_resolution_clock::now();
        auto reply1 = translate_request("restart runner on chi-il-1");
        auto reply2 = translate_request("check memory and run garbage collection");
        auto reply3 = translate_request("deploy latest commit to all nodes");
        auto t1 = std::chrono::high_resolution_clock::now();
        double intent_us = std::chrono::duration<double, std::micro>(t1 - t0).count() / 3.0;

        bool intent_ok = !reply1.command.empty() && !reply2.command.empty() && !reply3.command.empty();

        check(intent_ok, "Local Intent Translation & Action Mapping",
              intent_ok ? std::to_string(intent_us) + " µs/command (Zero external LLM API cost)"
                        : "commands: '" + reply1.command + "' '" + reply2.command + "' '" + reply3.command + "'");
    }

    // ──────────────────────────────────────────────────────────────────────────
    // [4] AIOS-CONSOLE-04: Instant One-Click PDF Transcript & Report Export
    // ──────────────────────────────────────────────────────────────────────────
    std::cout << "\n─── [4] AIOS-CONSOLE-04: In-Memory Vector PDF Layout Engine ──────────\n";
    {
        std::string transcript_html = 
            "<html><body>"
            "<h1>Assay Mission Execution Transcript</h1>"
            "<p><b>Mission ID:</b> mission_902628 | <b>Node:</b> chi-il-1</p>"
            "<hr/>"
            "<div><b>[Turn 1 - Operator]</b> Check in-memory ASQL status on chi-il-1.</div>"
            "<div><b>[Turn 2 - Assistant]</b> Ran query SELECT COUNT(*) FROM salesorders. Result: 347,860 rows.</div>"
            "<div><b>[Turn 3 - Tool]</b> Bash execution completed in 10.76 ms with exit code 0.</div>"
            "</body></html>";

        auto t0 = std::chrono::high_resolution_clock::now();
        
        auto dom = parse_html(aios::ByteSpan{reinterpret_cast<const uint8_t*>(transcript_html.data()), transcript_html.size()});
        auto author_css = parse_css(R"(
            body { font-size: 14px; margin: 20px; }
            h1 { font-size: 20px; font-weight: bold; margin-bottom: 10px; }
            div { margin: 6px 0; }
        )");
        
        Metrics metrics;
        metrics.measure = [](const std::string& text, const FontKey& f) {
            return (int)(text.size() * (f.px * 0.55f));
        };
        metrics.line_height = [](const FontKey& f) {
            return (int)(f.px * 1.35f);
        };
        metrics.ascent = [](const FontKey& f) {
            return (int)(f.px * 1.1f);
        };

        Document doc;
        doc.dom = std::move(dom);
        layout(doc, author_css, metrics, 800, 1100);

        // Emit vector PDF 1.4 binary stream
        std::ostringstream pdf_stream;
        pdf_stream << "%PDF-1.4\n";
        pdf_stream << "1 0 obj << /Type /Catalog /Pages 2 0 R >> endobj\n";
        pdf_stream << "2 0 obj << /Type /Pages /Kids [3 0 R] /Count 1 >> endobj\n";
        pdf_stream << "3 0 obj << /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 4 0 R >> endobj\n";
        
        std::string stream_content = "BT /F1 12 Tf 50 792 Td (Assay Mission Execution Transcript - chi-il-1) Tj ET\n";
        pdf_stream << "4 0 obj << /Length " << stream_content.length() << " >> stream\n";
        pdf_stream << stream_content << "\nendstream\nendobj\n";
        pdf_stream << "xref\n0 5\n0000000000 65535 f \n";
        pdf_stream << "trailer << /Size 5 /Root 1 0 R >>\nstartxref\n300\n%%EOF\n";
        
        auto t1 = std::chrono::high_resolution_clock::now();
        double render_us = std::chrono::duration<double, std::micro>(t1 - t0).count();

        // The name claimed "Transcript HTML -> PDF": the PDF stream below is
        // a fixed template with one hardcoded line of text, built without
        // ever reading doc/doc.fragments -- the real HTML->layout pipeline
        // above (parse_html/parse_css/layout) runs and IS genuinely
        // exercised, but its result is discarded except to report a count.
        // The %%EOF check was on a literal this same block had just written
        // two lines above, which can't fail short of the compiler being
        // broken. Testing what's actually true: the layout pipeline produced
        // real fragments, and the byte stream this block hand-assembles is
        // well-formed.
        bool layout_ok = !doc.fragments.empty();
        bool pdf_wellformed = pdf_stream.str().rfind("%PDF-1.4", 0) == 0 &&
            pdf_stream.str().find("%%EOF") != std::string::npos;

        check(layout_ok && pdf_wellformed, "HTML Layout Pipeline + Hand-Assembled PDF Stream (not HTML-driven PDF content)",
              (layout_ok && pdf_wellformed)
                  ? std::to_string(render_us) + " µs (" + std::to_string(pdf_stream.str().size()) + " bytes, " +
                        std::to_string(doc.fragments.size()) + " box fragments from real layout)"
                  : "fragments=" + std::to_string(doc.fragments.size()) + " pdf_wellformed=" + std::to_string(pdf_wellformed));
    }

    // ──────────────────────────────────────────────────────────────────────────
    // [5] AIOS-CONSOLE-05: Dynamic Fleet Resource Resolution & P2P Routing
    // ──────────────────────────────────────────────────────────────────────────
    std::cout << "\n─── [5] AIOS-CONSOLE-05: Dynamic Fleet Resource Resolution (P2P DHT) ──\n";
    {
        P2PMeshDiscovery node_arch("arch-console-seed");
        P2PMeshDiscovery node_chi("chi-il-1-seed");
        P2PMeshDiscovery node_devops("devops-runner-seed");
        P2PMeshDiscovery node_rotors("rotors-gpu-seed");

        // Register Fleet Resources. The real signature is
        // announce_resource(uri, metadata, capacity, ip, port) -- this used
        // to call it as (uri, ip, port, metadata, capacity), which compiles
        // (every slot is string/int-compatible) but silently swaps every
        // field: provider_ip ended up holding the metadata text, capacity
        // held the port number, and so on. find_resource() below still found
        // each entry by its URI, so "found" was never the problem; every IP
        // comparison against the real address was comparing against the
        // wrong field and failing, which the old unconditional pass_check
        // never surfaced.
        node_chi.announce_resource("Compute/ASQL-ERP", "347k salesorders resident", 100, "203.0.113.10", 7311);
        node_devops.announce_resource("Workspace/AssayRunner", "system unit runner", 50, "203.0.113.10", 4317);
        node_rotors.announce_resource("Model/gemma-2b", "weights cached in VRAM", 2048, "192.168.1.105", 7312);

        // Store announcements in Arch node catalog
        auto chi_res = node_chi.list_resources_by_prefix("");
        for (const auto& r : chi_res) node_arch.store_resource(r);
        auto dev_res = node_devops.list_resources_by_prefix("");
        for (const auto& r : dev_res) node_arch.store_resource(r);
        auto rot_res = node_rotors.list_resources_by_prefix("");
        for (const auto& r : rot_res) node_arch.store_resource(r);

        auto t0 = std::chrono::high_resolution_clock::now();
        auto res1 = node_arch.find_resource("Compute/ASQL-ERP");
        auto res2 = node_arch.find_resource("Workspace/AssayRunner");
        auto res3 = node_arch.find_resource("Model/gemma-2b");
        auto t1 = std::chrono::high_resolution_clock::now();

        double resolve_us = std::chrono::duration<double, std::micro>(t1 - t0).count() / 3.0;

        bool p2p_ok = res1.found && res1.resource.provider_ip == "203.0.113.10" &&
            res2.found && res2.resource.provider_ip == "203.0.113.10" &&
            res3.found && res3.resource.provider_ip == "192.168.1.105";

        check(p2p_ok, "P2P 160-bit XOR Resource Resolution (3 lookups)",
              p2p_ok ? std::to_string(resolve_us) + " µs/lookup (Target nodes: chi-il-1, devops, rotors)"
                     : "found: " + std::to_string(res1.found) + "/" + std::to_string(res2.found) + "/" + std::to_string(res3.found));
    }

    std::cout << "\n======================================================================\n";
    if (checks_failed == 0) {
        std::cout << "  SUMMARY: " << checks_passed << "/" << total_checks << " CHECKS EXECUTED, 0 FAILURES.\n";
        std::cout << "  ASSAY CONSOLE AIOS INTEGRATION: OPERATIONAL.\n";
    } else {
        std::cout << "  SUMMARY: " << checks_passed << "/" << total_checks << " PASSED, " << checks_failed << " FAILED.\n";
    }
    std::cout << "======================================================================\n";

    return checks_failed == 0 ? 0 : 1;
}
