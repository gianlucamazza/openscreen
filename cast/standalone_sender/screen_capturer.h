// Copyright 2026. BSD-style license.
//
// ScreenCapturer: a minimal Wayland client that continuously captures a
// wl_output using the ext-image-copy-capture-v1 protocol (the standardized
// successor to wlr-screencopy; no xdg-desktop-portal picker required). Captured
// frames are delivered, as raw shm pixels, via a callback on the capture thread.
//
// This is intentionally free of openscreen types so it stays a self-contained,
// reusable capture engine. The consumer converts to I420 and feeds the encoder.

#ifndef CAST_STANDALONE_SENDER_SCREEN_CAPTURER_H_
#define CAST_STANDALONE_SENDER_SCREEN_CAPTURER_H_

#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

struct wl_display;
struct wl_registry;
struct wl_shm;
struct wl_output;
struct wl_buffer;
struct ext_output_image_capture_source_manager_v1;
struct ext_foreign_toplevel_image_capture_source_manager_v1;
struct ext_image_capture_source_v1;
struct ext_image_copy_capture_manager_v1;
struct ext_image_copy_capture_session_v1;
struct ext_image_copy_capture_frame_v1;
struct ext_foreign_toplevel_list_v1;
struct ext_foreign_toplevel_handle_v1;
struct hyprland_toplevel_mapping_manager_v1;
struct hyprland_toplevel_window_mapping_handle_v1;

namespace openscreen::cast {

class ScreenCapturer {
 public:
  // Raw captured frame. `data` points to `format`-encoded pixels valid only for
  // the duration of the callback. `present_nsec` is the compositor presentation
  // time (CLOCK_MONOTONIC), used as the capture timestamp for A/V sync.
  struct Frame {
    const uint8_t* data;
    uint32_t shm_format;  // wl_shm.format value
    int width;
    int height;
    int stride;
    uint64_t present_nsec;
  };
  using FrameCallback = std::function<void(const Frame&)>;

  // What to capture: a whole output (monitor) or a single foreign toplevel
  // (window). Both go through the same ext-image-copy-capture session machinery;
  // only the capture *source* object differs.
  enum class Target { kOutput, kToplevel };

  ScreenCapturer();
  ~ScreenCapturer();

  // Start capturing. For kOutput, `selector` is the output name (e.g. "eDP-1");
  // empty = first output. For kToplevel, the window is chosen by `address` (the
  // Hyprland window address, the reliable key — see hyprland-toplevel-mapping)
  // when non-zero; otherwise `selector` matches by app-id (exact, ci) then title
  // substring (fallback). `on_frame` runs on the internal capture thread for
  // every frame. Returns false on setup failure.
  bool Start(Target target, const std::string& selector, uint64_t address,
             FrameCallback on_frame);
  void Stop();

  int width() const { return buffer_width_; }
  int height() const { return buffer_height_; }

  // Static C-listener trampolines (defined in the .cc). Public so the
  // namespace-scope wl_listener tables can reference them; they forward into
  // the private instance methods below.
  struct Listeners;

 private:
  // wl_registry global handling.
  void OnGlobal(wl_registry* registry, uint32_t name, const char* interface,
                uint32_t version);
  // ext_foreign_toplevel_list / handle events (window-capture mode).
  void OnToplevel(ext_foreign_toplevel_handle_v1* handle);
  void OnToplevelTitle(ext_foreign_toplevel_handle_v1* handle,
                       const char* title);
  void OnToplevelAppId(ext_foreign_toplevel_handle_v1* handle,
                       const char* app_id);
  void OnToplevelClosed(ext_foreign_toplevel_handle_v1* handle);
  // Hyprland window address for a toplevel (via hyprland-toplevel-mapping).
  void OnToplevelAddress(hyprland_toplevel_window_mapping_handle_v1* maph,
                         uint64_t address);
  // Pick the toplevel: by `target_address_` if set, else by `selector_`.
  void SelectToplevel();
  // Session constraint events.
  void OnBufferSize(uint32_t w, uint32_t h);
  void OnShmFormat(uint32_t format);
  void OnSessionDone();
  // Frame lifecycle.
  void CreateAndCaptureFrame();
  void OnFrameReady();
  void OnFramePresentationTime(uint64_t nsec);
  void OnFrameFailed(uint32_t reason);

  bool EnsureBuffer();
  void ThreadMain();

  Target target_ = Target::kOutput;
  std::string selector_;     // output name (kOutput) or app-id/title (kToplevel)
  uint64_t target_address_ = 0;  // Hyprland window address (kToplevel; 0 = unused)

  wl_display* display_ = nullptr;
  wl_registry* registry_ = nullptr;
  wl_shm* shm_ = nullptr;
  wl_output* output_ = nullptr;
  std::string wanted_output_name_;
  std::string current_output_name_;
  uint32_t output_name_id_ = 0;  // wl_registry name for the chosen output

  // Window-capture (kToplevel) state.
  ext_foreign_toplevel_list_v1* toplevel_list_ = nullptr;
  ext_foreign_toplevel_image_capture_source_manager_v1*
      toplevel_source_manager_ = nullptr;
  struct ToplevelInfo {
    ext_foreign_toplevel_handle_v1* handle;
    std::string app_id;
    std::string title;
    hyprland_toplevel_window_mapping_handle_v1* map_handle = nullptr;
    uint64_t address = 0;  // Hyprland window address (0 = not yet resolved)
  };
  std::vector<ToplevelInfo> toplevels_;
  hyprland_toplevel_mapping_manager_v1* toplevel_mapping_manager_ = nullptr;
  // Number of toplevel handles that have received their `done` event (initial
  // app_id/title sent). Enumeration is complete when this equals toplevels_
  // size and the set has stopped growing.
  int toplevels_done_count_ = 0;
  // Number of toplevels whose Hyprland address mapping has resolved (address
  // received or failed). Used to wait for mappings before selecting by address.
  int toplevels_mapped_count_ = 0;
  ext_foreign_toplevel_handle_v1* chosen_toplevel_ = nullptr;

  ext_output_image_capture_source_manager_v1* source_manager_ = nullptr;
  ext_image_capture_source_v1* source_ = nullptr;
  ext_image_copy_capture_manager_v1* capture_manager_ = nullptr;
  ext_image_copy_capture_session_v1* session_ = nullptr;
  ext_image_copy_capture_frame_v1* frame_ = nullptr;

  // Negotiated buffer constraints.
  int buffer_width_ = 0;
  int buffer_height_ = 0;
  uint32_t shm_format_ = 0;
  bool have_format_ = false;
  bool session_ready_ = false;

  // shm buffer (single, reused; capture is serialized one frame at a time).
  wl_buffer* wl_buffer_ = nullptr;
  uint8_t* pool_data_ = nullptr;
  size_t pool_size_ = 0;
  int pool_fd_ = -1;
  int buffer_stride_ = 0;

  uint64_t pending_present_nsec_ = 0;

  FrameCallback on_frame_;
  std::thread thread_;
  bool running_ = false;
};

}  // namespace openscreen::cast

#endif  // CAST_STANDALONE_SENDER_SCREEN_CAPTURER_H_
