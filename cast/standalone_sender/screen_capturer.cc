// Copyright 2026. BSD-style license.

#include "cast/standalone_sender/screen_capturer.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include <cctype>
#include <cstring>

#include "ext-foreign-toplevel-list-v1-client.h"
#include "ext-image-capture-source-v1-client.h"
#include "ext-image-copy-capture-v1-client.h"
#include "hyprland-toplevel-mapping-v1-client.h"
#include "util/osp_logging.h"

namespace openscreen::cast {

// ---- C listener trampolines -------------------------------------------------

namespace {
ScreenCapturer* Self(void* data) {
  return static_cast<ScreenCapturer*>(data);
}
}  // namespace

struct ScreenCapturer::Listeners {
  // wl_registry
  static void Global(void* data, wl_registry* reg, uint32_t name,
                     const char* iface, uint32_t version) {
    Self(data)->OnGlobal(reg, name, iface, version);
  }
  static void GlobalRemove(void*, wl_registry*, uint32_t) {}

  // wl_output (to learn the output's name)
  static void OutGeometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t,
                          int32_t, const char*, const char*, int32_t) {}
  static void OutMode(void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {}
  static void OutDone(void*, wl_output*) {}
  static void OutScale(void*, wl_output*, int32_t) {}
  static void OutName(void* data, wl_output*, const char* name) {
    Self(data)->current_output_name_ = name ? name : "";
  }
  static void OutDescription(void*, wl_output*, const char*) {}

  // ext_foreign_toplevel_list_v1
  static void TlToplevel(void* data, ext_foreign_toplevel_list_v1*,
                         ext_foreign_toplevel_handle_v1* toplevel) {
    Self(data)->OnToplevel(toplevel);
  }
  static void TlFinished(void*, ext_foreign_toplevel_list_v1*) {}

  // ext_foreign_toplevel_handle_v1
  static void TlClosed(void* data, ext_foreign_toplevel_handle_v1* h) {
    Self(data)->OnToplevelClosed(h);
  }
  static void TlDone(void* data, ext_foreign_toplevel_handle_v1*) {
    // Initial app_id/title for this handle have now been delivered.
    Self(data)->toplevels_done_count_++;
  }
  static void TlTitle(void* data, ext_foreign_toplevel_handle_v1* h,
                      const char* title) {
    Self(data)->OnToplevelTitle(h, title);
  }
  static void TlAppId(void* data, ext_foreign_toplevel_handle_v1* h,
                      const char* app_id) {
    Self(data)->OnToplevelAppId(h, app_id);
  }
  static void TlIdentifier(void*, ext_foreign_toplevel_handle_v1*,
                           const char*) {}

  // hyprland_toplevel_window_mapping_handle_v1
  static void MapAddress(void* data,
                         hyprland_toplevel_window_mapping_handle_v1* maph,
                         uint32_t address_hi, uint32_t address) {
    const uint64_t addr =
        (static_cast<uint64_t>(address_hi) << 32) | address;
    Self(data)->OnToplevelAddress(maph, addr);
  }
  static void MapFailed(void* data,
                        hyprland_toplevel_window_mapping_handle_v1* maph) {
    Self(data)->OnToplevelAddress(maph, 0);  // resolved-as-failed
  }

  // ext_image_copy_capture_session_v1
  static void SesBufferSize(void* data, ext_image_copy_capture_session_v1*,
                            uint32_t w, uint32_t h) {
    Self(data)->OnBufferSize(w, h);
  }
  static void SesShmFormat(void* data, ext_image_copy_capture_session_v1*,
                           uint32_t fmt) {
    Self(data)->OnShmFormat(fmt);
  }
  static void SesDmabufDevice(void*, ext_image_copy_capture_session_v1*,
                              wl_array*) {}
  static void SesDmabufFormat(void*, ext_image_copy_capture_session_v1*,
                              uint32_t, wl_array*) {}
  static void SesDone(void* data, ext_image_copy_capture_session_v1*) {
    Self(data)->OnSessionDone();
  }
  static void SesStopped(void*, ext_image_copy_capture_session_v1*) {}

  // ext_image_copy_capture_frame_v1
  static void FrTransform(void*, ext_image_copy_capture_frame_v1*, uint32_t) {}
  static void FrDamage(void*, ext_image_copy_capture_frame_v1*, int32_t, int32_t,
                       int32_t, int32_t) {}
  static void FrPresentationTime(void* data, ext_image_copy_capture_frame_v1*,
                                 uint32_t sec_hi, uint32_t sec_lo,
                                 uint32_t nsec) {
    const uint64_t sec = (static_cast<uint64_t>(sec_hi) << 32) | sec_lo;
    Self(data)->OnFramePresentationTime(sec * 1000000000ull + nsec);
  }
  static void FrReady(void* data, ext_image_copy_capture_frame_v1*) {
    Self(data)->OnFrameReady();
  }
  static void FrFailed(void* data, ext_image_copy_capture_frame_v1*,
                       uint32_t reason) {
    Self(data)->OnFrameFailed(reason);
  }
};

namespace {
const wl_registry_listener kRegistryListener = {
    &ScreenCapturer::Listeners::Global,
    &ScreenCapturer::Listeners::GlobalRemove};

const wl_output_listener kOutputListener = {
    &ScreenCapturer::Listeners::OutGeometry,
    &ScreenCapturer::Listeners::OutMode,
    &ScreenCapturer::Listeners::OutDone,
    &ScreenCapturer::Listeners::OutScale,
    &ScreenCapturer::Listeners::OutName,
    &ScreenCapturer::Listeners::OutDescription};

const ext_foreign_toplevel_list_v1_listener kToplevelListListener = {
    &ScreenCapturer::Listeners::TlToplevel,
    &ScreenCapturer::Listeners::TlFinished};

const ext_foreign_toplevel_handle_v1_listener kToplevelHandleListener = {
    &ScreenCapturer::Listeners::TlClosed,
    &ScreenCapturer::Listeners::TlDone,
    &ScreenCapturer::Listeners::TlTitle,
    &ScreenCapturer::Listeners::TlAppId,
    &ScreenCapturer::Listeners::TlIdentifier};

const hyprland_toplevel_window_mapping_handle_v1_listener kMappingListener = {
    &ScreenCapturer::Listeners::MapAddress,
    &ScreenCapturer::Listeners::MapFailed};

const ext_image_copy_capture_session_v1_listener kSessionListener = {
    &ScreenCapturer::Listeners::SesBufferSize,
    &ScreenCapturer::Listeners::SesShmFormat,
    &ScreenCapturer::Listeners::SesDmabufDevice,
    &ScreenCapturer::Listeners::SesDmabufFormat,
    &ScreenCapturer::Listeners::SesDone,
    &ScreenCapturer::Listeners::SesStopped};

const ext_image_copy_capture_frame_v1_listener kFrameListener = {
    &ScreenCapturer::Listeners::FrTransform,
    &ScreenCapturer::Listeners::FrDamage,
    &ScreenCapturer::Listeners::FrPresentationTime,
    &ScreenCapturer::Listeners::FrReady,
    &ScreenCapturer::Listeners::FrFailed};
}  // namespace

// ---- ScreenCapturer ---------------------------------------------------------

ScreenCapturer::ScreenCapturer() = default;

ScreenCapturer::~ScreenCapturer() {
  Stop();
}

bool ScreenCapturer::Start(Target target, const std::string& selector,
                           uint64_t address, FrameCallback on_frame) {
  target_ = target;
  selector_ = selector;
  target_address_ = address;
  wanted_output_name_ = (target == Target::kOutput) ? selector : "";
  on_frame_ = std::move(on_frame);
  running_ = true;
  thread_ = std::thread([this] { ThreadMain(); });
  return true;
}

void ScreenCapturer::Stop() {
  // Just signal the capture thread; it polls `running_` with a short timeout
  // and exits on its own. Never touch wl_display from this thread.
  running_ = false;
  if (thread_.joinable()) {
    thread_.join();
  }
}

void ScreenCapturer::OnGlobal(wl_registry* registry, uint32_t name,
                              const char* interface, uint32_t version) {
  const std::string iface = interface;
  if (iface == wl_shm_interface.name) {
    shm_ = static_cast<wl_shm*>(
        wl_registry_bind(registry, name, &wl_shm_interface, 1));
  } else if (iface == ext_output_image_capture_source_manager_v1_interface.name) {
    source_manager_ = static_cast<ext_output_image_capture_source_manager_v1*>(
        wl_registry_bind(registry, name,
                         &ext_output_image_capture_source_manager_v1_interface,
                         1));
  } else if (iface ==
             ext_foreign_toplevel_image_capture_source_manager_v1_interface
                 .name) {
    toplevel_source_manager_ =
        static_cast<ext_foreign_toplevel_image_capture_source_manager_v1*>(
            wl_registry_bind(
                registry, name,
                &ext_foreign_toplevel_image_capture_source_manager_v1_interface,
                1));
  } else if (iface == ext_image_copy_capture_manager_v1_interface.name) {
    capture_manager_ = static_cast<ext_image_copy_capture_manager_v1*>(
        wl_registry_bind(registry, name,
                         &ext_image_copy_capture_manager_v1_interface, 1));
  } else if (iface == ext_foreign_toplevel_list_v1_interface.name) {
    // Only enumerate toplevels when capturing a window; the list keeps the
    // compositor sending toplevel events otherwise.
    if (target_ == Target::kToplevel) {
      const uint32_t bind_ver = version < 1 ? version : 1;
      toplevel_list_ = static_cast<ext_foreign_toplevel_list_v1*>(
          wl_registry_bind(registry, name,
                           &ext_foreign_toplevel_list_v1_interface, bind_ver));
      ext_foreign_toplevel_list_v1_add_listener(toplevel_list_,
                                                &kToplevelListListener, this);
    }
  } else if (iface == hyprland_toplevel_mapping_manager_v1_interface.name) {
    // Hyprland-only: maps a toplevel handle to its window address (the reliable
    // selection key, shared with `hyprctl clients`).
    if (target_ == Target::kToplevel) {
      toplevel_mapping_manager_ =
          static_cast<hyprland_toplevel_mapping_manager_v1*>(wl_registry_bind(
              registry, name, &hyprland_toplevel_mapping_manager_v1_interface,
              1));
    }
  } else if (iface == wl_output_interface.name) {
    // Bind every output (v4 for the name event); keep the one that matches.
    const uint32_t bind_ver = version < 4 ? version : 4;
    wl_output* out = static_cast<wl_output*>(
        wl_registry_bind(registry, name, &wl_output_interface, bind_ver));
    // Temporarily track candidate; selection happens after names arrive.
    if (!output_ ) {
      output_ = out;
      output_name_id_ = name;
    }
    wl_output_add_listener(out, &kOutputListener, this);
  }
}

void ScreenCapturer::OnToplevel(ext_foreign_toplevel_handle_v1* handle) {
  toplevels_.push_back(ToplevelInfo{handle, "", ""});
  ext_foreign_toplevel_handle_v1_add_listener(handle, &kToplevelHandleListener,
                                              this);
}

void ScreenCapturer::OnToplevelTitle(ext_foreign_toplevel_handle_v1* handle,
                                     const char* title) {
  for (auto& t : toplevels_) {
    if (t.handle == handle) {
      t.title = title ? title : "";
      return;
    }
  }
}

void ScreenCapturer::OnToplevelAppId(ext_foreign_toplevel_handle_v1* handle,
                                     const char* app_id) {
  for (auto& t : toplevels_) {
    if (t.handle == handle) {
      t.app_id = app_id ? app_id : "";
      return;
    }
  }
}

void ScreenCapturer::OnToplevelClosed(ext_foreign_toplevel_handle_v1* handle) {
  // If the window we're mirroring goes away, stop cleanly (the capture loop
  // checks running_); closures of other toplevels are ignored.
  if (handle == chosen_toplevel_) {
    OSP_LOG_INFO << "ScreenCapturer: captured window closed; stopping";
    running_ = false;
  }
}

void ScreenCapturer::OnToplevelAddress(
    hyprland_toplevel_window_mapping_handle_v1* maph, uint64_t address) {
  for (auto& t : toplevels_) {
    if (t.map_handle == maph) {
      t.address = address;  // 0 means mapping failed
      toplevels_mapped_count_++;
      return;
    }
  }
}

void ScreenCapturer::SelectToplevel() {
  auto ieq = [](const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
      return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(a[i])) !=
          std::tolower(static_cast<unsigned char>(b[i]))) {
        return false;
      }
    }
    return true;
  };

  chosen_toplevel_ = nullptr;
  if (toplevels_.empty()) {
    return;
  }
  // Preferred path: exact match by Hyprland window address (the reliable key,
  // shared with `hyprctl clients`). Distinguishes windows of the same app.
  if (target_address_ != 0) {
    for (const auto& t : toplevels_) {
      if (t.address == target_address_) {
        chosen_toplevel_ = t.handle;
        current_output_name_ = t.title.empty() ? t.app_id : t.title;
        return;
      }
    }
    // An explicit address that we cannot map must NOT silently fall back to an
    // arbitrary window. castbridge drives mirroring purely by address (it clears
    // the selector), having already resolved and validated a specific window via
    // window_resolver; capturing a different window would be a silent divergence.
    // Fail loudly here (chosen_toplevel_ stays null -> Start() errors out) unless
    // the caller also supplied a selector to fall back on.
    if (selector_.empty()) {
      OSP_LOG_WARN << "ScreenCapturer: no toplevel mapped to address "
                   << target_address_
                   << " and no selector given; refusing to pick an arbitrary "
                      "window";
      return;
    }
    OSP_LOG_WARN << "ScreenCapturer: no toplevel mapped to address "
                 << target_address_ << "; falling back to selector '"
                 << selector_ << "'";
  }
  if (selector_.empty()) {
    chosen_toplevel_ = toplevels_.front().handle;
    return;
  }
  // Fallback: exact (case-insensitive) app-id match, then a title substring.
  for (const auto& t : toplevels_) {
    if (ieq(t.app_id, selector_)) {
      chosen_toplevel_ = t.handle;
      current_output_name_ = t.app_id;
      return;
    }
  }
  for (const auto& t : toplevels_) {
    if (t.title.find(selector_) != std::string::npos) {
      chosen_toplevel_ = t.handle;
      current_output_name_ = t.title;
      return;
    }
  }
}

