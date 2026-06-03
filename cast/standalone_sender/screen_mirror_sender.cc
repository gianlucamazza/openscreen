// Copyright 2026. BSD-style license.

#include "cast/standalone_sender/screen_mirror_sender.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <utility>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include "cast/standalone_sender/constants.h"
#include "cast/standalone_sender/streaming_vpx_encoder.h"
#include "util/osp_logging.h"

namespace openscreen::cast {

using std::chrono::microseconds;
using std::chrono::milliseconds;

namespace {

// wl_shm format codes (see wayland.xml): ARGB8888 = 0, XRGB8888 = 1.
constexpr uint32_t kWlShmArgb8888 = 0;
constexpr uint32_t kWlShmXrgb8888 = 1;

int WlShmToAvPixfmt(uint32_t shm_format) {
  switch (shm_format) {
    case kWlShmXrgb8888:
      return AV_PIX_FMT_BGR0;  // little-endian XRGB == bytes B,G,R,X
    case kWlShmArgb8888:
    default:
      return AV_PIX_FMT_BGRA;  // little-endian ARGB == bytes B,G,R,A
  }
}

// Audio: 10 ms of stereo silence per tick at 48 kHz.
constexpr int kAudioTickMs = 10;

// Parse the capture target string into (kind, selector):
//   "screen" / "screen:<output>"  -> capture an output (monitor)
//   "window" / "window:<selector>"-> capture a foreign toplevel (window)
// The selector is whatever follows the colon (empty if none).
struct CaptureTarget {
  ScreenCapturer::Target kind;
  std::string selector;  // output name, or app-id/title (fallback)
  uint64_t address = 0;  // Hyprland window address ("window:addr=<hex|dec>")
};

CaptureTarget ParseCaptureTarget(const std::string& path) {
  const ScreenCapturer::Target kind = (path.rfind("window", 0) == 0)
                                          ? ScreenCapturer::Target::kToplevel
                                          : ScreenCapturer::Target::kOutput;
  const auto colon = path.find(':');
  std::string arg = (colon == std::string::npos) ? "" : path.substr(colon + 1);
  uint64_t address = 0;
  std::string selector = arg;
  if (arg.rfind("addr=", 0) == 0) {
    // Hyprland window address; accept 0x-hex or decimal. strtoull(base=0).
    address = std::strtoull(arg.c_str() + 5, nullptr, 0);
    selector.clear();
  }
  return {kind, std::move(selector), address};
}

}  // namespace

ScreenMirrorSender::ScreenMirrorSender(
    Environment& environment,
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
      audio_task_(env_.now_function(), env_.task_runner()) {
  OSP_CHECK(senders.audio_config.codec == AudioCodec::kOpus);
  OSP_CHECK(senders.video_config.codec == VideoCodec::kVp8 ||
            senders.video_config.codec == VideoCodec::kVp9 ||
            senders.video_config.codec == VideoCodec::kH264);

  // Hardware H.264 (VAAPI) when negotiated; otherwise the software libvpx path.
  use_h264_ = (senders.video_config.codec == VideoCodec::kH264);
  if (use_h264_) {
    h264_encoder_ = std::make_unique<H264VaapiEncoder>(
        std::move(senders.video_sender), settings_.max_bitrate);
  } else {
    video_encoder_ = CreateVideoEncoder(
        StreamingVideoEncoder::Parameters{.codec = settings_.codec},
        env_.task_runner(), std::move(senders.video_sender));
  }

  bandwidth_being_utilized_ = settings_.max_bitrate / 2;
  UpdateEncoderBitrates();

  const CaptureTarget target = ParseCaptureTarget(settings_.path_to_file);
  const bool is_window = target.kind == ScreenCapturer::Target::kToplevel;
  window_mode_ = is_window;
  OSP_LOG_INFO << "ScreenMirrorSender: starting capture of "
               << (is_window ? "window " : "output ")
               << (target.address ? ("addr=" + std::to_string(target.address))
                   : target.selector.empty() ? "<first>"
                                              : target.selector);

  capturer_.Start(target.kind, target.selector, target.address,
                  [this](const ScreenCapturer::Frame& f) { OnCapturedFrame(f); });

  next_task_.ScheduleFromNow([this] { ControlForNetworkCongestion(); },
                             kCongestionCheckInterval);

  // Audio. Window mirror with a target PID → capture only that app's audio;
  // otherwise the whole desktop (default sink monitor). Either way fall back to
  // a silent track (the receiver expects an audio stream) if capture can't run.
  per_app_audio_ = settings_.audio_target_pid > 0;
  auto on_audio = [this](const float* s, int n, int ch) {
    OnCapturedAudio(s, n, ch);
  };
  if (per_app_audio_) {
    const std::set<int> pids{settings_.audio_target_pid};
    real_audio_active_ = audio_capturer_.StartForPids(
        audio_encoder_.num_channels(), audio_encoder_.sample_rate(), pids,
        on_audio);
    OSP_LOG_INFO << "ScreenMirrorSender: per-app audio for pid "
                 << settings_.audio_target_pid;
    // Per-app audio is intermittent (silent app, or stream not yet present), so
    // always run the gap-filler to keep the audio track continuous.
    audio_task_.Schedule([this] { FillAudioGap(); }, Alarm::kImmediately);
  } else {
    real_audio_active_ = audio_capturer_.Start(
        audio_encoder_.num_channels(), audio_encoder_.sample_rate(), on_audio);
    if (!real_audio_active_) {
      OSP_LOG_WARN << "ScreenMirrorSender: PipeWire capture unavailable; "
                      "sending silence";
      audio_task_.Schedule([this] { SendSilentAudio(); }, Alarm::kImmediately);
    }
  }
}

ScreenMirrorSender::~ScreenMirrorSender() {
  capturer_.Stop();
  audio_capturer_.Stop();
  if (sws_) {
    sws_freeContext(sws_);
  }
}

std::unique_ptr<StreamingVideoEncoder> ScreenMirrorSender::CreateVideoEncoder(
    const StreamingVideoEncoder::Parameters& params,
    TaskRunner& task_runner,
    std::unique_ptr<Sender> sender) {
  return std::make_unique<StreamingVpxEncoder>(params, task_runner,
                                               std::move(sender));
}

void ScreenMirrorSender::UpdateEncoderBitrates() {
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

void ScreenMirrorSender::ControlForNetworkCongestion() {
  bandwidth_estimate_ = session_->GetEstimatedNetworkBandwidth();
  if (bandwidth_estimate_ > 0) {
    constexpr double kGoodNetworkCitizenFactor = 0.8;
    const int usable_bandwidth = std::max<int>(
        kGoodNetworkCitizenFactor * bandwidth_estimate_, kMinRequiredBitrate);
    if (usable_bandwidth > bandwidth_being_utilized_) {
      constexpr double kConservativeIncrease = 1.1;
      bandwidth_being_utilized_ = std::min<int>(
          bandwidth_being_utilized_ * kConservativeIncrease, usable_bandwidth);
    } else {
      bandwidth_being_utilized_ = usable_bandwidth;
    }
    bandwidth_being_utilized_ =
        std::min(bandwidth_being_utilized_, settings_.max_bitrate);
    UpdateEncoderBitrates();
  }
  next_task_.ScheduleFromNow([this] { ControlForNetworkCongestion(); },
                             kCongestionCheckInterval);
}

void ScreenMirrorSender::SendSilentAudio() {
  const int channels = audio_encoder_.num_channels();
  const int rate = audio_encoder_.sample_rate();
  const int frames = rate * kAudioTickMs / 1000;  // e.g. 480 @ 48 kHz
  const std::vector<float> silence(static_cast<size_t>(frames) * channels,
                                   0.0f);

  const Clock::time_point now = env_.now();
  if (!audio_started_) {
    audio_reference_time_ = now;
    audio_started_ = true;
  }
  audio_encoder_.EncodeAndSend(silence.data(), frames, now, now,
                               audio_reference_time_);
  audio_reference_time_ += milliseconds(kAudioTickMs);
  audio_task_.ScheduleFromNow([this] { SendSilentAudio(); },
                              milliseconds(kAudioTickMs));
}

void ScreenMirrorSender::OnCapturedFrame(const ScreenCapturer::Frame& frame) {
  // Runs on the ScreenCapturer thread. Convert (and downscale) here, then post
  // the encode onto the main task runner (encoder/Sender are not thread-safe).
  const int src_w = frame.width & ~1;
  const int src_h = frame.height & ~1;
  if (src_w <= 0 || src_h <= 0) {
    return;
  }
  const int src_fmt = WlShmToAvPixfmt(frame.shm_format);

  // Encode within a box that never exceeds the negotiated max (1920x1080): the
  // receiver rejects frames larger than the OFFER (our panel is 1920x1200).
  // H.264 is hardware-encoded, so we can afford full 1080p (sharp); the
  // software VP8 path stays at 720p to keep encode latency down.
  const int max_w = use_h264_ ? 1920 : 1280;
  const int max_h = use_h264_ ? 1080 : 720;
  // Output mirror / VP8: scale down to fit, never up. Window mirror (H.264):
  // scale to *fill* the box (upscaling small windows) so the TV receives a full
  // 1080p frame and shows it 1:1 — no soft TV-side upscale. Aspect is preserved
  // here and the leftover area is letterboxed below.
  const bool letterbox = use_h264_ && window_mode_;
  const double scale =
      letterbox ? std::min(static_cast<double>(max_w) / src_w,
                           static_cast<double>(max_h) / src_h)
                : std::min({1.0, static_cast<double>(max_w) / src_w,
                            static_cast<double>(max_h) / src_h});
  const int w = std::max(2, static_cast<int>(src_w * scale) & ~1);
  const int h = std::max(2, static_cast<int>(src_h * scale) & ~1);

  const AVPixelFormat dst_fmt =
      use_h264_ ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
  if (!sws_ || sws_src_format_ != src_fmt || sws_w_ != src_w ||
      sws_h_ != src_h) {
    if (sws_) {
      sws_freeContext(sws_);
    }
    sws_ = sws_getContext(src_w, src_h, static_cast<AVPixelFormat>(src_fmt), w,
                          h, dst_fmt, SWS_LANCZOS, nullptr, nullptr, nullptr);
    sws_src_format_ = src_fmt;
    sws_w_ = src_w;
    sws_h_ = src_h;
  }
  if (!sws_) {
    return;
  }

  // swscale writes in SIMD chunks and may touch a few bytes past each line, so
  // align strides to 32 and add slack to avoid heap overruns.
  auto align32 = [](int v) { return (v + 31) & ~31; };
  const Clock::time_point now = Clock::now();

  if (use_h264_) {
    // Output frame size: the scaled content itself (output mirror) or the full
    // box with the content centered + black bars (window mirror).
    const int out_w = letterbox ? max_w : w;
    const int out_h = letterbox ? max_h : h;
    // NV12: Y plane, then interleaved UV plane (half height, full byte-width).
    const int y_stride = align32(out_w);
    const int uv_stride = align32(out_w);
    const size_t y_size = static_cast<size_t>(y_stride) * out_h;
    const size_t uv_size = static_cast<size_t>(uv_stride) * (out_h / 2);
    auto nv12 = std::make_shared<std::vector<uint8_t>>(y_size + uv_size + 64);

    int ox = 0, oy = 0;
    if (letterbox) {
      ox = ((out_w - w) / 2) & ~1;
      oy = ((out_h - h) / 2) & ~1;
      std::memset(nv12->data(), 16, y_size);              // black luma (limited)
      std::memset(nv12->data() + y_size, 128, uv_size);   // neutral chroma
    }
    // swscale writes the w x h content into the (centered) sub-rectangle.
    uint8_t* dst[2] = {nv12->data() + oy * y_stride + ox,
                       nv12->data() + y_size + (oy / 2) * uv_stride + ox};
    int dst_stride[2] = {y_stride, uv_stride};
    const uint8_t* src[1] = {frame.data};
    int src_stride[1] = {frame.stride};
    sws_scale(sws_, src, src_stride, 0, src_h, dst, dst_stride);

    env_.task_runner().PostTask(
        [weak = weak_factory_.GetWeakPtr(), nv12, out_w, out_h, y_size, y_stride,
         uv_stride, now] {
          ScreenMirrorSender* self = weak.get();
          if (!self) {
            return;
          }
          self->h264_encoder_->EncodeFrame(nv12->data(), y_stride,
                                           nv12->data() + y_size, uv_stride,
                                           out_w, out_h, now);
        });
    return;
  }

  // libvpx I420 path: Y + U + V planes.
  Clock::duration duration = milliseconds(16);
  if (last_video_time_ != Clock::time_point{}) {
    duration = std::clamp<Clock::duration>(now - last_video_time_,
                                           milliseconds(1), milliseconds(100));
  }
  last_video_time_ = now;

  const int y_stride = align32(w);
  const int uv_stride = align32(w / 2);
  const size_t y_size = static_cast<size_t>(y_stride) * h;
  const size_t uv_size = static_cast<size_t>(uv_stride) * (h / 2);
  auto i420 =
      std::make_shared<std::vector<uint8_t>>(y_size + 2 * uv_size + 64);

  uint8_t* dst[3] = {i420->data(), i420->data() + y_size,
                     i420->data() + y_size + uv_size};
  int dst_stride[3] = {y_stride, uv_stride, uv_stride};
  const uint8_t* src[1] = {frame.data};
  int src_stride[1] = {frame.stride};
  sws_scale(sws_, src, src_stride, 0, src_h, dst, dst_stride);

  // Capture a WeakPtr (not `this`): the encode runs on the main task runner,
  // but this object may be destroyed (Shutdown on socket error) while frame
  // tasks are still queued. If so, the WeakPtr is invalid and we drop the frame.
  env_.task_runner().PostTask(
      [weak = weak_factory_.GetWeakPtr(), i420, w, h, y_size, uv_size, y_stride,
       uv_stride, now, duration] {
        ScreenMirrorSender* self = weak.get();
        if (!self) {
          return;
        }
        StreamingVideoEncoder::VideoFrame vf{};
        vf.width = w;
        vf.height = h;
        vf.yuv_planes[0] = i420->data();
        vf.yuv_planes[1] = i420->data() + y_size;
        vf.yuv_planes[2] = i420->data() + y_size + uv_size;
        vf.yuv_strides[0] = y_stride;
        vf.yuv_strides[1] = uv_stride;
        vf.yuv_strides[2] = uv_stride;
        vf.duration = duration;
        vf.capture_begin_time = now;
        vf.capture_end_time = now;
        self->video_encoder_->EncodeAndSend(vf, now, {});
      });
}

void ScreenMirrorSender::OnCapturedAudio(const float* samples, int num_frames,
                                         int channels) {
  // Runs on the AudioCapturer (PipeWire) thread. Copy the interleaved samples
  // and post the Opus encode onto the main task runner (encoder not
  // thread-safe). The encoder buffers and re-frames into 10 ms Cast frames.
  const Clock::time_point now = Clock::now();
  auto pcm = std::make_shared<std::vector<float>>(
      samples, samples + static_cast<size_t>(num_frames) * channels);
  env_.task_runner().PostTask(
      [weak = weak_factory_.GetWeakPtr(), pcm, num_frames, now] {
        ScreenMirrorSender* self = weak.get();
        if (!self) {
          return;
        }
        self->EmitAudioFrames(pcm->data(), num_frames, now, /*is_real=*/true);
      });
}

void ScreenMirrorSender::EmitAudioFrames(const float* pcm, int num_frames,
                                         Clock::time_point now, bool is_real) {
  // Runs on the main task runner. Derive a continuous reference time from the
  // running sample count (anchored at the first buffer), not from per-buffer
  // wall-clock, so the encoder sees a gapless timeline. PipeWire delivers
  // slightly under real-time, so re-anchor whenever the sample clock drifts too
  // far from wall-clock: this caps end-to-end latency (one occasional skip)
  // instead of letting a steady deficit accumulate into seconds of lag. Real
  // app audio and gap-filler silence share this one timeline.
  if (is_real) {
    last_real_audio_time_ = now;
  }
  const int rate = audio_encoder_.sample_rate();
  if (audio_epoch_ == Clock::time_point{}) {
    audio_epoch_ = now;
  }
  Clock::time_point ref =
      audio_epoch_ + microseconds(audio_samples_sent_ * 1000000 / rate);
  if (now - ref > milliseconds(200) || ref - now > milliseconds(200)) {
    audio_epoch_ = now - microseconds(audio_samples_sent_ * 1000000 / rate);
    ref = now;
  }
  audio_encoder_.EncodeAndSend(pcm, num_frames, ref, ref, ref);
  audio_samples_sent_ += num_frames;
}

void ScreenMirrorSender::FillAudioGap() {
  // If real app audio arrived within the last ~60 ms, the app is producing
  // sound and the captured buffers carry the timeline; do nothing.
  const Clock::time_point now = env_.now();
  constexpr Clock::duration kRealAudioFreshness = milliseconds(60);
  if (last_real_audio_time_ == Clock::time_point{} ||
      now - last_real_audio_time_ > kRealAudioFreshness) {
    const int channels = audio_encoder_.num_channels();
    const int rate = audio_encoder_.sample_rate();
    if (audio_epoch_ == Clock::time_point{}) {
      audio_epoch_ = now;
    }
    // Emit exactly enough silence to catch the sample clock up to wall-clock.
    // A fixed 10 ms/tick drifts when this Alarm is delayed by the video encode
    // on the same task runner (the deficit accumulates until the 200 ms
    // re-anchor fires, which the Opus gap-detector logs as a skip). Filling by
    // elapsed time keeps the timeline real-time regardless of Alarm jitter.
    const int64_t expected =
        std::chrono::duration_cast<microseconds>(now - audio_epoch_).count() *
        rate / 1000000;
    int64_t needed = expected - audio_samples_sent_;
    const int64_t kMaxBurst = rate / 5;  // cap a long stall at 200 ms of silence
    needed = std::clamp<int64_t>(needed, 0, kMaxBurst);
    if (needed > 0) {
      const std::vector<float> silence(
          static_cast<size_t>(needed) * channels, 0.0f);
      EmitAudioFrames(silence.data(), static_cast<int>(needed), now,
                      /*is_real=*/false);
    }
  }
  audio_task_.ScheduleFromNow([this] { FillAudioGap(); },
                              milliseconds(kAudioTickMs));
}

}  // namespace openscreen::cast
