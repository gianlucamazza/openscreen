// Copyright 2026. BSD-style license.

#include "cast/standalone_sender/audio_capturer.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

// PipeWire/SPA headers use GNU case-range extensions; silence under -Werror.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wgnu-case-range"
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/hook.h>
#pragma clang diagnostic pop

#include "util/osp_logging.h"

namespace openscreen::cast {

namespace {
const pw_stream_events kStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = &AudioCapturer::OnProcess,
};

const pw_registry_events kRegistryEvents = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = &AudioCapturer::OnRegistryGlobal,
    .global_remove = &AudioCapturer::OnRegistryGlobalRemove,
};

// Read the parent PID of `pid` from /proc/<pid>/stat (field 4). Returns 0 on
// failure or for pid<=1. Field 2 (comm) may contain spaces/parens, so scan to
// the last ')' first, then the state char, then ppid.
int ParentPid(int pid) {
  if (pid <= 1) {
    return 0;
  }
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/%d/stat", pid);
  std::FILE* f = std::fopen(path, "re");
  if (!f) {
    return 0;
  }
  char buf[512];
  const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
  std::fclose(f);
  if (n == 0) {
    return 0;
  }
  buf[n] = '\0';
  const char* rparen = nullptr;
  for (const char* p = buf; *p; ++p) {
    if (*p == ')') {
      rparen = p;
    }
  }
  if (!rparen) {
    return 0;
  }
  // After ")": " <state> <ppid> ..."
  char state = 0;
  int ppid = 0;
  if (std::sscanf(rparen + 1, " %c %d", &state, &ppid) == 2) {
    return ppid;
  }
  return 0;
}

}  // namespace

AudioCapturer::AudioCapturer() = default;

AudioCapturer::~AudioCapturer() {
  Stop();
}

void AudioCapturer::OnProcess(void* userdata) {
  auto* self = static_cast<AudioCapturer*>(userdata);
  pw_buffer* b = pw_stream_dequeue_buffer(self->stream_);
  if (!b) {
    return;
  }
  spa_buffer* buf = b->buffer;
  spa_data& d = buf->datas[0];
  if (d.data && d.chunk->size > 0 && self->channels_ > 0) {
    const auto* samples = reinterpret_cast<const float*>(
        static_cast<uint8_t*>(d.data) + d.chunk->offset);
    const int frame_bytes = sizeof(float) * self->channels_;
    const int num_frames = static_cast<int>(d.chunk->size) / frame_bytes;
    if (num_frames > 0 && self->on_audio_) {
      self->on_audio_(samples, num_frames, self->channels_);
    }
  }
  pw_stream_queue_buffer(self->stream_, b);
}

static std::string ToLower(const char* s) {
  std::string out(s ? s : "");
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return out;
}

// True if `app` (a lowercased application name/binary) matches `target` (also
// lowercased): equal, or one is a prefix of the other (e.g. "librewolf" vs
// "librewolf-bin"). Empty inputs never match.
static bool AppMatches(const std::string& target, const std::string& app) {
  if (target.empty() || app.empty()) {
    return false;
  }
  return app == target || app.rfind(target, 0) == 0 ||
         target.rfind(app, 0) == 0;
}

// True if `pid` is one of the target PIDs or a descendant of one (walk the
// parent chain up to init). Bounded depth guards against cycles.
static bool IsPidTargeted(const std::set<int>& targets, int pid) {
  int cur = pid;
  for (int depth = 0; depth < 64 && cur > 1; ++depth) {
    if (targets.count(cur)) {
      return true;
    }
    cur = ParentPid(cur);
  }
  return false;
}

