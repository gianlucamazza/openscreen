// Copyright 2026. BSD-style license.
//
// AudioCapturer: a minimal PipeWire client that delivers interleaved 32-bit
// float samples, in chunks, via a callback on its own thread.
//
// Two modes:
//  - System mix (default): records the default sink's monitor, i.e. everything
//    the desktop plays (PW_KEY_STREAM_CAPTURE_SINK). Used by the output mirror.
//  - Per-app: taps only the audio of a given set of process IDs (a window's PID
//    plus its descendants), used by the window mirror so the cast carries only
//    the mirrored app's sound. This needs the PipeWire registry to discover the
//    app's Stream/Output/Audio node and target it.
//
// Like ScreenCapturer it is intentionally free of openscreen types so it stays
// a self-contained, reusable capture engine. The consumer feeds the samples to
// the Opus encoder (which handles its own 10 ms framing).

#ifndef CAST_STANDALONE_SENDER_AUDIO_CAPTURER_H_
#define CAST_STANDALONE_SENDER_AUDIO_CAPTURER_H_

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>

struct pw_thread_loop;
struct pw_stream;
struct pw_context;
struct pw_core;
struct pw_registry;
struct spa_hook;
struct spa_dict;

namespace openscreen::cast {

class AudioCapturer {
 public:
  // `samples` are `num_frames` * `channels` interleaved floats, valid only for
  // the duration of the callback. Invoked on the internal PipeWire thread.
  using FrameCallback =
      std::function<void(const float* samples, int num_frames, int channels)>;

  AudioCapturer();
  ~AudioCapturer();

  AudioCapturer(const AudioCapturer&) = delete;
  AudioCapturer& operator=(const AudioCapturer&) = delete;

  // Start recording the default sink monitor (system mix) at `sample_rate` Hz /
  // `channels`. `on_audio` is invoked on the capture thread for every buffer.
  // Returns false if PipeWire is unavailable or the stream could not be created.
  bool Start(int channels, int sample_rate, FrameCallback on_audio);

  // Start recording only the audio of processes in `pids` (e.g. a window's PID
  // and its descendants), tapping the matching app's PipeWire node. The tap is
  // non-invasive (the app keeps playing on its own sinks). If no matching node
  // exists yet, the capturer waits for one to appear (the consumer should fill
  // silence meanwhile). Returns false only if PipeWire itself is unavailable.
  // `pids` taps the app by PID (and descendants); `app` additionally matches the
  // node by application name / binary (case-insensitive), for apps whose node
  // reports a launcher PID. Either may be empty; at least one should be set.
  bool StartForPids(int channels, int sample_rate, const std::set<int>& pids,
                    const std::string& app, FrameCallback on_audio);
  void Stop();

  // PipeWire trampolines (public so the file-scope event tables can name them);
  // forward into the instance. Do not call directly.
  static void OnProcess(void* userdata);
  static void OnRegistryGlobal(void* data, uint32_t id, uint32_t permissions,
                               const char* type, uint32_t version,
                               const struct spa_dict* props);
  static void OnRegistryGlobalRemove(void* data, uint32_t id);

 private:
  // Shared setup for both modes; `target_serial`==0 means the system-mix path.
  bool StartInternal(int channels, int sample_rate, FrameCallback on_audio);
  // Re-scan pending candidate app nodes against the known client PIDs; taps the
  // first that resolves to a targeted process. Registry globals can arrive out
  // of order (a node before its owning Client), so matching is deferred here.
  void TryMatchPendingNodes();
  // Create (or recreate) the capture stream targeting `target_serial_` (0 for
  // the sink monitor). Runs on the PipeWire loop thread.
  bool ConnectStream();
  void DestroyStream();

  pw_thread_loop* loop_ = nullptr;
  pw_stream* stream_ = nullptr;
  FrameCallback on_audio_;
  int channels_ = 0;
  int sample_rate_ = 0;
  bool running_ = false;

  // Per-app mode state (empty `target_pids_` AND empty `target_app_` => system
  // mix mode).
  std::set<int> target_pids_;
  // Lowercased app identity (application name or binary) to match nodes by name.
  std::string target_app_;
  pw_context* context_ = nullptr;
  pw_core* core_ = nullptr;
  pw_registry* registry_ = nullptr;
  spa_hook* registry_hook_ = nullptr;
  // object.serial of the app node we're currently tapping (0 = none yet).
  uint32_t target_serial_ = 0;
  // registry id of that node, to detect its removal.
  uint32_t target_node_id_ = 0;
  // PipeWire Client registry id -> owning process PID. Many app nodes don't
  // carry application.process.id themselves (e.g. mpv); the PID lives on their
  // Client object, looked up via the node's client.id.
  std::map<uint32_t, int> client_pids_;
  // PipeWire Client registry id -> lowercased app identity (application name or
  // binary), used for name-based matching when the PID doesn't line up.
  std::map<uint32_t, std::string> client_apps_;
  // Candidate Stream/Output/Audio nodes not yet matched (registry id -> info).
  // `own_pid` is the node's own application.process.id (0 if absent → resolve
  // via `client_id`); `app` is its lowercased app identity (may be empty →
  // resolve via the owning Client).
  struct NodeCandidate {
    uint32_t serial;
    uint32_t client_id;
    int own_pid;
    std::string app;
  };
  std::map<uint32_t, NodeCandidate> candidate_nodes_;
};

}  // namespace openscreen::cast

#endif  // CAST_STANDALONE_SENDER_AUDIO_CAPTURER_H_
