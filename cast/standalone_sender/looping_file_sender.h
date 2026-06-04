// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_STANDALONE_SENDER_LOOPING_FILE_SENDER_H_
#define CAST_STANDALONE_SENDER_LOOPING_FILE_SENDER_H_

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cast/standalone_sender/connection_settings.h"
#include "cast/standalone_sender/constants.h"
#include "cast/standalone_sender/h264_vaapi_encoder.h"
#include "cast/standalone_sender/simulated_capturer.h"
#include "cast/standalone_sender/streaming_opus_encoder.h"
#include "cast/standalone_sender/streaming_video_encoder.h"
#include "cast/streaming/public/sender_session.h"

struct SwsContext;

namespace openscreen::cast {

// Plays the media file at a given path over and over again, transcoding and
// streaming its audio/video.
class LoopingFileSender final : public SimulatedAudioCapturer::Client,
                                public SimulatedVideoCapturer::Client {
 public:
  using ShutdownCallback = std::function<void()>;

  LoopingFileSender(Environment& environment,
                    ConnectionSettings settings,
                    const SenderSession* session,
                    SenderSession::ConfiguredSenders senders,
                    ShutdownCallback shutdown_callback);

  ~LoopingFileSender() final;

  void SetPlaybackRate(double rate);

  void OnInputMessage(InputMessage message);

 private:
  void UpdateEncoderBitrates();
  void ControlForNetworkCongestion();
  void SendFileAgain();

  // SimulatedAudioCapturer overrides.
  void OnAudioData(const float* interleaved_samples,
                   int num_samples,
                   Clock::time_point capture_begin_time,
                   Clock::time_point capture_end_time,
                   Clock::time_point reference_time) final;

  // SimulatedVideoCapturer overrides;
  void OnVideoFrame(const AVFrame& av_frame,
                    Clock::time_point capture_begin_time,
                    Clock::time_point capture_end_time,
                    Clock::time_point reference_time) final;

  void UpdateStatusOnConsole();

  // Draws any active animations (like mouse clicks) onto the frame.
  void DrawAnimations(const AVFrame& av_frame,
                      int frame_width,
                      int frame_height);

  // SimulatedCapturer::Client overrides.
  void OnEndOfFile(SimulatedCapturer* capturer) final;
  void OnError(SimulatedCapturer* capturer, const std::string& message) final;

  const char* ToTrackName(SimulatedCapturer* capturer) const;

  std::unique_ptr<StreamingVideoEncoder> CreateVideoEncoder(
      const StreamingVideoEncoder::Parameters& params,
      TaskRunner& task_runner,
      std::unique_ptr<Sender> sender);

  // H.264 path: convert the decoded I420 frame to NV12 (downscaling to fit the
  // negotiated 1080p box) and hand it to the VAAPI hardware encoder.
  void EncodeVideoFrameH264(const AVFrame& av_frame,
                            Clock::time_point reference_time);

  // Holds the required injected dependencies (clock, task runner) used for Cast
  // Streaming, and owns the UDP socket over which all communications occur with
  // the remote's Receivers.
  Environment& env_;

  // The connection settings used for this session.
  const ConnectionSettings settings_;

  // Session to query for bandwidth information.
  const SenderSession* session_;

  // Callback for tearing down the sender process.
  ShutdownCallback shutdown_callback_;

  int bandwidth_estimate_ = 0;
  int bandwidth_being_utilized_;

  StreamingOpusEncoder audio_encoder_;
  std::unique_ptr<StreamingVideoEncoder> video_encoder_;

  // VAAPI H.264 hardware encode path (selected when the negotiated video codec
  // is H.264; the iGPU has no VP8/VP9 hardware encode). `use_h264_` chooses it
  // over `video_encoder_`; `sws_` converts the decoded I420 frame to NV12.
  std::unique_ptr<H264VaapiEncoder> h264_encoder_;
  bool use_h264_ = false;
  SwsContext* sws_ = nullptr;
  int sws_src_w_ = 0;
  int sws_src_h_ = 0;
  int sws_src_fmt_ = -1;
  // Reusable NV12 scratch buffer for the H.264 path (synchronous encode, so no
  // per-frame allocation needed). Grown on demand in EncodeVideoFrameH264.
  std::vector<uint8_t> nv12_buf_;

  int num_capturers_running_ = 0;
  Clock::time_point capture_begin_time_{};
  Clock::time_point latest_frame_time_{};
  std::optional<SimulatedAudioCapturer> audio_capturer_;
  std::optional<SimulatedVideoCapturer> video_capturer_;

  struct Click {
    float x;
    float y;
    Clock::time_point start_time;
    Clock::time_point end_time;
  };
  std::vector<Click> active_clicks_;

  Alarm next_task_;
  Alarm console_update_task_;
};

}  // namespace openscreen::cast

#endif  // CAST_STANDALONE_SENDER_LOOPING_FILE_SENDER_H_
