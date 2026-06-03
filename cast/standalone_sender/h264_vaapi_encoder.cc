// Copyright 2026. BSD-style license.

#include "cast/standalone_sender/h264_vaapi_encoder.h"

#include <chrono>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
}

#include "cast/streaming/public/encoded_frame.h"
#include "platform/base/span.h"
#include "util/chrono_helpers.h"
#include "util/osp_logging.h"

namespace openscreen::cast {

namespace {
constexpr char kVaapiDevice[] = "/dev/dri/renderD128";
// Constant quantizer for CQP (this driver's only supported RC mode). Lower =
// sharper/higher bitrate; ~20 is visually crisp for desktop text on a LAN.
constexpr int kEncodeQp = 20;
}  // namespace

H264VaapiEncoder::H264VaapiEncoder(std::unique_ptr<Sender> sender,
                                   int max_bitrate)
    : sender_(std::move(sender)), target_bitrate_(max_bitrate) {}

H264VaapiEncoder::~H264VaapiEncoder() {
  Teardown();
}

void H264VaapiEncoder::SetTargetBitrate(int bitrate) {
  // Applied at (re)initialization; mid-stream VAAPI bitrate changes are not
  // reconfigured in this first cut.
  target_bitrate_ = bitrate;
}

void H264VaapiEncoder::Teardown() {
  if (packet_) {
    av_packet_free(&packet_);
  }
  if (hw_frame_) {
    av_frame_free(&hw_frame_);
  }
  if (ctx_) {
    avcodec_free_context(&ctx_);
  }
  if (hw_frames_ctx_) {
    av_buffer_unref(&hw_frames_ctx_);
  }
  if (hw_device_ctx_) {
    av_buffer_unref(&hw_device_ctx_);
  }
  enc_width_ = enc_height_ = 0;
}

bool H264VaapiEncoder::EnsureInitialized(int width, int height) {
  if (ctx_ && width == enc_width_ && height == enc_height_) {
    return true;
  }
  if (ctx_) {
    Teardown();
  }

  int err = av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_VAAPI,
                                   kVaapiDevice, nullptr, 0);
  if (err < 0) {
    OSP_LOG_ERROR << "H264VaapiEncoder: av_hwdevice_ctx_create failed: " << err;
    return false;
  }

  const AVCodec* codec = avcodec_find_encoder_by_name("h264_vaapi");
  if (!codec) {
    OSP_LOG_ERROR << "H264VaapiEncoder: h264_vaapi encoder not found";
    Teardown();
    return false;
  }

  ctx_ = avcodec_alloc_context3(codec);
  if (!ctx_) {
    Teardown();
    return false;
  }
  ctx_->width = width;
  ctx_->height = height;
  ctx_->pix_fmt = AV_PIX_FMT_VAAPI;
  // RTP-style microsecond timeline; pts are real elapsed microseconds.
  ctx_->time_base = AVRational{1, 1000000};
  ctx_->framerate = AVRational{60, 1};
  ctx_->bit_rate = target_bitrate_;
  ctx_->rc_max_rate = target_bitrate_;
  ctx_->rc_buffer_size = target_bitrate_;  // ~1s buffer
  ctx_->max_b_frames = 0;                  // no reordering -> low latency
  ctx_->gop_size = 120;                    // rare auto-IDR; PLI forces on demand
  ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;

  // Build the NV12 VAAPI frame pool the encoder draws from.
  hw_frames_ctx_ = av_hwframe_ctx_alloc(hw_device_ctx_);
  if (!hw_frames_ctx_) {
    Teardown();
    return false;
  }
  auto* fctx = reinterpret_cast<AVHWFramesContext*>(hw_frames_ctx_->data);
  fctx->format = AV_PIX_FMT_VAAPI;
  fctx->sw_format = AV_PIX_FMT_NV12;
  fctx->width = width;
  fctx->height = height;
  fctx->initial_pool_size = 8;
  err = av_hwframe_ctx_init(hw_frames_ctx_);
  if (err < 0) {
    OSP_LOG_ERROR << "H264VaapiEncoder: av_hwframe_ctx_init failed: " << err;
    Teardown();
    return false;
  }
  ctx_->hw_frames_ctx = av_buffer_ref(hw_frames_ctx_);

  // This iGPU's VAAPI driver only supports CQP (constant-QP) rate control for
  // H.264 — not CBR/VBR. A low QP keeps desktop text/edges sharp; the bitrate
  // floats with content (capped downstream by in-flight pacing on the LAN).
  AVDictionary* opts = nullptr;
  av_dict_set(&opts, "low_power", "1", 0);  // use the LP entrypoint (EncSliceLP)
  av_dict_set(&opts, "rc_mode", "CQP", 0);
  av_dict_set_int(&opts, "qp", kEncodeQp, 0);
  err = avcodec_open2(ctx_, codec, &opts);
  av_dict_free(&opts);
  if (err < 0) {
    // Retry without low_power, in case the LP entrypoint rejects the config.
    OSP_LOG_WARN << "H264VaapiEncoder: open with low_power failed (" << err
                 << "), retrying without it";
    avcodec_free_context(&ctx_);
    ctx_ = avcodec_alloc_context3(codec);
    ctx_->width = width;
    ctx_->height = height;
    ctx_->pix_fmt = AV_PIX_FMT_VAAPI;
    ctx_->time_base = AVRational{1, 1000000};
    ctx_->framerate = AVRational{60, 1};
    ctx_->bit_rate = target_bitrate_;
    ctx_->rc_max_rate = target_bitrate_;
    ctx_->rc_buffer_size = target_bitrate_;
    ctx_->max_b_frames = 0;
    ctx_->gop_size = 120;
    ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx_->hw_frames_ctx = av_buffer_ref(hw_frames_ctx_);
    // Keep CQP + explicit QP (drop only low_power): without rc_mode the driver
    // picks a high default QP that softens the image.
    AVDictionary* retry_opts = nullptr;
    av_dict_set(&retry_opts, "rc_mode", "CQP", 0);
    av_dict_set_int(&retry_opts, "qp", kEncodeQp, 0);
    err = avcodec_open2(ctx_, codec, &retry_opts);
    av_dict_free(&retry_opts);
    if (err < 0) {
      OSP_LOG_ERROR << "H264VaapiEncoder: avcodec_open2 failed: " << err;
      Teardown();
      return false;
    }
  }

