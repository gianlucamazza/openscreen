// Copyright 2021 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_STANDALONE_SENDER_CONNECTION_SETTINGS_H_
#define CAST_STANDALONE_SENDER_CONNECTION_SETTINGS_H_

#include <string>

#include "cast/streaming/public/constants.h"
#include "platform/base/interface_info.h"

namespace openscreen::cast {

// The connection settings for a given standalone sender instance. These fields
// are used throughout the standalone sender component to initialize state from
// the command line parameters.
struct ConnectionSettings {
  // The endpoint of the receiver we wish to connect to.
  IPEndpoint receiver_endpoint;

  // The path to the file that we want to play.
  std::string path_to_file;

  // The maximum bitrate. Default value means a reasonable default will be
  // selected.
  int max_bitrate = 0;

  // Whether the stream should include video, or just be audio only.
  bool should_include_video = true;

  // Whether we should use the hacky RTP stream IDs for legacy android
  // receivers, or if we should use the proper values. For more information,
  // see issuetracker.google.com/184438154.
  bool use_android_rtp_hack = true;

  // Whether we should use remoting for the video, instead of the default of
  // mirroring.
  bool use_remoting = false;

  // Diagnostic mode: after the mirroring app session is established, send a
  // GET_CAPABILITIES request, log the receiver's RemotingCapabilities, and exit
  // without negotiating a stream. Used to probe what a given receiver advertises
  // for remoting (codecs, 4k) before building the remoting content path.
  bool probe_caps = false;

  // Whether we should loop the video when it is completed.
  bool should_loop_video = true;

  // The codec to use for encoding negotiated video streams.
  VideoCodec codec;

  // Whether DSCP support should be enabled for Quality of Service.
  bool enable_dscp = true;

  // Whether input event API should be enabled for this session.
  bool enable_input_events = false;

  // For window mirroring: capture only the audio of this process (and its
  // descendants) instead of the whole system mix. 0 = system mix (default).
  int audio_target_pid = 0;

  // For window mirroring: also match the app's audio node by application name /
  // binary (case-insensitive). Catches apps (e.g. browsers) whose PipeWire node
  // resolves to a launcher PID via pipewire.sec.pid rather than the window PID.
  std::string audio_target_app;
};

}  // namespace openscreen::cast

#endif  // CAST_STANDALONE_SENDER_CONNECTION_SETTINGS_H_
