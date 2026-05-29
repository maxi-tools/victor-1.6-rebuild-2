/**
 * File: BehaviorVisageRender
 *
 * Copyright: Maxi Tools 2026
 **/

#include "engine/aiComponent/behaviorComponent/behaviors/animationWrappers/behaviorVisageRender.h"

#include "clad/externalInterface/messageGameToEngine.h"
#include "clad/robotInterface/messageRobotToEngine.h"
#include "clad/robotInterface/messageEngineToRobot.h"
#include "clad/types/proceduralFaceTypes.h"

#include "engine/aiComponent/behaviorComponent/behaviorExternalInterface/behaviorExternalInterface.h"
#include "engine/components/animationComponent.h"
#include "engine/robot.h"
#include "engine/robotInterface/messageHandler.h"
#include "engine/aiComponent/behaviorComponent/behaviorExternalInterface/beiRobotInfo.h"
#include "engine/externalInterface/externalInterface.h"
#include "clad/externalInterface/messageGameToEngine.h"

#include "util/logging/logging.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace Anki {
namespace Vector {

namespace {

constexpr const char* kKeySocketPath          = "socketPath";
constexpr const char* kKeyFrameDuration       = "frameDuration_ms";
constexpr const char* kKeyStalenessTimeout    = "stalenessTimeout_ms";
constexpr const char* kKeyKeepAliveMode       = "keepAliveMode";
constexpr const char* kKeyInterruptRunning    = "interruptRunning";

constexpr const char* kDefaultSocketPath = "/run/visage.sock";
constexpr uint32_t    kDefaultFrameDuration_ms    = 33;
constexpr uint32_t    kDefaultStalenessTimeout_ms = 250;

bool IsFiniteFloat(float v) {
  return std::isfinite(v);
}

} // anonymous namespace

// -----------------------------------------------------------------------------
// Construction / config
// -----------------------------------------------------------------------------

BehaviorVisageRender::BehaviorVisageRender(const Json::Value& config)
: ICozmoBehavior(config)
{
  _iConfig.socketPath          = config.get(kKeySocketPath, kDefaultSocketPath).asString();
  _iConfig.frameDuration_ms    = config.get(kKeyFrameDuration, kDefaultFrameDuration_ms).asUInt();
  _iConfig.stalenessTimeout_ms = config.get(kKeyStalenessTimeout, kDefaultStalenessTimeout_ms).asUInt();
  _iConfig.interruptRunning    = config.get(kKeyInterruptRunning, true).asBool();

  const std::string kam = config.get(kKeyKeepAliveMode, "suppress").asString();
  _iConfig.keepAliveMode = (kam == "compose") ? KeepAliveMode::Compose : KeepAliveMode::Suppress;

  _dVars.sock_fd                 = -1;
  _dVars.had_first_frame         = false;
  _dVars.last_frame_received_ms  = 0;
  _dVars.frames_received         = 0;
  _dVars.frames_dropped_invalid  = 0;
}

BehaviorVisageRender::~BehaviorVisageRender() {
  CloseSocket();
}

void BehaviorVisageRender::GetBehaviorJsonKeys(std::set<const char*>& expectedKeys) const {
  expectedKeys.insert(kKeySocketPath);
  expectedKeys.insert(kKeyFrameDuration);
  expectedKeys.insert(kKeyStalenessTimeout);
  expectedKeys.insert(kKeyKeepAliveMode);
  expectedKeys.insert(kKeyInterruptRunning);
}

// -----------------------------------------------------------------------------
// Activation
// -----------------------------------------------------------------------------

bool BehaviorVisageRender::WantsToBeActivatedBehavior() const {
  // Wants to be activated whenever the behavior arbiter selects it; the actual
  // gating is done by upstream JSON conditions on this behavior's parent.
  return true;
}

void BehaviorVisageRender::OnBehaviorActivated() {
  if (!OpenSocket()) {
    PRINT_NAMED_WARNING("BehaviorVisageRender.OnBehaviorActivated.OpenSocketFailed",
                        "Failed to open %s; behavior will be a no-op until socket binds.",
                        _iConfig.socketPath.c_str());
  }

  _dVars.had_first_frame         = false;
  _dVars.last_frame_received_ms  = 0;
  _dVars.frames_received         = 0;
  _dVars.frames_dropped_invalid  = 0;

  // TODO(visage-sock): subscribe to behavior-arbiter events for the back-channel
  // (wake-word, charger, distress). Wired in task #21.

  EmitEvent(EventKind::BehaviorResumed, nullptr, 0);
}

void BehaviorVisageRender::OnBehaviorDeactivated() {
  EmitEvent(EventKind::BehaviorPreempted, nullptr, 0);
  EmitNeutralFace();
  CloseSocket();
  _dVars.event_subscribers.clear();
}

// -----------------------------------------------------------------------------
// Tick loop
// -----------------------------------------------------------------------------

void BehaviorVisageRender::BehaviorUpdate() {
  if (!IsActivated()) return;
  if (_dVars.sock_fd < 0) return;

  DrainAndEmit();
  CheckStaleness();
}

// -----------------------------------------------------------------------------
// Socket open / close
// -----------------------------------------------------------------------------

bool BehaviorVisageRender::OpenSocket() {
  CloseSocket();

  _dVars.sock_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (_dVars.sock_fd < 0) {
    PRINT_NAMED_ERROR("BehaviorVisageRender.OpenSocket.SocketFailed",
                      "socket(): %s", std::strerror(errno));
    return false;
  }

  // Remove any stale socket file so we can rebind.
  ::unlink(_iConfig.socketPath.c_str());

  if (_iConfig.socketPath.size() >= sizeof(sockaddr_un::sun_path)) {
    PRINT_NAMED_ERROR("BehaviorVisageRender.OpenSocket.SocketPathTooLong",
                      "socketPath len=%zu exceeds sun_path budget=%zu; refusing to bind silently truncated path.",
                      _iConfig.socketPath.size(),
                      sizeof(sockaddr_un::sun_path));
    CloseSocket();
    return false;
  }

  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, _iConfig.socketPath.data(), _iConfig.socketPath.size());
  // sun_path is now NUL-terminated because the trailing byte was zeroed by memset.

