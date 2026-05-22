/**
 * File: testBehaviorVisageRender.cpp
 *
 * Description: Tests for BehaviorVisageRender's wire-format invariants
 *              (VisageFrame layout, magic, version, validation rules).
 *              Heavyweight behavior-lifecycle tests are intentionally
 *              skipped here; the recv loop is exercised in hardware
 *              bring-up using tools/visage_sock_sender.py.
 *
 * Copyright: Maxi Tools 2026
 *
 **/

#include "engine/aiComponent/behaviorComponent/behaviors/animationWrappers/behaviorVisageRender.h"

#include "gtest/gtest.h"

#include <cmath>
#include <cstring>
#include <limits>

using namespace Anki::Vector;

namespace {

// Build a minimally-valid VisageFrame for tests to mutate.
BehaviorVisageRender::VisageFrame MakeGoodFrame() {
  BehaviorVisageRender::VisageFrame f;
  std::memset(&f, 0, sizeof(f));
  f.magic            = BehaviorVisageRender::kFrameMagic;
  f.version          = BehaviorVisageRender::kProtocolVersion;
  f.flags            = 0;
  f.duration_ms      = 33;
  f.scene_clock_us   = 0;
  f.face_angle_deg   = 0.0f;
  f.face_center_x    = 0.0f;
  f.face_center_y    = 0.0f;
  f.face_scale_x     = 1.0f;
  f.face_scale_y     = 1.0f;
  f.scanline_opacity = 1.0f;
  for (size_t i = 0; i < BehaviorVisageRender::kEyeAxisCount; ++i) {
    f.left_eye[i]  = 0.0f;
    f.right_eye[i] = 0.0f;
  }
  return f;
}

} // namespace

// ============================================================================
// Layout invariants — these guard against the wire format silently drifting
// out of sync with senders (visage_sock_sender.py, future maxi-visage runtime,
// any other consumer).
// ============================================================================

TEST(BehaviorVisageRender_WireFormat, FrameIs240Bytes)
{
  // Senders pack exactly 240 bytes. If this drifts, every cross-bot sender
  // breaks. See maxi-tools/maxi-visage Documentation/visage_sock_protocol.md.
  EXPECT_EQ(sizeof(BehaviorVisageRender::VisageFrame), size_t{240});
}

TEST(BehaviorVisageRender_WireFormat, MagicIsVISG)
{
  // 'V','I','S','G' little-endian = 0x47534956.
  EXPECT_EQ(BehaviorVisageRender::kFrameMagic, uint32_t{0x47534956u});
}

TEST(BehaviorVisageRender_WireFormat, EventMagicIsVEVT)
{
  // 'V','E','V','T' little-endian = 0x54564556.
  EXPECT_EQ(BehaviorVisageRender::kEventMagic, uint32_t{0x54564556u});
}

TEST(BehaviorVisageRender_WireFormat, ProtocolVersionIsOne)
{
  EXPECT_EQ(BehaviorVisageRender::kProtocolVersion, uint16_t{1});
}

TEST(BehaviorVisageRender_WireFormat, EyeAxisCountIs25)
{
  // procedural_25 face engine. Must match ProceduralEyeParameter::NumParameters.
  EXPECT_EQ(BehaviorVisageRender::kEyeAxisCount, size_t{25});
}

TEST(BehaviorVisageRender_WireFormat, EyeArrayOffsets)
{
  // left_eye starts after the 16-byte header + 6 face floats (24 bytes) = byte 40.
  // right_eye follows left_eye (25 floats × 4 bytes = 100 bytes) → byte 140.
  BehaviorVisageRender::VisageFrame f;
  std::memset(&f, 0, sizeof(f));
  const uint8_t* base = reinterpret_cast<const uint8_t*>(&f);
  EXPECT_EQ(reinterpret_cast<const uint8_t*>(&f.left_eye[0])  - base, ptrdiff_t{40});
  EXPECT_EQ(reinterpret_cast<const uint8_t*>(&f.right_eye[0]) - base, ptrdiff_t{140});
}

// NOTE: ValidateFrame is intentionally private; deeper unit tests of validation
// belong with the integration tests in tools/visage_sock_sender.py, where we
// can exercise the live recv path against a real BehaviorVisageRender. The
// layout invariants above catch the most common drift class (sender/receiver
// packed-struct mismatch).
