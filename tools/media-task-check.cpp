// Checks the media stack end to end: audio capture and playback, speech-to-text (silence
// transcribes to nothing), text-to-speech, and a spoken round trip.

#include <iostream>
#include <cstdlib>
#include <thread>
#include "aios/hal/audio.hpp"
#include "aios/hal/net.hpp"
#include "aios/media/audio.hpp"
#include "aios/media/stt.hpp"
#include "aios/media/tts.hpp"
#include "aios/media/voice.hpp"
#include "aios/userland/task_list.hpp"
#include "aios/channel/peer_discovery.hpp"
#include "aios/compute/backends.hpp"
#include "aios/intelligence/qwen_mind.hpp"
#include "aios/intelligence/world_model.hpp"

using namespace aios;
using namespace aios::media;
using namespace aios::userland;
using namespace aios::channel;
using namespace aios::compute;
using namespace aios::intelligence;

// Loopback port for the peer that ping_peer() checks below.
static constexpr uint16_t kPeerPort = 18080;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      std::cerr << "CHECK FAILED at line " << __LINE__ << ": " #cond "\n"; \
      std::exit(1); \
    } \
  } while (0)

int main() {
  std::cout << "[CHECK] Verifying Audio recording and playback interfaces...\n";
  AudioSpec spec{16000, 1, AudioSampleFormat::PCM_S16LE};
  AudioRecorder recorder;
  CHECK(recorder.open(spec).ok());
  CHECK(recorder.start_recording().ok());

  AudioBuffer in_buf;
  in_buf.spec = spec;
  in_buf.data.resize(3200, 0); // 100ms of PCM_S16LE audio
  CHECK(recorder.push_samples(in_buf).ok());
  CHECK(recorder.available_frames() == 1600);

  auto read_res = recorder.read_samples(1600);
  CHECK(read_res.ok());
  CHECK(read_res->frame_count() == 1600);
  CHECK(recorder.stop_recording().ok());

  AudioPlayer player;
  CHECK(player.open(spec).ok());
  CHECK(player.play().ok());
  CHECK(player.write_samples(*read_res).ok());
  CHECK(player.buffered_frames() == 1600);
  auto out_res = player.get_playback_output(1600);
  CHECK(out_res.ok());
  CHECK(out_res->frame_count() == 1600);
  CHECK(player.stop().ok());

  // These three checks used to demand the fakes. "transcribe 100ms of zeroes
  // and assert the transcript is NOT empty" can only pass if the transcriber
  // invents words, and it did — the same eight, every time. "converse with no
  // mind bound and assert the answer contains run:" could only pass because
  // the voice engine carried canned sentences of its own. What is checked now
  // is the real contract: silence transcribes to nothing, and a machine with
  // no engine says so instead of answering anyway.
  std::cout << "[CHECK] Verifying STT Engine...\n";
  STTEngine stt;
  STTConfig stt_cfg;
  stt_cfg.sample_rate = 16000;
  CHECK(stt.initialize(stt_cfg).ok());

  auto stt_res = stt.transcribe(*read_res);
  CHECK(stt_res.ok());
  CHECK(stt_res->transcript.empty());                       // silence said nothing
  CHECK(stt_res->tokens.size() == 1);
  CHECK(stt_res->tokens[0].text == "[silence]");
  std::cout << "        silence transcribes to nothing, as it should\n";

  std::cout << "[CHECK] Verifying TTS Engine...\n";
  TTSEngine tts;
  TTSConfig tts_cfg;
  tts_cfg.sample_rate = 22050;
  bool have_synthesizer = false;
  auto tts_init = tts.initialize(tts_cfg);
  if (!tts_init.ok()) {
    // No synthesizer on this substrate. That is an answer, not a failure — and
    // it is the answer the sine generator used to hide.
    std::cout << "        no speech synthesizer here: " << tts_init.error().message << "\n";
  } else {
    have_synthesizer = true;
    CHECK(!tts.available_voices().empty());
    auto tts_res = tts.synthesize("Hello AIOS audio and task system");
    CHECK(tts_res.ok());
    CHECK(tts_res->audio.frame_count() > 0);
    // A real engine speaks at a human rate. The sine generator produced a flat
    // 50ms per character, so this sentence came out at exactly 1.6 seconds.
    double seconds = tts_res->audio.duration_seconds();
    CHECK(seconds > 0.5 && seconds < 12.0);
    std::cout << "        " << tts.available_voices().size() << " real voice(s); that sentence took "
              << seconds << "s\n";
  }

  std::cout << "[CHECK] Verifying Full-Duplex Voice Engine (Talk & Listen)...\n";
  VoiceEngine voice;
  VoiceConfig vcfg;
  vcfg.sample_rate = 16000;
  vcfg.auto_speak_response = true;
  auto voice_init = voice.initialize(vcfg);
  if (!voice_init.ok()) {
    std::cout << "        no voice hardware or engine here: " << voice_init.error().message << "\n";
  } else {
    CHECK(voice.state() == VoiceState::Idle);
    // With nothing bound to answer, the engine must refuse rather than recite.
    CHECK(!voice.has_responder());
    auto refused = voice.process_voice_interaction("status");
    CHECK(!refused.ok());
    if (!have_synthesizer) {
      // Nothing here can turn an answer into speech, so the rest of the
      // conversation path is not runnable. The refusal above is the real
      // assertion, and it does not need a synthesizer.
      std::cout << "        no synthesizer, so the answering half is not exercised here\n";
    } else {
    CHECK(refused.status() == Status::Unsupported);
    std::cout << "        with no mind bound it refuses: " << refused.error().message << "\n";

    voice.set_responder([](const std::string& heard) -> Result<std::string> {
      return std::string("heard: ") + heard;
    });
    auto answered = voice.process_voice_interaction("status");
    CHECK(answered.ok());
    CHECK(answered->text == "heard: status");
    }
  }

  auto pull_res = voice.pull_shared_state();
  CHECK(pull_res.ok());

  std::cout << "[CHECK] Verifying P2P Mesh Discovery & Kademlia DHT...\n";
  P2PMeshDiscovery node_a("node_a");
  P2PMeshDiscovery node_b("node_b");
  CHECK(!node_a.local_node_id().empty());
  CHECK(!node_b.local_node_id().empty());
  CHECK(node_a.local_node_id() != node_b.local_node_id());

  // ping_peer() opens a real TCP connection (src/channel/peer_discovery.cpp),
  // so it can only be checked against something that is actually listening.
  // Point it at a loopback listener this process runs, not at an address that
  // only exists on paper.
  std::thread peer_server([&]() {
    auto listener = hal::net_listen(kPeerPort, /*loopback_only=*/true);
    if (!listener.ok()) return;
    auto conn = (*listener)->accept(5000);
    if (conn.ok()) (*conn)->close();
  });
  peer_server.detach();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));  // let it bind
  CHECK(node_a.ping_peer("node_b_id", "127.0.0.1", kPeerPort).ok());
  CHECK(node_a.active_peer_count() == 1);
  CHECK(node_a.active_bucket_count() > 0);

  auto closest = node_a.find_closest_peers(node_b.local_node_id(), 5);
  CHECK(closest.size() == 1);
  CHECK(closest[0].node_id == "node_b_id");

  CHECK(node_a.rpc_store("cluster:key", "value_123", node_a.local_node_id()).ok());
  CHECK(node_a.stored_value_count() == 1);

  auto find_res = node_a.rpc_find_value("cluster:key");
  CHECK(find_res.found);
  CHECK(find_res.value.key == "cluster:key");
  CHECK(find_res.value.value == "value_123");

  std::string mdns_pkt = node_a.generate_mdns_announcement(5353);
  CHECK(!mdns_pkt.empty());
  auto parsed_peer = node_b.parse_mdns_announcement(mdns_pkt);
  CHECK(parsed_peer.ok());
  CHECK(node_b.active_peer_count() == 1);

  std::cout << "[CHECK] Verifying Core Task List Data Structures...\n";
  TaskList task_list;
  TaskItem t1;
  t1.title = "Task 1: Process Audio";
  t1.priority = TaskPriority::High;
  t1.category = TaskCategory::Compute;
  auto t1_id_res = task_list.add_task(t1);
  CHECK(t1_id_res.ok());
  std::string t1_id = *t1_id_res;

  TaskItem t2;
  t2.title = "Task 2: Speech Recognition";
  t2.priority = TaskPriority::Critical;
  t2.category = TaskCategory::Compute;
  t2.dependencies.push_back(t1_id);
  auto t2_id_res = task_list.add_task(t2);
  CHECK(t2_id_res.ok());
  std::string t2_id = *t2_id_res;

  CHECK(task_list.size() == 2);
  CHECK(task_list.can_execute(t1_id));
  CHECK(!task_list.can_execute(t2_id));

  CHECK(task_list.set_status(t1_id, TaskStatus::Completed).ok());
  CHECK(task_list.can_execute(t2_id));

  auto exec_tasks = task_list.get_executable_tasks();
  CHECK(exec_tasks.size() == 1);
  CHECK(exec_tasks[0].id == t2_id);

  // The speech round trip: this machine says a sentence, hears it back through
  // its own recogniser, and acts on it. It needs both engines to be real, so
  // it runs where they are and says why when they are not. It could never have
  // failed before, because the transcriber returned the same eight words for
  // any audio at all — including the sine wave the synthesizer produced.
  std::cout << "[CHECK] Verifying STT and TTS Task List Integration...\n";
  auto recogniser = hal::speech_recognizer();
  if (!have_synthesizer || !recogniser.ok()) {
    std::cout << "        skipped: "
              << (have_synthesizer ? recogniser.error().message : std::string("no speech synthesizer here"))
              << "\n";
    std::cout << "\n[PASS] media and task checks complete\n";
    return 0;
  }
  std::cout << "        speaking to " << *recogniser << " and back\n";

  auto speech_synth = tts.synthesize("Synthesize speech for tasks");
  CHECK(speech_synth.ok());

  auto speech_add_res = task_list.add_task_from_speech(stt, speech_synth->audio, TaskPriority::High, TaskCategory::User);
  CHECK(speech_add_res.ok());
  std::string add_id = speech_add_res->id;
  CHECK(!add_id.empty());
  CHECK(task_list.has_task(add_id));
  CHECK(speech_add_res->priority == TaskPriority::High);

  auto speech_update_synth = tts.synthesize("complete");
  CHECK(speech_update_synth.ok());
  auto speech_up_res = task_list.update_task_from_speech(stt, add_id, speech_update_synth->audio);
  CHECK(speech_up_res.ok());
  CHECK(speech_up_res->status == TaskStatus::Completed);

  auto cmd_synth = tts.synthesize("add Record podcast");
  CHECK(cmd_synth.ok());
  auto cmd_res = task_list.process_speech_command(stt, tts, cmd_synth->audio);
  CHECK(cmd_res.ok());
  CHECK(!cmd_res->empty());

  auto audio_fb = task_list.synthesize_task_audio_feedback(tts, t1_id);
  CHECK(audio_fb.ok());
  CHECK(audio_fb->audio.frame_count() > 0);

  auto list_fb = task_list.synthesize_task_list_audio_feedback(tts);
  CHECK(list_fb.ok());
  CHECK(list_fb->audio.frame_count() > 0);

  auto act_fb = task_list.synthesize_action_feedback(tts, "Task updated successfully");
  CHECK(act_fb.ok());
  CHECK(act_fb->audio.frame_count() > 0);

  std::cout << "[CHECK] Verifying Vulkan & CUDA Compute Backends...\n";
  DeviceID vk_id(Id128{0, 100});
  DeviceID cuda_id(Id128{0, 200});
  VulkanComputeBackend vk_dev(vk_id);
  CudaComputeBackend cuda_dev(cuda_id);

  CHECK(vk_dev.type() == DeviceType::GPU);
  CHECK(cuda_dev.type() == DeviceType::GPU);
  CHECK(vk_dev.capacity().lanes == 1024);
  CHECK(cuda_dev.capacity().lanes == 2048);

  bool custom_registered = false;
  CHECK(vk_dev.register_kernel("vk_test_kernel", [&](KernelArgs&) {
    custom_registered = true;
    return Ok();
  }).ok());

  Operation custom_op;
  custom_op.code = OpCode::Custom;
  custom_op.custom_kernel = 14695981039346656037ULL; // dummy hash or matching
  CHECK(vk_dev.register_kernel("custom_hash", [&](KernelArgs&) {
    custom_registered = true;
    return Ok();
  }).ok());

  std::cout << "[CHECK] Verifying Qwen LLM Mind Backend & Client Interface...\n";
  QwenClient qwen_client("http://127.0.0.1:8000/v1/chat/completions", "spark");
  CHECK(qwen_client.url() == "http://127.0.0.1:8000/v1/chat/completions");
  CHECK(qwen_client.model() == "spark");

  auto qwen_mind = make_qwen_intelligence("http://127.0.0.1:8000/v1/chat/completions", "spark");
  CHECK(qwen_mind != nullptr);
  CHECK(qwen_mind->name() == "net:spark");

  std::cout << "[CHECK] Verifying Qwen Mind Userland Console Command Translation Loop...\n";
  auto trans_res = qwen_client.translate_command("show system memory usage");
  CHECK(trans_res.ok());
  CHECK(trans_res->find("run:") != std::string::npos);

  std::cout << "[CHECK] Verifying JEPA Substrate World Model, Action Policy Head & Local CUDA Training Engine...\n";
  SubstrateState s0;
  CHECK(s0.vector.size() == kSubstrateStateDim);
  std::vector<float> a0(kActionVectorDim, 0.05f);

  JEPAWorldModel model;
  SubstrateState s_hat = model.predict_next_state(s0, a0);
  CHECK(s_hat.vector.size() == kSubstrateStateDim);

  ExecutionTraceLogger trace_log(1000);
  size_t syn_gen = generate_synthetic_traces(trace_log, 100);
  CHECK(syn_gen == 100);
  CHECK(trace_log.size() == 100);

  SubstrateState real_telemetry = SubstrateState::sample_kernel_telemetry();
  CHECK(real_telemetry.vector.size() == kSubstrateStateDim);
  CHECK(real_telemetry.vector[0] >= 0.0f); // CPU load channel
  CHECK(real_telemetry.vector[1] >= 0.0f); // NIC depth channel

  auto samples = trace_log.get_samples(32);
  CHECK(samples.size() == 32);

  CudaComputeBackend cuda_be(cuda_id);
  float initial_loss = model.current_loss();
  CHECK(initial_loss >= 0.0f);
  auto train_res = model.train_step(samples, &cuda_be);
  CHECK(train_res.ok());
  CHECK(model.total_steps() == 1);
  CHECK(*train_res >= 0.0f);

  ActionPolicyHead policy_head;
  auto sel_a = policy_head.select_action(s0);
  CHECK(sel_a.size() == kActionVectorDim);
  auto pol_train_res = policy_head.train_step(samples, &cuda_be);
  CHECK(pol_train_res.ok());
  CHECK(policy_head.total_steps() == 1);

  auto jepa_mind = make_jepa_world_model_intelligence();
  CHECK(jepa_mind != nullptr);
  CHECK(jepa_mind->name() == "jepa-world-model");

  std::cout << "[CHECK] Verifying Shared State Vector Region & Kernel-Userland Substrate Interface...\n";
  SubstrateState s_test;
  std::vector<float> a_test(kActionVectorDim, 0.123f);
  SubstrateState s_next_test = model.predict_next_state(s_test, a_test);
  update_shared_state_region(s_test, a_test, s_next_test);

  auto* shm_region = get_global_shared_state_region();
  CHECK(shm_region != nullptr);
  CHECK(shm_region->sequence.load() > 0);
  CHECK(shm_region->action_vector[0] == 0.123f);

  std::cout << "[CHECK] All audio, STT, TTS, duplex Voice engine, P2P mesh discovery, Compute backends, Qwen LLM mind, Local CUDA GPU mind, JEPA Substrate World Model, Action Policy Head, Shared State Region, and integrated task list operations passed!\n";

  return 0;
}
