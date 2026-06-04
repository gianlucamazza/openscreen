# ADR 0001: Native castbridge Backend for Firefox Cast

## Status

Proposed

## Context

This fork adds a native Linux backend for a Firefox Cast extension on top of
Open Screen. The backend has two linked pieces:

- `cast/castbridge`: a native-messaging relay plus daemon that handles browser
  IPC, device discovery, media control, YouTube control, and mirroring requests.
- `cast/standalone_sender`: the native Cast Streaming sender used for
  Wayland/Hyprland screen and window mirroring with PipeWire audio and VAAPI
  H.264.

The fork is built standalone on Linux with system libraries (`use_sysroot=false`)
instead of Chromium's sysroot. This keeps the integration practical for local
desktop use, but it also means the fork owns product-level concerns that upstream
Open Screen examples normally do not: native-messaging framing, daemon lifecycle,
AF_UNIX IPC, subprocess management, receiver selection, and desktop capture
dependencies.

An audit of the current fork found that `castbridge` and `cast_sender` build and
link successfully, and that the daemon can discover a real Cast receiver on the
local network. The same audit also found remaining product-readiness gaps:

- IPC responses can be lost for one-shot clients that write a request and close
  the connection immediately.
- The daemon shutdown path uses a signal handler and a detached stop thread that
  need stricter lifecycle handling.
- Both native binaries are still declared as `testonly`.
- `cast_sender --help` does not document the new H.264, screen/window, and
  `--audio-pid` behavior completely.
- `castbridge` has no dedicated tests.
- The media and YouTube Cast clients duplicate channel/session setup logic.

A follow-up audit (answering "were the fork's additions necessary or duplicated?")
confirmed this decision and added three findings:

- **No upstream duplication.** None of the castbridge components duplicate Open
  Screen. Discovery/media controllers are thin wrappers over existing APIs; the
  media client, MDX handshake, Lounge HTTP layer, native-messaging relay and IPC
  daemon have no upstream equivalent. The additions were necessary.
- **The two Cast clients were the one real internal duplication.** `media_receiver_client`
  and `youtube_cast_client` shared ~150 lines of channel setup (socket factory,
  router, namespace handler, launch/heartbeat handshake, RECEIVER_STATUS, volume,
  teardown). This has been extracted into a shared `CastChannelClient` base.
- **`window_resolver` is not a removable duplicate.** It overlaps with
  `ScreenCapturer`'s app-id/title matching, but it also resolves the window PID
  (required for `--audio-pid`, which ScreenCapturer cannot provide) and produces
  the canonical Hyprland *address* the daemon drives the sender by. The benign
  overlap is documented; ScreenCapturer now refuses to capture an arbitrary
  window when an explicit address is unmapped (avoids silent daemon↔sender
  divergence).

## Decision

Keep `castbridge` as the fork's native backend for the Firefox extension, and
keep `cast_sender` as the native mirroring implementation that `castbridge`
orchestrates.

The browser extension should continue to speak a small JSON command protocol to
the native backend. It should not own Open Screen objects, Cast channel state,
desktop capture, or long-running subprocess lifecycle. Those responsibilities
belong in the native backend because they depend on Open Screen threading rules,
Linux desktop APIs, and receiver-specific Cast behavior.

The native backend remains fork-local and Linux-focused. It is not designed as an
upstream Open Screen API surface until the daemon lifecycle, IPC contract, tests,
and build target classification are hardened.

## Consequences

This decision makes the fork coherent as a local product path: Open Screen
continues to provide Cast discovery, Cast channel, and streaming primitives,
while the fork adds a desktop-oriented backend around them.

It also makes `castbridge` a product surface, not just an experiment. The daemon
must be treated as responsible for:

- stable JSON request/response semantics;
- safe behavior when clients disconnect early;
- clean signal and shutdown handling;
- predictable session state for media, YouTube, and mirroring;
- subprocess lifecycle for `cast_sender`, `curl`, and Hyprland tools;
- test coverage for IPC and controller behavior.

The current implementation should remain `Proposed` until the product-readiness
gaps above are resolved or explicitly accepted by the maintainer.

## Validation

The audit established the following current evidence:

- `autoninja -C out/Default cast_sender castbridge` completed successfully.
- `castbridge --daemon` started discovery on the active wireless interface.
- The daemon discovered a Cast receiver named `43PUS9235/12` at
  `192.168.1.228`.
- JSON IPC responses work when the client keeps the AF_UNIX connection open.
- Some one-shot IPC clients receive zero bytes if they close immediately after
  writing a request. (Fixed since — see Follow-up.)
- `openscreen_unittests` were not completed during the audit.

## Follow-up

Before this ADR can move from `Proposed` to `Accepted`, the fork should:

- remove or justify `testonly` for native product binaries;
- update `cast_sender --help` to match the fork's actual CLI (it omits H.264 in
  `-c`, the `--audio-pid` option, and the `screen`/`window` mirror targets);
- add controller-level tests for `castbridge` (media/youtube/mirror), building on
  the IPC tests below.

Done since this ADR was first drafted:

- **[done] Hardened the daemon shutdown lifecycle.** The signal handler was
  already async-signal-safe; the real issue — a use-after-free if a signal hit
  during teardown — is fixed by making the global `IpcServer*` atomic and
  clearing it right after `Run()` returns so the handler no-ops during shutdown.
- **[done] Fixed IPC request drop / response loss for one-shot clients.** The
  poll loop closed a connection on POLLHUP/EOF before dispatching the buffered
  request; half-closed clients also lost late (async) responses. It now dispatches
  buffered requests before teardown and keeps a half-closed connection open to
  deliver the reply. Covered by `castbridge_unittests` (`ipc_server_unittest.cc`):
  round-trip, half-close async response, dispatch-on-immediate-close, pipelining,
  fragmented frames.
- **[done] Reduced Cast-client duplication** by extracting `CastChannelClient`
  (`cast/castbridge/cast_channel_client.{h,cc}`); `media_receiver_client` and
  `youtube_cast_client` now derive from it.
- **[done] Hardened mirror lifecycle.** `mirror_controller` no longer scrapes the
  sender log for "Streaming to "; `cast_sender` reports "streaming"/"failed" on a
  dedicated status pipe (`CASTBRIDGE_STATUS_FD`). The sender is spawned in its own
  process group and signalled as a group on stop.
- **[confirmed] Subprocess mirroring kept.** Spawning `cast_sender` (rather than
  linking it in-process) is retained deliberately: it isolates encoder/VAAPI
  crashes and avoids the libcurl↔boringssl symbol clash. In-process linking
  remains a documented, non-urgent alternative.