void ScreenCapturer::OnBufferSize(uint32_t w, uint32_t h) {
  buffer_width_ = static_cast<int>(w);
  buffer_height_ = static_cast<int>(h);
}

void ScreenCapturer::OnShmFormat(uint32_t format) {
  // Prefer the first advertised format (typically XRGB8888/ARGB8888).
  if (!have_format_) {
    shm_format_ = format;
    have_format_ = true;
  }
}

void ScreenCapturer::OnSessionDone() {
  if (!session_ready_) {
    session_ready_ = true;
    CreateAndCaptureFrame();
  }
}

bool ScreenCapturer::EnsureBuffer() {
  const int stride = buffer_width_ * 4;
  const size_t size = static_cast<size_t>(stride) * buffer_height_;
  if (wl_buffer_ && pool_size_ == size) {
    return true;
  }
  if (wl_buffer_) {
    wl_buffer_destroy(wl_buffer_);
    wl_buffer_ = nullptr;
  }
  if (pool_data_) {
    munmap(pool_data_, pool_size_);
    pool_data_ = nullptr;
  }
  if (pool_fd_ >= 0) {
    close(pool_fd_);
    pool_fd_ = -1;
  }

  pool_fd_ = memfd_create("cast-mirror-shm", MFD_CLOEXEC);
  if (pool_fd_ < 0 || ftruncate(pool_fd_, size) != 0) {
    OSP_LOG_ERROR << "ScreenCapturer: failed to allocate shm buffer";
    return false;
  }
  pool_data_ = static_cast<uint8_t*>(
      mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, pool_fd_, 0));
  if (pool_data_ == MAP_FAILED) {
    pool_data_ = nullptr;
    OSP_LOG_ERROR << "ScreenCapturer: mmap failed";
    return false;
  }
  pool_size_ = size;
  buffer_stride_ = stride;

  wl_shm_pool* pool = wl_shm_create_pool(shm_, pool_fd_, size);
  wl_buffer_ = wl_shm_pool_create_buffer(pool, 0, buffer_width_, buffer_height_,
                                         stride, shm_format_);
  wl_shm_pool_destroy(pool);
  return wl_buffer_ != nullptr;
}

