// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/standalone_sender/looping_file_sender.h"

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#if defined(CAST_STANDALONE_SENDER_HAVE_LIBAOM)
#include "cast/standalone_sender/streaming_av1_encoder.h"
#endif
#include "cast/standalone_sender/streaming_vpx_encoder.h"
#include "platform/base/trivial_clock_traits.h"
#include "util/osp_logging.h"
#include "util/trace_logging.h"

namespace openscreen::cast {

LoopingFileSender::LoopingFileSender(Environment& environment,
                                     ConnectionSettings settings,
                                     const SenderSession* session,
                                     SenderSession::ConfiguredSenders senders,
                                     ShutdownCallback shutdown_callback)
    : env_(environment),
      settings_(std::move(settings)),
      session_(session),
      shutdown_callback_(std::move(shutdown_callback)),
      audio_encoder_(senders.audio_sender->config().channels,
                     StreamingOpusEncoder::kDefaultCastAudioFramesPerSecond,
                     std::move(senders.audio_sender)),
      next_task_(env_.now_function(), env_.task_runner()),
      console_update_task_(env_.now_function(), env_.task_runner()) {
  // Opus and Vp8 are the default values for the config, and if these are set
  // to a different value that means we offered a codec that we do not
  // support, which is a developer error.
  OSP_CHECK(senders.audio_config.codec == AudioCodec::kOpus);
  OSP_CHECK(senders.video_config.codec == VideoCodec::kVp8 ||
            senders.video_config.codec == VideoCodec::kVp9 ||
            senders.video_config.codec == VideoCodec::kAv1 ||
            senders.video_config.codec == VideoCodec::kH264);

  // H.264 is hardware-encoded via VAAPI (no StreamingVideoEncoder); VP8/VP9/AV1
  // go through the software encoder factory. Mirrors ScreenMirrorSender. Branch
  // on the negotiated codec (the field the OSP_CHECK above validates) so the
  // encoder choice matches what the receiver actually answered.
  use_h264_ = (senders.video_config.codec == VideoCodec::kH264);
  if (use_h264_) {
    h264_encoder_ = std::make_unique<H264VaapiEncoder>(
        std::move(senders.video_sender), settings_.max_bitrate);
  } else {
    video_encoder_ = CreateVideoEncoder(
        StreamingVideoEncoder::Parameters{.codec = settings_.codec},
        env_.task_runner(), std::move(senders.video_sender));
  }

  OSP_LOG_INFO << "Max allowed media bitrate (audio + video) will be "
               << settings_.max_bitrate;
  bandwidth_being_utilized_ = settings_.max_bitrate / 2;
  UpdateEncoderBitrates();

  next_task_.Schedule([this] { SendFileAgain(); }, Alarm::kImmediately);
}

LoopingFileSender::~LoopingFileSender() {
  if (sws_) {
    sws_freeContext(sws_);
  }
}

void LoopingFileSender::SetPlaybackRate(double rate) {
  video_capturer_->SetPlaybackRate(rate);
  audio_capturer_->SetPlaybackRate(rate);
}

void LoopingFileSender::OnInputMessage(InputMessage message) {
  const auto now = env_.now();
  for (const auto& event : message.events()) {
    if (event.type() == InputMessage::INPUT_TYPE_MOUSE_DOWN &&
        event.has_mouse_event()) {
      active_clicks_.push_back(Click{event.mouse_event().location().x(),
                                     event.mouse_event().location().y(), now,
                                     now + std::chrono::milliseconds(500)});
    }
  }
}

void LoopingFileSender::UpdateEncoderBitrates() {
  if (bandwidth_being_utilized_ >= kHighBandwidthThreshold) {
    audio_encoder_.UseHighQuality();
  } else {
    audio_encoder_.UseStandardQuality();
  }
  const int video_bitrate =
      bandwidth_being_utilized_ - audio_encoder_.GetBitrate();
  if (use_h264_) {
    h264_encoder_->SetTargetBitrate(video_bitrate);
  } else {
    video_encoder_->SetTargetBitrate(video_bitrate);
  }
}

void LoopingFileSender::ControlForNetworkCongestion() {
  bandwidth_estimate_ = session_->GetEstimatedNetworkBandwidth();
  if (bandwidth_estimate_ > 0) {
    // Don't ever try to use *all* of the network bandwidth! However, don't go
    // below the absolute minimum requirement either.
    constexpr double kGoodNetworkCitizenFactor = 0.8;
    const int usable_bandwidth = std::max<int>(
        kGoodNetworkCitizenFactor * bandwidth_estimate_, kMinRequiredBitrate);

    // See "congestion control" discussion in the class header comments for
    // BandwidthEstimator.
    if (usable_bandwidth > bandwidth_being_utilized_) {
      constexpr double kConservativeIncrease = 1.1;
      bandwidth_being_utilized_ = std::min<int>(
          bandwidth_being_utilized_ * kConservativeIncrease, usable_bandwidth);
    } else {
      bandwidth_being_utilized_ = usable_bandwidth;
    }

    // Repsect the user's maximum bitrate setting.
    bandwidth_being_utilized_ =
        std::min(bandwidth_being_utilized_, settings_.max_bitrate);

    UpdateEncoderBitrates();
  } else {
    // There is no current bandwidth estimate. So, nothing should be adjusted.
  }

  next_task_.ScheduleFromNow([this] { ControlForNetworkCongestion(); },
                             kCongestionCheckInterval);
}

void LoopingFileSender::SendFileAgain() {
  OSP_LOG_INFO << "Sending " << settings_.path_to_file
               << " (starts in one second)...";
  TRACE_DEFAULT_SCOPED(TraceCategory::kStandaloneSender);

  OSP_CHECK_EQ(num_capturers_running_, 0);
  num_capturers_running_ = 2;
  capture_begin_time_ = latest_frame_time_ = env_.now() + seconds(1);
  audio_capturer_.emplace(
      env_, settings_.path_to_file.c_str(), audio_encoder_.num_channels(),
      audio_encoder_.sample_rate(), capture_begin_time_, *this);
  video_capturer_.emplace(env_, settings_.path_to_file.c_str(),
                          capture_begin_time_, *this);

  next_task_.ScheduleFromNow([this] { ControlForNetworkCongestion(); },
                             kCongestionCheckInterval);
  console_update_task_.Schedule([this] { UpdateStatusOnConsole(); },
                                capture_begin_time_);
}

void LoopingFileSender::OnAudioData(const float* interleaved_samples,
                                    int num_samples,
                                    Clock::time_point capture_begin_time,
                                    Clock::time_point capture_end_time,
                                    Clock::time_point reference_time) {
  TRACE_SCOPED2(TraceCategory::kStandaloneSender, "OnAudioData", "num_samples",
                std::to_string(num_samples), "reference_time",
                ToString(reference_time));
  latest_frame_time_ = std::max(reference_time, latest_frame_time_);
  audio_encoder_.EncodeAndSend(interleaved_samples, num_samples,
                               capture_begin_time, capture_end_time,
                               reference_time);
}

void LoopingFileSender::OnVideoFrame(const AVFrame& av_frame,
                                     Clock::time_point capture_begin_time,
                                     Clock::time_point capture_end_time,
                                     Clock::time_point reference_time) {
  TRACE_SCOPED1(TraceCategory::kStandaloneSender, "OnVideoFrame",
                "reference_time", ToString(reference_time));
  latest_frame_time_ = std::max(reference_time, latest_frame_time_);

  if (use_h264_) {
    // No DrawAnimations here: the click overlay is a mirroring-only feature
    // (active_clicks_ is fed by OnInputMessage, never during file casting), and
    // its 8-bit luma/chroma writes would corrupt a 10-bit (yuv420p10le) frame.
    EncodeVideoFrameH264(av_frame, reference_time);
    return;
  }

  StreamingVideoEncoder::VideoFrame frame{};
  frame.width = av_frame.width - av_frame.crop_left - av_frame.crop_right;
  frame.height = av_frame.height - av_frame.crop_top - av_frame.crop_bottom;

  DrawAnimations(av_frame, frame.width, frame.height);

  frame.yuv_planes[0] = av_frame.data[0] + av_frame.crop_left +
                        av_frame.linesize[0] * av_frame.crop_top;
  frame.yuv_planes[1] = av_frame.data[1] + av_frame.crop_left / 2 +
                        av_frame.linesize[1] * av_frame.crop_top / 2;
  frame.yuv_planes[2] = av_frame.data[2] + av_frame.crop_left / 2 +
                        av_frame.linesize[2] * av_frame.crop_top / 2;
  for (int i = 0; i < 3; ++i) {
    frame.yuv_strides[i] = av_frame.linesize[i];
  }
  frame.capture_begin_time = capture_begin_time;
  frame.capture_end_time = capture_end_time;

  // TODO(jophba): Add performance metrics visual overlay (based on Stats
  // callback).
  video_encoder_->EncodeAndSend(frame, reference_time, {});
}

// Known limitations (shared with the screen path, by design):
//  - The output box is hardcoded to 1920x1080: the negotiated resolution is not
//    read back from the receiver's ANSWER (same simplification as
//    ScreenMirrorSender), so a receiver that answered a smaller box would still
//    get 1080p. The configured TV negotiates 1080p, so this holds today.
//  - H264VaapiEncoder uses constant-QP (qp=20), so SetTargetBitrate() does not
//    hard-cap the output: bitrate is variable and a high-motion film scene may
//    exceed the network budget. Acceptable for LAN casting; a qp/VBR knob would
//    live in the shared encoder, not here.
void LoopingFileSender::EncodeVideoFrameH264(
    const AVFrame& av_frame,
    Clock::time_point reference_time) {
  const int src_w =
      (av_frame.width - av_frame.crop_left - av_frame.crop_right) & ~1;
  const int src_h =
      (av_frame.height - av_frame.crop_top - av_frame.crop_bottom) & ~1;
  if (src_w <= 0 || src_h <= 0) {
    return;
  }

  // Scale down to fit the negotiated 1080p box (the receiver rejects larger
  // frames); never upscale. H.264 is hardware-encoded so 1080p is affordable.
  constexpr int kMaxW = 1920;
  constexpr int kMaxH = 1080;
  const double scale = std::min({1.0, static_cast<double>(kMaxW) / src_w,
                                 static_cast<double>(kMaxH) / src_h});
  const int dst_w = std::max(2, static_cast<int>(src_w * scale) & ~1);
  const int dst_h = std::max(2, static_cast<int>(src_h * scale) & ~1);

  // Use the decoded frame's actual pixel format (e.g. yuv420p10le for 10-bit
  // HEVC) — hardcoding 8-bit YUV420P misreads 10-bit samples and distorts the
  // image. swscale converts any of these to NV12 for the encoder.
  const int src_fmt = av_frame.format;
  if (!sws_ || sws_src_w_ != src_w || sws_src_h_ != src_h ||
      sws_src_fmt_ != src_fmt) {
    if (sws_) {
      sws_freeContext(sws_);
    }
    sws_ = sws_getContext(src_w, src_h, static_cast<AVPixelFormat>(src_fmt),
                          dst_w, dst_h, AV_PIX_FMT_NV12, SWS_LANCZOS, nullptr,
                          nullptr, nullptr);
    sws_src_w_ = src_w;
    sws_src_h_ = src_h;
    sws_src_fmt_ = src_fmt;
  }
  if (!sws_) {
    return;
  }

  // swscale writes in SIMD chunks past each line, so align strides to 32 and
  // add slack to avoid heap overruns. NV12: Y plane then interleaved UV.
  auto align32 = [](int v) { return (v + 31) & ~31; };
  const int y_stride = align32(dst_w);
  const int uv_stride = align32(dst_w);
  const size_t y_size = static_cast<size_t>(y_stride) * dst_h;
  const size_t uv_size = static_cast<size_t>(uv_stride) * (dst_h / 2);
  // Reusable scratch buffer: the encode below is synchronous on this thread, so
  // (unlike the screen path, which posts to another thread) we need no per-frame
  // allocation. resize() only reallocates when the frame size changes.
  nv12_buf_.resize(y_size + uv_size + 64);
  std::vector<uint8_t>& nv12 = nv12_buf_;

  const uint8_t* src[3] = {
      av_frame.data[0] + av_frame.crop_left +
          av_frame.linesize[0] * av_frame.crop_top,
      av_frame.data[1] + av_frame.crop_left / 2 +
          av_frame.linesize[1] * av_frame.crop_top / 2,
      av_frame.data[2] + av_frame.crop_left / 2 +
          av_frame.linesize[2] * av_frame.crop_top / 2};
  int src_stride[3] = {av_frame.linesize[0], av_frame.linesize[1],
                       av_frame.linesize[2]};
  uint8_t* dst[2] = {nv12.data(), nv12.data() + y_size};
  int dst_stride[2] = {y_stride, uv_stride};
  sws_scale(sws_, src, src_stride, 0, src_h, dst, dst_stride);

  // OnVideoFrame already runs on the main TaskRunner (the encoder/Sender
  // thread), so encode synchronously — no PostTask needed (unlike the screen
  // path, whose capturer runs on another thread).
  h264_encoder_->EncodeFrame(nv12.data(), y_stride, nv12.data() + y_size,
                             uv_stride, dst_w, dst_h, reference_time);
}

void LoopingFileSender::UpdateStatusOnConsole() {
  const Clock::duration elapsed = latest_frame_time_ - capture_begin_time_;
  // The control codes here attempt to erase the current line the cursor is
  // on, and then print out the updated status text. If the terminal does not
  // support simple ANSI escape codes, the following will still work, but
  // there might sometimes be old status lines not getting erased (i.e., just
  // partially overwritten).
  OSP_VLOG << "LoopingFileSender: At " << elapsed
           << " in file (est. network bandwidth: " << bandwidth_estimate_ / 1024
           << ").";
  console_update_task_.ScheduleFromNow([this] { UpdateStatusOnConsole(); },
                                       kConsoleUpdateInterval);
}

void LoopingFileSender::DrawAnimations(const AVFrame& av_frame,
                                       int frame_width,
                                       int frame_height) {
  const auto now = env_.now();
  // Remove clicks that have exceeded their 500ms display duration.
  active_clicks_.erase(
      std::remove_if(active_clicks_.begin(), active_clicks_.end(),
                     [now](const Click& c) { return c.end_time < now; }),
      active_clicks_.end());

  if (active_clicks_.empty()) {
    return;
  }

  // We modify the AVFrame data in-place. We use the same base pointer offsets
  // as the encoder to account for any cropping or padding in the original file.
  uint8_t* const y_plane = av_frame.data[0] + av_frame.crop_left +
                           av_frame.linesize[0] * av_frame.crop_top;
  uint8_t* const u_plane = av_frame.data[1] + av_frame.crop_left / 2 +
                           av_frame.linesize[1] * av_frame.crop_top / 2;
  uint8_t* const v_plane = av_frame.data[2] + av_frame.crop_left / 2 +
                           av_frame.linesize[2] * av_frame.crop_top / 2;

  for (const auto& click : active_clicks_) {
    // Map the click coordinates from the receiver's logical display space
    // to the sender's actual video frame resolution.
    const float scale_x = static_cast<float>(frame_width);
    const float scale_y = static_cast<float>(frame_height);
    const int center_x = static_cast<int>(std::round(click.x * scale_x));
    const int center_y = static_cast<int>(std::round(click.y * scale_y));

    // Calculate animation progress (0.0 to 1.0) and the resulting ring radius.
    const double duration = (click.end_time - click.start_time).count();
    const double elapsed = (now - click.start_time).count();
    const double progress = std::clamp(elapsed / duration, 0.0, 1.0);

    const float radius = static_cast<float>(5.0 + 40.0 * progress);
    const float thickness = 3.0f;
    const float inner_radius_sq = std::pow(radius - thickness, 2);
    const float outer_radius_sq = std::pow(radius, 2);

    // Iterate over a bounding box for the ring and draw pixels that fall
    // within the calculated thickness.
    const int box_size = static_cast<int>(radius) + 1;
    for (int dy = -box_size; dy <= box_size; ++dy) {
      for (int dx = -box_size; dx <= box_size; ++dx) {
        const float dist_sq = static_cast<float>(dx * dx + dy * dy);
        if (dist_sq >= inner_radius_sq && dist_sq <= outer_radius_sq) {
          const int px = center_x + dx;
          const int py = center_y + dy;

          if (px >= 0 && px < frame_width && py >= 0 && py < frame_height) {
            // In YUV, pure white is represented by maximum luminance (Y=255)
            // and neutral chrominance (U=128, V=128).
            y_plane[py * av_frame.linesize[0] + px] = 255;

            // Chrominance (U/V) planes are sub-sampled 2x2 in YUV420p.
            if (px % 2 == 0 && py % 2 == 0) {
              const int uv_offset = (py / 2) * av_frame.linesize[1] + (px / 2);
              u_plane[uv_offset] = 128;
              v_plane[uv_offset] = 128;
            }
          }
        }
      }
    }
  }
}

void LoopingFileSender::OnEndOfFile(SimulatedCapturer* capturer) {
  OSP_LOG_INFO << "The " << ToTrackName(capturer)
               << " capturer has reached the end of the media stream.";
  --num_capturers_running_;
  if (num_capturers_running_ == 0) {
    console_update_task_.Cancel();

    if (settings_.should_loop_video) {
      OSP_DLOG_INFO << "Starting the media stream over again.";
      next_task_.Schedule([this] { SendFileAgain(); }, Alarm::kImmediately);
    } else {
      OSP_DLOG_INFO << "Video complete. Exiting...";
      shutdown_callback_();
    }
  }
}

void LoopingFileSender::OnError(SimulatedCapturer* capturer,
                                const std::string& message) {
  OSP_LOG_ERROR << "The " << ToTrackName(capturer)
                << " has failed: " << message;
  --num_capturers_running_;
  // If both fail, the application just pauses. This accounts for things like
  // "file not found" errors. However, if only one track fails, then keep
  // going.
}

const char* LoopingFileSender::ToTrackName(SimulatedCapturer* capturer) const {
  if (capturer == &*audio_capturer_) {
    return "audio";
  } else if (capturer == &*video_capturer_) {
    return "video";
  } else {
    OSP_NOTREACHED();
  }
}

std::unique_ptr<StreamingVideoEncoder> LoopingFileSender::CreateVideoEncoder(
    const StreamingVideoEncoder::Parameters& params,
    TaskRunner& task_runner,
    std::unique_ptr<Sender> sender) {
  switch (params.codec) {
    case VideoCodec::kH264:
      // H.264 is handled by H264VaapiEncoder in the constructor, not here.
      return nullptr;
    case VideoCodec::kVp8:
    case VideoCodec::kVp9:
      return std::make_unique<StreamingVpxEncoder>(params, task_runner,
                                                   std::move(sender));
    case VideoCodec::kAv1:
#if defined(CAST_STANDALONE_SENDER_HAVE_LIBAOM)
      return std::make_unique<StreamingAv1Encoder>(params, task_runner,
                                                   std::move(sender));
#else
      OSP_LOG_FATAL << "AV1 codec selected, but could not be used because "
                       "LibAOM not installed.";
      return nullptr;
#endif
    default:
      // Since we only support VP8, VP9, and AV1, any other codec value here
      // should be due only to developer error.
      OSP_LOG_ERROR << "Unsupported codec " << CodecToString(params.codec);
      OSP_NOTREACHED();
  }
}

}  // namespace openscreen::cast