  if (::bind(_dVars.sock_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    PRINT_NAMED_ERROR("BehaviorVisageRender.OpenSocket.BindFailed",
                      "bind(%s): %s", _iConfig.socketPath.c_str(), std::strerror(errno));
    CloseSocket();
    return false;
  }

  // 0660 root:anki — senders must be in the anki group.
  ::chmod(_iConfig.socketPath.c_str(), 0660);

  return true;
}

void BehaviorVisageRender::CloseSocket() {
  if (_dVars.sock_fd >= 0) {
    ::close(_dVars.sock_fd);
    _dVars.sock_fd = -1;
  }
  ::unlink(_iConfig.socketPath.c_str());
}

// -----------------------------------------------------------------------------
// Frame validation
// -----------------------------------------------------------------------------

bool BehaviorVisageRender::ValidateFrame(const VisageFrame& frame) const {
  if (frame.magic != kFrameMagic)       return false;
  if (frame.version != kProtocolVersion) return false;

  // Reject reserved flag bits.
  const uint16_t kAllowedFlags = kFlagInterruptRunning | kFlagSenderIsIdle |
                                 kFlagHandshake | kFlagSubscribeEvents;
  if (frame.flags & ~kAllowedFlags) return false;

  // NaN / Inf rejection.
  if (!IsFiniteFloat(frame.face_angle_deg))   return false;
  if (!IsFiniteFloat(frame.face_center_x))    return false;
  if (!IsFiniteFloat(frame.face_center_y))    return false;
  if (!IsFiniteFloat(frame.face_scale_x))     return false;
  if (!IsFiniteFloat(frame.face_scale_y))     return false;
  if (!IsFiniteFloat(frame.scanline_opacity)) return false;

  for (size_t i = 0; i < kEyeAxisCount; ++i) {
    if (!IsFiniteFloat(frame.left_eye[i]))  return false;
    if (!IsFiniteFloat(frame.right_eye[i])) return false;
  }

  return true;
}

// -----------------------------------------------------------------------------
// DrainAndEmit — recv loop, validate, translate to DisplayProceduralFace, send.
// -----------------------------------------------------------------------------

namespace {
// Single static_assert check: ProceduralEyeParameter::NumParameters == 25.
// The frame format relies on this layout; if Anki ever changes the enum width,
// the build breaks here rather than silently mis-mapping axes.
static_assert(static_cast<size_t>(ProceduralEyeParameter::NumParameters) ==
                  BehaviorVisageRender::kEyeAxisCount,
              "ProceduralEyeParameter must have 25 entries; update kEyeAxisCount if Anki changes it.");

uint32_t NowMs() {
  using clock = std::chrono::steady_clock;
  const auto t = clock::now().time_since_epoch();
  return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(t).count());
}
} // namespace