  hw_frame_ = av_frame_alloc();
  packet_ = av_packet_alloc();
  if (!hw_frame_ || !packet_) {
    Teardown();
    return false;
  }
  enc_width_ = width;
  enc_height_ = height;
  OSP_LOG_INFO << "H264VaapiEncoder: initialized " << width << "x" << height
               << " @ " << target_bitrate_ << " bps (VAAPI)";
  return true;
}

void H264VaapiEncoder::EncodeFrame(const uint8_t* y,
                                   int y_stride,
                                   const uint8_t* uv,
                                   int uv_stride,
                                   int width,
                                   int height,
                                   Clock::time_point reference_time) {
  if (!EnsureInitialized(width, height)) {
    return;
  }

  // Compute the RTP timestamp from the reference timeline.
  RtpTimeTicks rtp_timestamp;
  if (start_time_ == Clock::time_point::min()) {
    start_time_ = reference_time;
    rtp_timestamp = RtpTimeTicks();
  } else {
    rtp_timestamp = RtpTimeTicks::FromTimeSinceOrigin(
        reference_time - start_time_, sender_->config().rtp_timebase);
    if (rtp_timestamp <= last_rtp_timestamp_) {
      return;  // Not monotonically increasing; drop.
    }
  }
  if (sender_->GetInFlightMediaDuration(rtp_timestamp) >
      sender_->GetMaxInFlightMediaDuration()) {
    return;  // Receiver would be over its in-flight limit; drop.
  }

  // Wrap the incoming NV12 planes in a software frame (no copy) and upload it
  // to a hardware surface.
  AVFrame* sw = av_frame_alloc();
  sw->format = AV_PIX_FMT_NV12;
  sw->width = width;
  sw->height = height;
  sw->data[0] = const_cast<uint8_t*>(y);
  sw->linesize[0] = y_stride;
  sw->data[1] = const_cast<uint8_t*>(uv);
  sw->linesize[1] = uv_stride;

  av_frame_unref(hw_frame_);
  int err = av_hwframe_get_buffer(hw_frames_ctx_, hw_frame_, 0);
  if (err < 0) {
    av_frame_free(&sw);
    return;
  }
  err = av_hwframe_transfer_data(hw_frame_, sw, 0);
  av_frame_free(&sw);  // Only frees the AVFrame struct; planes are borrowed.
  if (err < 0) {
    OSP_LOG_ERROR << "H264VaapiEncoder: hwframe transfer failed: " << err;
    return;
  }

  hw_frame_->pts =
      to_microseconds(reference_time - start_time_).count();
  hw_frame_->pict_type =
      sender_->NeedsKeyFrame() ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;

  err = avcodec_send_frame(ctx_, hw_frame_);
  if (err < 0) {
    OSP_LOG_ERROR << "H264VaapiEncoder: send_frame failed: " << err;
    return;
  }

  while (true) {
    err = avcodec_receive_packet(ctx_, packet_);
    if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
      break;
    }
    if (err < 0) {
      OSP_LOG_ERROR << "H264VaapiEncoder: receive_packet failed: " << err;
      break;
    }

    const bool is_key = (packet_->flags & AV_PKT_FLAG_KEY) != 0;
    EncodedFrame frame;
    frame.frame_id = sender_->GetNextFrameId();
    if (is_key) {
      frame.dependency = EncodedFrame::Dependency::kKeyFrame;
      frame.referenced_frame_id = frame.frame_id;
    } else {
      frame.dependency = EncodedFrame::Dependency::kDependent;
      frame.referenced_frame_id = frame.frame_id - 1;
    }
    frame.rtp_timestamp = rtp_timestamp;
    frame.reference_time = reference_time;
    frame.capture_begin_time = reference_time;
    frame.capture_end_time = reference_time;
    frame.data = ByteView(packet_->data, static_cast<size_t>(packet_->size));

    const Sender::EnqueueFrameResult result = sender_->EnqueueFrame(frame);
    if (result != Sender::OK) {
      OSP_LOG_WARN << "H264VaapiEncoder: EnqueueFrame rejected (" << result
                   << ")";
    }
    last_rtp_timestamp_ = rtp_timestamp;
    av_packet_unref(packet_);
  }
}

}  // namespace openscreen::cast
