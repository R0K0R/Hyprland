#pragma once

#include "ITrackpadGesture.hpp"

#include "../../../../desktop/DesktopTypes.hpp"

// Dock keystone (my.sidedock, trapezoid.patch): a 5-finger gesture that toggles the mission-
// control overview (expo). Registered as the "overview" gesture action in both config
// front-ends (legacy ConfigManager + Lua LuaBindingsConfigRules). Mirrors the local
// Move/Float gesture patch; the actual work lives in CHyprRenderer::ksOverviewToggle().
class COverviewTrackpadGesture : public ITrackpadGesture {
  public:
    COverviewTrackpadGesture()          = default;
    virtual ~COverviewTrackpadGesture() = default;

    virtual void begin(const ITrackpadGesture::STrackpadGestureBegin& e);
    virtual void update(const ITrackpadGesture::STrackpadGestureUpdate& e);
    virtual void end(const ITrackpadGesture::STrackpadGestureEnd& e);
};