void BehaviorVisageRender::DrainAndEmit() {
  std::array<uint8_t, kFrameBytes + 256> buf;  // small slack for malformed jumbo frames

  // Cap per-tick datagrams so a misbehaving sender cannot starve the behavior tick.
  // 64 covers a burst of one second's worth of 60 fps frames; any sender exceeding
  // that is malfunctioning and we'll see the backlog drained over subsequent ticks.
  constexpr size_t kMaxFramesPerTick = 64;
  size_t framesProcessed = 0;

  while (framesProcessed < kMaxFramesPerTick) {
    ++framesProcessed;
    struct sockaddr_un peer;
    socklen_t peer_len = sizeof(peer);
    std::memset(&peer, 0, sizeof(peer));

    ssize_t n = ::recvfrom(_dVars.sock_fd, buf.data(), buf.size(), MSG_DONTWAIT,
                           reinterpret_cast<struct sockaddr*>(&peer), &peer_len);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      PRINT_NAMED_WARNING("BehaviorVisageRender.DrainAndEmit.RecvError",
                          "recvfrom: %s", std::strerror(errno));
      break;
    }
    if (n == 0) break;

    if (static_cast<size_t>(n) != kFrameBytes) {
      _dVars.frames_dropped_invalid++;
      continue;
    }

    VisageFrame frame;
    std::memcpy(&frame, buf.data(), kFrameBytes);

    if (!ValidateFrame(frame)) {
      _dVars.frames_dropped_invalid++;
      continue;
    }

    // Handshake / event subscription bookkeeping.
    if (frame.flags & kFlagSubscribeEvents) {
      SubscribeSender(peer, peer_len);
    }

    _dVars.frames_received++;
    _dVars.last_frame_received_ms = NowMs();

    if (frame.flags & kFlagSenderIsIdle) {
      // Sender signalled idle: let staleness path handle it next tick.
      // (Don't emit this frame to the face; idle frames are advisory only.)
      continue;
    }

    _dVars.had_first_frame = true;

    // Translate VisageFrame → ProceduralFaceParameters and send.
    ProceduralFaceParameters pfp;
    pfp.faceAngle_deg   = frame.face_angle_deg;
    pfp.faceCenX        = frame.face_center_x;
    pfp.faceCenY        = frame.face_center_y;
    pfp.faceScaleX      = frame.face_scale_x;
    pfp.faceScaleY      = frame.face_scale_y;
    pfp.scanlineOpacity = frame.scanline_opacity;
    for (size_t i = 0; i < kEyeAxisCount; ++i) {
      pfp.leftEye[i]  = frame.left_eye[i];
      pfp.rightEye[i] = frame.right_eye[i];
    }

    const uint32_t duration_ms = (frame.duration_ms != 0)
                                     ? frame.duration_ms
                                     : _iConfig.frameDuration_ms;
    const bool interrupt = (frame.flags & kFlagInterruptRunning) || _iConfig.interruptRunning;

    // Honor interruptRunning: if anim is currently playing a canned animation and the
    // sender (or this behavior's config) has NOT asked to interrupt, skip this frame.
    // Mirrors the gating in AnimationComponent::HandleMessage(DisplayProceduralFace).
    auto& anim = GetBEI().GetAnimationComponent();
    if (anim.IsPlayingAnimation() && !interrupt) {
      continue;
    }

    GetBEI().GetRobotInfo().GetExternalInterface()->BroadcastToEngine<ExternalInterface::DisplayProceduralFace>(
        pfp, duration_ms, interrupt);
  }
}

void BehaviorVisageRender::CheckStaleness() {
  if (!_dVars.had_first_frame) return;

  const uint32_t now_ms = NowMs();
  if (now_ms - _dVars.last_frame_received_ms > _iConfig.stalenessTimeout_ms) {
    PRINT_NAMED_INFO("BehaviorVisageRender.CheckStaleness.SenderGone",
                     "%u ms since last frame; emitting neutral and yielding.",
                     now_ms - _dVars.last_frame_received_ms);
    EmitNeutralFace();
    _dVars.had_first_frame = false;
    CancelSelf();
  }
}

