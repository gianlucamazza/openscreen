// Copyright 2026. BSD-style license.
//
// ScreenMirrorSender: drop-in alternative to LoopingFileSender that sources
// video from the live Wayland screen (ScreenCapturer) instead of a media file.
// Phase 1: RGB capture -> swscale to I420 -> the existing libvpx encoder ->
// Sender::EnqueueFrame. Audio is a steady silent Opus track (the mirroring
// receiver expects an audio stream).

#ifndef CAST_STANDALONE_SENDER_SCREEN_MIRROR_SENDER_H_
#define CAST_STANDALONE_SENDER_SCREEN_MIRROR_SENDER_H_

#include <functional>
#include <memory>
#include <vector>

#include "cast/standalone_sender/audio_capturer.h"
#include "cast/standalone_sender/connection_settings.h"
#include "cast/standalone_sender/h264_vaapi_encoder.h"
#include "cast/standalone_sender/screen_capturer.h"
#include "cast/standalone_sender/streaming_opus_encoder.h"
#include "cast/standalone_sender/streaming_video_encoder.h"
#include "cast/streaming/public/sender_session.h"
#include "platform/api/time.h"
#include "util/alarm.h"
#include "util/weak_ptr.h"

struct SwsContext;

namespace openscreen::cast {

class ScreenMirrorSender final {
 public:
  using ShutdownCallback = std::function<void()>;

  ScreenMirrorSender(Environment& environment,
                     ConnectionSettings settings,
                     const SenderSession* session,
                     SenderSession::ConfiguredSenders senders,
                     ShutdownCallback shutdown_callback);
  ~ScreenMirrorSender();

  // No-ops kept for interface parity with the file sender (input/playback).
  void SetPlaybackRate(double) {}
  void OnInputMessage(InputMessage) {}

 private:
  void UpdateEncoderBitrates();
  void ControlForNetworkCongestion();
  void SendSilentAudio();

  // Encode+send one audio buffer on the main task runner, advancing the shared
  // sample-accurate timeline (audio_epoch_/audio_samples_sent_). `is_real`
  // marks captured app audio (vs. gap-filler silence) so the watchdog knows
  // when real audio last flowed.
  void EmitAudioFrames(const float* pcm, int num_frames, Clock::time_point now,
                       bool is_real);
  // Per-app audio watchdog: if no real app audio arrived recently, emit a tick
  // of silence so the audio track never stalls (a window may be silent, or its
  // stream may not exist yet). Reschedules itself every 10 ms.
  void FillAudioGap();

  // Invoked on the ScreenCapturer thread for each captured frame. Converts to
  // I420 and posts the encode onto the main task runner.
  void OnCapturedFrame(const ScreenCapturer::Frame& frame);

  // Invoked on the AudioCapturer thread for each PipeWire buffer. Copies the
  // samples and posts the Opus encode onto the main task runner.
  void OnCapturedAudio(const float* samples, int num_frames, int channels);

  std::unique_ptr<StreamingVideoEncoder> CreateVideoEncoder(
      const StreamingVideoEncoder::Parameters& params,
      TaskRunner& task_runner,
      std::unique_ptr<Sender> sender);

  Environment& env_;
  const ConnectionSettings settings_;
  const SenderSession* session_;
  ShutdownCallback shutdown_callback_;

  int bandwidth_estimate_ = 0;
  int bandwidth_being_utilized_;

  StreamingOpusEncoder audio_encoder_;
  // Exactly one of these is set, depending on the negotiated codec: libvpx
  // (VP8/VP9, software) or VAAPI H.264 (hardware). `use_h264_` selects which.
  std::unique_ptr<StreamingVideoEncoder> video_encoder_;
  std::unique_ptr<H264VaapiEncoder> h264_encoder_;
  bool use_h264_ = false;
  // Window mirror (vs. whole output): the captured frame is composited, scaled
  // to fit and letterboxed, onto a full 1920x1080 canvas so the TV displays it
  // 1:1 without its own (soft) upscaling or aspect stretching.
  bool window_mode_ = false;

  ScreenCapturer capturer_;
  AudioCapturer audio_capturer_;
  // True when PipeWire capture is running; otherwise we fall back to silence.
  bool real_audio_active_ = false;

  // swscale RGB->I420 state (created lazily on first frame).
  SwsContext* sws_ = nullptr;
  int sws_src_format_ = -1;
  int sws_w_ = 0;
  int sws_h_ = 0;

  Clock::time_point last_video_time_{};

  // Audio timeline (silent, gapless).
  Clock::time_point audio_reference_time_{};
  bool audio_started_ = false;

  // Real-audio (PipeWire) timeline, maintained on the main task runner: a
  // sample-accurate reference time anchored at the first captured buffer.
  // Wall-clock per-buffer timestamps would jitter and trip the Opus encoder's
  // gap detector; counting samples keeps the cadence smooth.
  Clock::time_point audio_epoch_{};
  int64_t audio_samples_sent_ = 0;
  // When real app audio (per-app mode) last arrived; drives FillAudioGap.
  Clock::time_point last_real_audio_time_{};
  // True in window mirror when capturing a single app's audio (PID > 0).
  bool per_app_audio_ = false;

  Alarm next_task_;
  Alarm audio_task_;

  // Must be the LAST member: invalidated first on destruction, so any frame
  // tasks already posted from the capture thread (which capture this WeakPtr)
  // silently no-op instead of touching half-destroyed members. See weak_ptr.h.
  WeakPtrFactory<ScreenMirrorSender> weak_factory_{this};
};

}  // namespace openscreen::cast

#endif  // CAST_STANDALONE_SENDER_SCREEN_MIRROR_SENDER_H_
