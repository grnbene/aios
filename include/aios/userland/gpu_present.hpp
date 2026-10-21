#pragma once
// The display, driven by the GPU.
//
// The compositor is a CPU software rasteriser: it computes every pixel of every
// frame, including the procedural background, and hands the finished buffer to
// whoever owns the screen. That measures 2.6–3.3 ms a frame at 1080p on a
// four-core machine, nearly all of it the background — which is a function of
// coordinates and the clock and therefore exactly what a GPU is for.
//
// This renders the background in a compute shader straight into a swapchain
// image and presents it. There is deliberately NO read-back: at 1080p a frame
// is 7.9 MiB and copying one back over PCIe costs about a millisecond, which is
// most of what moving the work would have saved. The compositor's own layer —
// text, panels, windows — is uploaded only when it changes and blended over the
// background on the GPU.
//
// A machine with no Vulkan says so and the compositor keeps painting on the
// CPU; nothing here is required for the desktop to work.
#include <cstdint>
#include <memory>
#include <vector>
#include <string>
#include "aios/core/result.hpp"

namespace aios::userland {

struct GpuFrameCost {
  double gpu_ms = 0.0;        // what the device reported for the dispatch
  double wall_ms = 0.0;       // acquire, submit, present, as the caller sees it
  uint64_t frames = 0;
  uint64_t overlay_uploads = 0;   // how often the UI layer actually changed
  uint64_t scene_marches = 0;     // FULL-picture raymarches (a scene change, a
                                  // rebuild — not the per-frame partial ones)
  uint64_t scene_pixels_marched = 0;  // raymarched pixels, the real GPU cost
};

class GpuPresenter {
 public:
  GpuPresenter();
  ~GpuPresenter();
  GpuPresenter(const GpuPresenter&) = delete;
  GpuPresenter& operator=(const GpuPresenter&) = delete;

  // `surface` is a VkSurfaceKHR the window system already made (SDL hands one
  // out for a window created with SDL_WINDOW_VULKAN). `instance` is the
  // VkInstance it was made against, because a surface belongs to one.
  Result<void> attach(void* instance, void* surface, uint32_t width, uint32_t height);
  // Creates the instance this presenter will use, with the extensions the
  // window system asks for. Returns null when there is no Vulkan here.
  static Result<void*> make_instance(const char* const* extensions, uint32_t count, std::string& why);

  bool ready() const { return ready_; }
  const std::string& unavailable() const { return why_; }
  const std::string& device_name() const { return device_name_; }
  // Which of the two paths the surface allowed: the shader writing the
  // swapchain image itself, or a render target of ours copied into it. Worth
  // saying out loud — the whole point of the design is the first one, and a
  // caller inspecting frames needs to know which it is looking at.
  bool direct_to_swapchain() const;

  // The display changed size; the swapchain is rebuilt on the next frame.
  void resize(uint32_t width, uint32_t height);

  // One frame. `overlay` is the compositor's own layer in ARGB8888, the same
  // buffer it would have presented, or null for none; it is uploaded only when
  // `overlay_generation` differs from the last frame's, so a still UI over a
  // moving background costs nothing but the dispatch.
  // Hand the shader a scene this machine composed. The floats are the layout
  // SceneDescription::to_floats() produces; scene 4 is what draws them. Kept
  // separate from present() because a description changes when somebody asks
  // for something, not every frame.
  Result<void> set_scene_description(const std::vector<float>& floats);

  // `quality_scale` is how coarsely the BACKGROUND is marched: 1 is the full
  // picture, 2 marches a quarter of the samples and 3 a ninth, and the frame is
  // interpolated back up to the display's resolution. The background is a
  // raymarch and at 1080p it is the entire frame budget, while the interface
  // over it composites at full resolution either way.
  //
  // It is the RESOLUTION that is reduced and never the rate. Both of the
  // cheaper-looking alternatives were built and measured, and both are visibly
  // worse — see the note at the top of the shader for the numbers. Every pixel
  // of a presented frame comes from the same instant, so the background moves
  // cleanly at whatever rate the display runs at.
  Result<void> present(int scene, double seconds, const uint32_t* overlay,
                       uint64_t overlay_generation, int quality_scale = 1);

  // What the device actually produced, as ARGB8888, once. Not part of the frame
  // path — see the note in the implementation: a Vulkan-presented window's
  // pixels are not something a screen grab can be trusted to return, so this is
  // how the picture is inspected honestly.
  Result<void> capture(std::vector<uint32_t>& out);

  // How strongly the scene is drawn. 1.0 is the scene as its own picture; a
  // compositor putting an interface over it wants less, because the interface's
  // own scrims and glows were drawn for a dark background.
  void set_backdrop(float strength) { backdrop_ = strength; }

  // What hour the shoreline is at: 0 midnight, 0.25 dawn, 0.5 midday, 0.75
  // dusk, 1 midnight again. Only scene 0 reads it. Separate from present()
  // for the same reason the composed description is — it changes when somebody
  // asks for a different hour, not every frame.
  void set_time_of_day(float tod) { tod_ = tod < 0.0f ? 0.0f : (tod > 1.0f ? 1.0f : tod); }

  const GpuFrameCost& cost() const { return cost_; }

 // The Vulkan state, named here only so the file that owns it can hand it to
  // its own helpers; nothing outside that file can do anything with it.
  struct Vk;

 private:
  std::unique_ptr<Vk> vk_;
  bool ready_ = false;
  std::string why_;
  std::string device_name_;
  GpuFrameCost cost_;
  float backdrop_ = 1.0f;
  float tod_ = 0.0f;   // the beach at night, which is what it always was
};

}  // namespace aios::userland