void BehaviorVisageRender::EmitNeutralFace() {
  ProceduralFaceParameters pfp;
  pfp.faceAngle_deg   = 0.0f;
  pfp.faceCenX        = 0.0f;
  pfp.faceCenY        = 0.0f;
  pfp.faceScaleX      = 1.0f;
  pfp.faceScaleY      = 1.0f;
  pfp.scanlineOpacity = 1.0f;
  for (size_t i = 0; i < kEyeAxisCount; ++i) {
    pfp.leftEye[i]  = 0.0f;
    pfp.rightEye[i] = 0.0f;
  }
  // Scale axes default to nominal 1.0
  pfp.leftEye[static_cast<size_t>(ProceduralEyeParameter::EyeScaleX)]  = 1.0f;
  pfp.leftEye[static_cast<size_t>(ProceduralEyeParameter::EyeScaleY)]  = 1.0f;
  pfp.rightEye[static_cast<size_t>(ProceduralEyeParameter::EyeScaleX)] = 1.0f;
  pfp.rightEye[static_cast<size_t>(ProceduralEyeParameter::EyeScaleY)] = 1.0f;

  GetBEI().GetRobotInfo().GetExternalInterface()->BroadcastToEngine<ExternalInterface::DisplayProceduralFace>(
      pfp, /*duration_ms=*/200, /*interruptRunning=*/true);  // 200 ms fade-to-neutral
}

// -----------------------------------------------------------------------------
// Event back-channel
// -----------------------------------------------------------------------------

void BehaviorVisageRender::EmitEvent(EventKind kind, const uint8_t* payload, uint32_t payload_len) {
  if (_dVars.sock_fd < 0) return;
  if (_dVars.event_subscribers.empty()) return;

  // Build VisageEvent header + payload into a contiguous buffer.
  // Header layout matches visage_sock_protocol.md v1.
  std::vector<uint8_t> packet;
  packet.reserve(16 + payload_len);

  auto append_u32 = [&](uint32_t v) {
    for (int i = 0; i < 4; ++i) packet.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
  };
  auto append_u16 = [&](uint16_t v) {
    for (int i = 0; i < 2; ++i) packet.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFFu));
  };

  append_u32(kEventMagic);
  append_u16(kProtocolVersion);
  append_u16(static_cast<uint16_t>(kind));
  append_u32(NowMs() * 1000u);  // approximate scene_clock_us; fine for sender-side ordering
  append_u32(payload_len);
  if (payload && payload_len > 0) {
    packet.insert(packet.end(), payload, payload + payload_len);
  }

  // Send to every subscriber. Don't remove on error; senders may be temporarily blocked.
  for (const auto& kv : _dVars.event_subscribers) {
    const std::vector<uint8_t>& raw_addr = kv.second;
    if (raw_addr.size() < sizeof(sockaddr_un::sun_family)) continue;
    const sockaddr* sa = reinterpret_cast<const sockaddr*>(raw_addr.data());
    socklen_t sa_len = static_cast<socklen_t>(raw_addr.size());
    ssize_t r = ::sendto(_dVars.sock_fd, packet.data(), packet.size(), MSG_DONTWAIT, sa, sa_len);
    if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      PRINT_NAMED_DEBUG("BehaviorVisageRender.EmitEvent.SendError",
                        "sendto subscriber %s: %s", kv.first.c_str(), std::strerror(errno));
    }
  }
}

void BehaviorVisageRender::SubscribeSender(const sockaddr_un& addr, socklen_t addr_len) {
  // Key by sun_path so re-subscribes from the same sender are idempotent.
  std::string key(addr.sun_path,
                  ::strnlen(addr.sun_path, sizeof(addr.sun_path)));
  if (key.empty()) return;  // anonymous bind; can't reach back

  std::vector<uint8_t> raw(reinterpret_cast<const uint8_t*>(&addr),
                           reinterpret_cast<const uint8_t*>(&addr) + addr_len);
  _dVars.event_subscribers[key] = std::move(raw);
}

void BehaviorVisageRender::OnWakeWordBegin()      { EmitEvent(EventKind::WakeWordBegin, nullptr, 0); }
void BehaviorVisageRender::OnWakeWordEnd()        { EmitEvent(EventKind::WakeWordEnd, nullptr, 0); }
void BehaviorVisageRender::OnChargerMounted()     { EmitEvent(EventKind::ChargerMounted, nullptr, 0); }
void BehaviorVisageRender::OnChargerDismounted()  { EmitEvent(EventKind::ChargerDismounted, nullptr, 0); }

} // namespace Vector
} // namespace Anki