void ScreenCapturer::CreateAndCaptureFrame() {
  if (!running_ || !session_ || !EnsureBuffer()) {
    return;
  }
  frame_ = ext_image_copy_capture_session_v1_create_frame(session_);
  ext_image_copy_capture_frame_v1_add_listener(frame_, &kFrameListener, this);
  ext_image_copy_capture_frame_v1_attach_buffer(frame_, wl_buffer_);
  ext_image_copy_capture_frame_v1_capture(frame_);
}

void ScreenCapturer::OnFramePresentationTime(uint64_t nsec) {
  pending_present_nsec_ = nsec;
}

void ScreenCapturer::OnFrameReady() {
  if (on_frame_ && pool_data_) {
    Frame f{pool_data_,      shm_format_,        buffer_width_,
            buffer_height_,  buffer_stride_,     pending_present_nsec_};
    on_frame_(f);
  }
  if (frame_) {
    ext_image_copy_capture_frame_v1_destroy(frame_);
    frame_ = nullptr;
  }
  // Immediately request the next frame to keep the stream continuous.
  CreateAndCaptureFrame();
}

void ScreenCapturer::OnFrameFailed(uint32_t reason) {
  OSP_LOG_WARN << "ScreenCapturer: frame failed, reason=" << reason;
  if (frame_) {
    ext_image_copy_capture_frame_v1_destroy(frame_);
    frame_ = nullptr;
  }
  if (running_) {
    CreateAndCaptureFrame();
  }
}