void AudioCapturer::OnRegistryGlobal(void* data, uint32_t id,
                                     uint32_t /*permissions*/,
                                     const char* type, uint32_t /*version*/,
                                     const spa_dict* props) {
  auto* self = static_cast<AudioCapturer*>(data);
  if (!props || !type) {
    return;
  }
  // Record Client objects' PID + app identity: app audio nodes often carry
  // these only on their owning Client (looked up via client.id). Prefer
  // `application.process.id` (the actual app PID, e.g. the browser) over
  // `pipewire.sec.pid` — the latter is the *connecting* process, which for apps
  // launched via a wrapper/portal is a launcher unrelated to the window PID.
  if (std::string(type) == PW_TYPE_INTERFACE_Client) {
    const char* p = spa_dict_lookup(props, PW_KEY_APP_PROCESS_ID);
    if (!p) {
      p = spa_dict_lookup(props, PW_KEY_SEC_PID);
    }
    const char* app = spa_dict_lookup(props, PW_KEY_APP_PROCESS_BINARY);
    if (!app) {
      app = spa_dict_lookup(props, PW_KEY_APP_NAME);
    }
    if (p) {
      self->client_pids_[id] = std::atoi(p);
    }
    if (app) {
      self->client_apps_[id] = ToLower(app);
    }
    if (self->target_serial_ == 0 && (p || app)) {
      self->TryMatchPendingNodes();  // a pending node may resolve now
    }
    return;
  }
  if (self->target_serial_ != 0 ||
      std::string(type) != PW_TYPE_INTERFACE_Node) {
    return;  // already tapping, or not a node
  }
  const char* media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
  if (!media_class || std::string(media_class) != "Stream/Output/Audio") {
    return;
  }
  const char* serial_str = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
  if (!serial_str) {
    return;
  }
  NodeCandidate cand{};
  cand.serial = static_cast<uint32_t>(std::strtoul(serial_str, nullptr, 10));
  if (const char* pid_str = spa_dict_lookup(props, PW_KEY_APP_PROCESS_ID)) {
    cand.own_pid = std::atoi(pid_str);
  }
  // Always remember the owning client: even when the node carries its own PID,
  // the app identity (for name matching) may live only on the Client object.
  if (const char* cid = spa_dict_lookup(props, PW_KEY_CLIENT_ID)) {
    cand.client_id = static_cast<uint32_t>(std::strtoul(cid, nullptr, 10));
  }
  if (const char* app = spa_dict_lookup(props, PW_KEY_APP_PROCESS_BINARY)) {
    cand.app = ToLower(app);
  } else if (const char* an = spa_dict_lookup(props, PW_KEY_APP_NAME)) {
    cand.app = ToLower(an);
  }
  self->candidate_nodes_[id] = cand;
  self->TryMatchPendingNodes();
}

void AudioCapturer::TryMatchPendingNodes() {
  if (target_serial_ != 0) {
    return;
  }
  for (const auto& [node_id, cand] : candidate_nodes_) {
    int pid = cand.own_pid;
    std::string app = cand.app;
    if ((pid <= 0 || app.empty()) && cand.client_id != 0) {
      if (pid <= 0) {
        const auto it = client_pids_.find(cand.client_id);
        if (it != client_pids_.end()) {
          pid = it->second;
        }
      }
      if (app.empty()) {
        const auto it = client_apps_.find(cand.client_id);
        if (it != client_apps_.end()) {
          app = it->second;
        }
      }
    }
    const bool pid_hit = pid > 0 && IsPidTargeted(target_pids_, pid);
    const bool name_hit = AppMatches(target_app_, app);
    if (pid_hit || name_hit) {
      target_serial_ = cand.serial;
      target_node_id_ = node_id;
      OSP_LOG_INFO << "AudioCapturer: tapping app audio (pid " << pid
                   << ", app '" << app << "', match "
                   << (pid_hit ? "pid" : "name") << ", serial "
                   << target_serial_ << ")";
      ConnectStream();
      return;
    }
  }
}

void AudioCapturer::OnRegistryGlobalRemove(void* data, uint32_t id) {
  auto* self = static_cast<AudioCapturer*>(data);
  self->candidate_nodes_.erase(id);
  self->client_pids_.erase(id);
  self->client_apps_.erase(id);
  if (id == self->target_node_id_ && self->target_node_id_ != 0) {
    OSP_LOG_INFO << "AudioCapturer: tapped app node gone; awaiting next";
    self->DestroyStream();
    self->target_serial_ = 0;
    self->target_node_id_ = 0;
    // Another instance of the app may still be playing; try to re-acquire.
    self->TryMatchPendingNodes();
  }
}

bool AudioCapturer::Start(int channels, int sample_rate,
                          FrameCallback on_audio) {
  // System-mix mode: no target PIDs; connect the sink-monitor stream right away.
  return StartInternal(channels, sample_rate, std::move(on_audio));
}

bool AudioCapturer::StartForPids(int channels, int sample_rate,
                                 const std::set<int>& pids,
                                 const std::string& app,
                                 FrameCallback on_audio) {
  target_pids_ = pids;
  target_app_ = ToLower(app.c_str());
  return StartInternal(channels, sample_rate, std::move(on_audio));
}

