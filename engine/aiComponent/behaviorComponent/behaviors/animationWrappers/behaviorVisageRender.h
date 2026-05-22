/**
 * File: BehaviorVisageRender
 *
 * Description: Consumes a stream of ProceduralFace parameter frames from a
 *              local Unix-domain datagram socket (/run/visage.sock) and
 *              emits DisplayProceduralFace CLAD messages to vic-anim each
 *              tick. Lets maxi-visage (or any external sender) drive Vector's
 *              face directly via the existing ProceduralFace rendering
 *              pipeline.
 *
 *              Protocol: see maxi-tools/maxi-visage Documentation/visage_sock_protocol.md
 *              Design:   see maxi-tools/maxi-vector-cloud docs/behavior_visage_render_design.md
 *
 * Copyright: Maxi Tools 2026
 **/

#ifndef __Cozmo_Basestation_Behaviors_BehaviorVisageRender_H__
#define __Cozmo_Basestation_Behaviors_BehaviorVisageRender_H__

#include "engine/aiComponent/behaviorComponent/behaviors/iCozmoBehavior.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declarations from <sys/socket.h> avoided in header; .cpp includes them.
struct sockaddr_un;

namespace Anki {
namespace Vector {

class BehaviorVisageRender : public ICozmoBehavior
{
protected:
  // Enforce creation through BehaviorFactory
  friend class BehaviorFactory;
  BehaviorVisageRender(const Json::Value& config);

public:
  virtual ~BehaviorVisageRender();

  virtual bool WantsToBeActivatedBehavior() const override;

protected:
  virtual void GetBehaviorOperationModifiers(BehaviorOperationModifiers& modifiers) const override {
    modifiers.wantsToBeActivatedWhenCarryingObject = true;
    modifiers.wantsToBeActivatedWhenOffTreads      = true;
    modifiers.wantsToBeActivatedWhenOnCharger      = true;
    modifiers.behaviorAlwaysDelegates              = false;
  }

  virtual void GetBehaviorJsonKeys(std::set<const char*>& expectedKeys) const override;

  virtual void OnBehaviorActivated() override;
  virtual void OnBehaviorDeactivated() override;
  virtual void BehaviorUpdate() override;

public:
  // --- Wire formats (must match maxi-visage Documentation/visage_sock_protocol.md v1) ---
  // Public so unit tests and any in-process consumer can assert layout invariants.

  static constexpr uint32_t kFrameMagic     = 0x47534956u; // 'V','I','S','G' LE
  static constexpr uint32_t kEventMagic     = 0x54564556u; // 'V','E','V','T' LE
  static constexpr uint16_t kProtocolVersion = 1;
  static constexpr size_t   kFrameBytes     = 240;
  static constexpr size_t   kEyeAxisCount   = 25;

  enum FrameFlag : uint16_t {
    kFlagInterruptRunning = 1u << 0,
    kFlagSenderIsIdle     = 1u << 1,
    kFlagHandshake        = 1u << 2,
    kFlagSubscribeEvents  = 1u << 3,
  };

  enum class EventKind : uint16_t {
    WakeWordBegin     = 0x0001,
    WakeWordEnd       = 0x0002,
    ChargerMounted    = 0x0003,
    ChargerDismounted = 0x0004,
    CubeTapped        = 0x0005,
    BehaviorPreempted = 0x0006,
    BehaviorResumed   = 0x0007,
    Distress          = 0x0008,
    IdleHint          = 0x0009,
    Heartbeat         = 0x000A,
  };

  struct VisageFrame {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint32_t duration_ms;
    uint32_t scene_clock_us;
    float    face_angle_deg;
    float    face_center_x;
    float    face_center_y;
    float    face_scale_x;
    float    face_scale_y;
    float    scanline_opacity;
    float    left_eye[kEyeAxisCount];
    float    right_eye[kEyeAxisCount];
  } __attribute__((packed));
  static_assert(sizeof(VisageFrame) == kFrameBytes, "VisageFrame must be 240 bytes");

private:
  enum class KeepAliveMode { Suppress, Compose };

  struct InstanceConfig {
    std::string   socketPath;          // default "/run/visage.sock"
    uint32_t      frameDuration_ms;    // 33 (30 fps) or 16 (60 fps)
    uint32_t      stalenessTimeout_ms; // default 250
    KeepAliveMode keepAliveMode;       // default Suppress
    bool          interruptRunning;    // default true
  };

  struct DynamicVariables {
    int       sock_fd;
    bool      had_first_frame;
    uint32_t  last_frame_received_ms;
    uint32_t  frames_received;
    uint32_t  frames_dropped_invalid;
    // Subscribed senders for the event back-channel: key = stringified sun_path
    std::unordered_map<std::string, std::vector<uint8_t>> event_subscribers;
  };

  InstanceConfig   _iConfig;
  DynamicVariables _dVars;

  // --- Socket management ---
  bool OpenSocket();
  void CloseSocket();

  // --- Per-tick work ---
  void DrainAndEmit();   // recv all pending datagrams; emit DisplayProceduralFace per valid frame
  void EmitNeutralFace(); // fade-to-idle on deactivate / staleness
  void CheckStaleness();

  // --- Frame validation ---
  // Returns true if frame structurally valid; out_sender_addr filled with peer for event subscription.
  bool ValidateFrame(const VisageFrame& frame) const;

  // --- Event back-channel ---
  void EmitEvent(EventKind kind, const uint8_t* payload, uint32_t payload_len);
  void SubscribeSender(const sockaddr_un& addr, socklen_t addr_len);

  // --- Behavior arbiter hooks (subscribed during OnBehaviorActivated) ---
  void OnWakeWordBegin();
  void OnWakeWordEnd();
  void OnChargerMounted();
  void OnChargerDismounted();
};

} // namespace Vector
} // namespace Anki

#endif // __Cozmo_Basestation_Behaviors_BehaviorVisageRender_H__