void ScreenCapturer::ThreadMain() {
  display_ = wl_display_connect(nullptr);
  if (!display_) {
    OSP_LOG_ERROR << "ScreenCapturer: cannot connect to Wayland display";
    return;
  }
  registry_ = wl_display_get_registry(display_);
  wl_registry_add_listener(registry_, &kRegistryListener, this);
  wl_display_roundtrip(display_);  // bind globals
  wl_display_roundtrip(display_);  // receive wl_output names / toplevel handles

  if (!shm_ || !capture_manager_) {
    OSP_LOG_ERROR << "ScreenCapturer: missing required Wayland globals "
                  << "(ext-image-copy-capture supported on this compositor?)";
    return;
  }

  if (target_ == Target::kToplevel) {
    if (!toplevel_source_manager_ || !toplevel_list_) {
      OSP_LOG_ERROR << "ScreenCapturer: window capture unsupported "
                    << "(ext-foreign-toplevel-image-capture-source missing)";
      return;
    }
    // Enumerate to quiescence: ext-foreign-toplevel-list-v1 has no "initial
    // enumeration done" event, and toplevel/app_id/title events stream in over
    // several round-trips. A fixed count races (we'd miss windows). Instead loop
    // round-trips until the set stops growing AND every seen handle has emitted
    // its `done` (so titles/app-ids are populated), bounded to avoid hanging.
    int last_count = -1, stable_rounds = 0;
    for (int i = 0; i < 20 && running_; ++i) {
      wl_display_roundtrip(display_);
      const int n = static_cast<int>(toplevels_.size());
      const bool all_done = toplevels_done_count_ >= n;
      if (n == last_count && all_done) {
        if (++stable_rounds >= 2) {
          break;  // two quiet, fully-described rounds: enumeration settled
        }
      } else {
        stable_rounds = 0;
      }
      last_count = n;
    }

    // Resolve each toplevel's Hyprland window address (the reliable selection
    // key) via hyprland-toplevel-mapping, then wait for the replies. Hyprland's
    // ext-foreign-toplevel `title` is stale for idle windows, so address is the
    // only dependable way to pick a specific window among same-app instances.
    if (target_address_ != 0 && toplevel_mapping_manager_) {
      for (auto& t : toplevels_) {
        t.map_handle = hyprland_toplevel_mapping_manager_v1_get_window_for_toplevel(
            toplevel_mapping_manager_, t.handle);
        hyprland_toplevel_window_mapping_handle_v1_add_listener(
            t.map_handle, &kMappingListener, this);
      }
      for (int i = 0; i < 20 && running_; ++i) {
        wl_display_roundtrip(display_);
        if (toplevels_mapped_count_ >= static_cast<int>(toplevels_.size())) {
          break;
        }
      }
    } else if (target_address_ != 0) {
      OSP_LOG_WARN << "ScreenCapturer: hyprland-toplevel-mapping unavailable; "
                   << "falling back to app-id/title selection";
    }

    SelectToplevel();
    if (!chosen_toplevel_) {
      OSP_LOG_ERROR << "ScreenCapturer: no window matched (address "
                    << target_address_ << ", selector '" << selector_ << "', "
                    << toplevels_.size() << " toplevels seen)";
      return;
    }
    source_ = ext_foreign_toplevel_image_capture_source_manager_v1_create_source(
        toplevel_source_manager_, chosen_toplevel_);
    OSP_LOG_INFO << "ScreenCapturer: capturing window '" << current_output_name_
                 << "'";
  } else {
    if (!source_manager_ || !output_) {
      OSP_LOG_ERROR << "ScreenCapturer: no output to capture";
      return;
    }
    source_ = ext_output_image_capture_source_manager_v1_create_source(
        source_manager_, output_);
    OSP_LOG_INFO << "ScreenCapturer: capturing output '" << current_output_name_
                 << "'";
  }

  session_ = ext_image_copy_capture_manager_v1_create_session(
      capture_manager_, source_, 0);
  ext_image_copy_capture_session_v1_add_listener(session_, &kSessionListener,
                                                 this);

  // Thread-safe Wayland event loop: poll the display fd with a timeout so Stop()
  // (which only flips `running_`) is observed promptly, without any other thread
  // ever touching wl_display.
  const int fd = wl_display_get_fd(display_);
  while (running_) {
    while (wl_display_prepare_read(display_) != 0) {
      wl_display_dispatch_pending(display_);
    }
    wl_display_flush(display_);

    pollfd pfd{fd, POLLIN, 0};
    const int r = poll(&pfd, 1, 100);  // 100 ms timeout to re-check running_
    if (r > 0 && (pfd.revents & POLLIN)) {
      wl_display_read_events(display_);
      wl_display_dispatch_pending(display_);
    } else {
      wl_display_cancel_read(display_);
    }
  }

  if (frame_) {
    ext_image_copy_capture_frame_v1_destroy(frame_);
  }
  if (session_) {
    ext_image_copy_capture_session_v1_destroy(session_);
  }
  if (wl_buffer_) {
    wl_buffer_destroy(wl_buffer_);
  }
  if (pool_data_) {
    munmap(pool_data_, pool_size_);
  }
  if (pool_fd_ >= 0) {
    close(pool_fd_);
  }
  wl_display_disconnect(display_);
  display_ = nullptr;
}

}  // namespace openscreen::cast