bool AudioCapturer::StartInternal(int channels, int sample_rate,
                                  FrameCallback on_audio) {
  on_audio_ = std::move(on_audio);
  channels_ = channels;
  sample_rate_ = sample_rate;

  pw_init(nullptr, nullptr);

  loop_ = pw_thread_loop_new("cast-mirror-audio", nullptr);
  if (!loop_) {
    OSP_LOG_WARN << "AudioCapturer: pw_thread_loop_new failed";
    return false;
  }

  if (target_pids_.empty() && target_app_.empty()) {
    // System mix: connect the sink-monitor stream immediately.
    if (!ConnectStream()) {
      Stop();
      return false;
    }
  } else {
    // Per-app: stand up a registry to discover the app's node; the stream is
    // connected lazily once a matching Stream/Output/Audio node appears.
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (!context_) {
      OSP_LOG_WARN << "AudioCapturer: pw_context_new failed";
      Stop();
      return false;
    }
    core_ = pw_context_connect(context_, nullptr, 0);
    if (!core_) {
      OSP_LOG_WARN << "AudioCapturer: pw_context_connect failed";
      Stop();
      return false;
    }
    registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
    if (!registry_) {
      OSP_LOG_WARN << "AudioCapturer: pw_core_get_registry failed";
      Stop();
      return false;
    }
    registry_hook_ = new spa_hook{};
    pw_registry_add_listener(registry_, registry_hook_, &kRegistryEvents, this);
  }

  if (pw_thread_loop_start(loop_) < 0) {
    OSP_LOG_WARN << "AudioCapturer: pw_thread_loop_start failed";
    Stop();
    return false;
  }
  running_ = true;
  if (target_pids_.empty() && target_app_.empty()) {
    OSP_LOG_INFO << "AudioCapturer: capturing default sink monitor @ "
                 << sample_rate << " Hz, " << channels << " ch";
  } else {
    OSP_LOG_INFO << "AudioCapturer: per-app capture armed @ " << sample_rate
                 << " Hz, " << channels << " ch (awaiting app audio)";
  }
  return true;
}

bool AudioCapturer::ConnectStream() {
  // Request a ~10 ms quantum so buffers arrive at a steady low-latency cadence.
  char latency[32];
  std::snprintf(latency, sizeof(latency), "%d/%d", sample_rate_ / 100,
                sample_rate_);

  pw_properties* props = nullptr;
  if (target_pids_.empty() && target_app_.empty()) {
    // STREAM_CAPTURE_SINK records the monitor of the default sink (everything).
    props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Music", PW_KEY_STREAM_CAPTURE_SINK, "true",
        PW_KEY_NODE_LATENCY, latency, nullptr);
  } else {
    // Per-app: tap the matched app output node by object.serial. No
    // CAPTURE_SINK: we link directly to that node's output (fan-out, the app
    // keeps playing on its own sinks).
    char serial[32];
    std::snprintf(serial, sizeof(serial), "%u", target_serial_);
    props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Music", PW_KEY_TARGET_OBJECT, serial,
        PW_KEY_NODE_LATENCY, latency, nullptr);
  }

  stream_ = pw_stream_new_simple(pw_thread_loop_get_loop(loop_),
                                 "cast-mirror-audio", props, &kStreamEvents,
                                 this);
  if (!stream_) {
    OSP_LOG_WARN << "AudioCapturer: pw_stream_new_simple failed";
    return false;
  }

  uint8_t buffer[1024];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  spa_audio_info_raw info = {};
  info.format = SPA_AUDIO_FORMAT_F32;
  info.rate = static_cast<uint32_t>(sample_rate_);
  info.channels = static_cast<uint32_t>(channels_);
  const spa_pod* params[1] = {
      spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};

  // Non-RT process callback (no RT_PROCESS flag) so the consumer may allocate.
  int err = pw_stream_connect(
      stream_, PW_DIRECTION_INPUT, PW_ID_ANY,
      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                   PW_STREAM_FLAG_MAP_BUFFERS),
      params, 1);
  if (err < 0) {
    OSP_LOG_WARN << "AudioCapturer: pw_stream_connect failed: " << err;
    pw_stream_destroy(stream_);
    stream_ = nullptr;
    return false;
  }
  return true;
}

void AudioCapturer::DestroyStream() {
  if (stream_) {
    pw_stream_destroy(stream_);
    stream_ = nullptr;
  }
}

void AudioCapturer::Stop() {
  if (loop_) {
    pw_thread_loop_stop(loop_);
  }
  DestroyStream();
  if (registry_hook_) {
    spa_hook_remove(registry_hook_);
    delete registry_hook_;
    registry_hook_ = nullptr;
  }
  if (registry_) {
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
    registry_ = nullptr;
  }
  if (core_) {
    pw_core_disconnect(core_);
    core_ = nullptr;
  }
  if (context_) {
    pw_context_destroy(context_);
    context_ = nullptr;
  }
  if (loop_) {
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
  }
  running_ = false;
}

}  // namespace openscreen::cast
