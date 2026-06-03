// Copyright 2026. BSD-style license.
//
// H264VaapiEncoder: a minimal, low-latency H.264 encoder built on libavcodec's
// `h264_vaapi` (hardware encode via /dev/dri/renderD128). It takes NV12 frames,
// encodes them as Annex-B H.264, wraps each access unit into an openscreen
// `EncodedFrame`, and hands it to a `Sender`.
//
// Rationale: this machine's iGPU (Tiger Lake) has no VP8/VP9/AV1 hardware
// encode, only H.264/HEVC. The Cast mirroring receiver on the target TV accepts
// an H.264 OFFER, so we feed it hardware-encoded H.264 directly, bypassing the
// software libvpx path (which was the latency/sharpness bottleneck).
//
// All methods run on the openscreen main TaskRunner thread (the same thread that
// owns the Sender). EncodeFrame() is called synchronously from there.

#ifndef CAST_STANDALONE_SENDER_H264_VAAPI_ENCODER_H_
#define CAST_STANDALONE_SENDER_H264_VAAPI_ENCODER_H_

#include <cstdint>
#include <memory>

#include "cast/streaming/public/sender.h"
#include "platform/api/time.h"

struct AVBufferRef;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace openscreen::cast {

class H264VaapiEncoder {
 public:
  // `sender` transmits the encoded frames. `max_bitrate` is the target encode
  // bitrate in bits/sec (may be updated later via SetTargetBitrate()).
  H264VaapiEncoder(std::unique_ptr<Sender> sender, int max_bitrate);
  ~H264VaapiEncoder();

  H264VaapiEncoder(const H264VaapiEncoder&) = delete;
  H264VaapiEncoder& operator=(const H264VaapiEncoder&) = delete;

  // Encode one NV12 frame and enqueue it on the Sender. `y`/`uv` point to the
  // luma and interleaved-chroma planes; `width`/`height` are the frame size
  // (even). `reference_time` is the presentation/reference timestamp used for
  // RTP timing (must be monotonically increasing). Frames may be dropped if the
  // in-flight media duration would exceed the receiver's limit.
  void EncodeFrame(const uint8_t* y,
                   int y_stride,
                   const uint8_t* uv,
                   int uv_stride,
                   int width,
                   int height,
                   Clock::time_point reference_time);

  void SetTargetBitrate(int bitrate);

 private:
  // Lazily builds the VAAPI device, encoder context and hw frame pool for the
  // given size. Returns false on failure (encoding is then skipped).
  bool EnsureInitialized(int width, int height);
  void Teardown();

  std::unique_ptr<Sender> sender_;
  int target_bitrate_;

  AVBufferRef* hw_device_ctx_ = nullptr;
  AVBufferRef* hw_frames_ctx_ = nullptr;
  AVCodecContext* ctx_ = nullptr;
  AVFrame* hw_frame_ = nullptr;
  AVPacket* packet_ = nullptr;
  int enc_width_ = 0;
  int enc_height_ = 0;

  // RTP timeline state.
  Clock::time_point start_time_ = Clock::time_point::min();
  RtpTimeTicks last_rtp_timestamp_;
};

}  // namespace openscreen::cast

#endif  // CAST_STANDALONE_SENDER_H264_VAAPI_ENCODER_H_
