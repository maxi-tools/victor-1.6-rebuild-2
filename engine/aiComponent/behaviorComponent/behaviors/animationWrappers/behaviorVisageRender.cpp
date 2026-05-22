/**
 * File: BehaviorVisageRender
 *
 * Copyright: Maxi Tools 2026
 **/

#include "engine/aiComponent/behaviorComponent/behaviors/animationWrappers/behaviorVisageRender.h"

#include "clad/externalInterface/messageGameToEngine.h"
#include "clad/types/proceduralFaceTypes.h"

#include "engine/aiComponent/behaviorComponent/behaviorExternalInterface/behaviorExternalInterface.h"
#include "engine/robot.h"
#include "engine/robotInterface/messageHandler.h"

#include "util/logging/logging.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
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

  struct sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, _iConfig.socketPath.c_str(), sizeof(addr.sun_path) - 1);

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
// DrainAndEmit / CheckStaleness / EmitNeutralFace
//
// Stubs land here in task #18 (skeleton). Concrete recv loop + DisplayProceduralFace
// emission lands in tasks #19 (socket reader) and #20 (emission).
// -----------------------------------------------------------------------------

void BehaviorVisageRender::DrainAndEmit() {
  // TODO(task #19, #20): recv loop, validate, translate to DisplayProceduralFace, send.
}

void BehaviorVisageRender::CheckStaleness() {
  if (!_dVars.had_first_frame) return;

  // TODO(task #19): pull tick time from behaviorExternalInterface clock.
  // For now, leave staleness handling for the socket-reader task.
}

void BehaviorVisageRender::EmitNeutralFace() {
  // TODO(task #20): construct neutral DisplayProceduralFace and send.
}

// -----------------------------------------------------------------------------
// Event back-channel
//
// Stubs here; concrete behavior-arbiter subscription + event emission lands in task #21.
// -----------------------------------------------------------------------------

void BehaviorVisageRender::EmitEvent(EventKind kind, const uint8_t* payload, uint32_t payload_len) {
  // TODO(task #21): build VisageEvent and sendto each subscriber.
  (void)kind;
  (void)payload;
  (void)payload_len;
}

void BehaviorVisageRender::SubscribeSender(const sockaddr_un& addr, socklen_t addr_len) {
  // TODO(task #21): record sender address.
  (void)addr;
  (void)addr_len;
}

void BehaviorVisageRender::OnWakeWordBegin()      { EmitEvent(EventKind::WakeWordBegin, nullptr, 0); }
void BehaviorVisageRender::OnWakeWordEnd()        { EmitEvent(EventKind::WakeWordEnd, nullptr, 0); }
void BehaviorVisageRender::OnChargerMounted()     { EmitEvent(EventKind::ChargerMounted, nullptr, 0); }
void BehaviorVisageRender::OnChargerDismounted()  { EmitEvent(EventKind::ChargerDismounted, nullptr, 0); }

} // namespace Vector
} // namespace Anki
