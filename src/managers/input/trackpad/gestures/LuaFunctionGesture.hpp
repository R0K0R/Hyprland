#pragma once

#include "ITrackpadGesture.hpp"

#include <memory>
#include <string>

class CLuaFunctionGesture : public ITrackpadGesture {
  public:
    CLuaFunctionGesture(int ref);
    CLuaFunctionGesture(int startRef, int updateRef, int endRef);
    virtual ~CLuaFunctionGesture() = default;

    virtual void begin(const ITrackpadGesture::STrackpadGestureBegin& e);
    virtual void update(const ITrackpadGesture::STrackpadGestureUpdate& e);
    virtual void end(const ITrackpadGesture::STrackpadGestureEnd& e);

    // Handoff (hl.gesture_handoff from inside a `start` callback): the Lua decides, as the
    // swipe begins, to give the REST of it to a built-in gesture -- "workspace", the
    // interactive workspace swipe -- instead of handling it itself. features/hyprland
    // uses it for a 4-finger swipe that walks the column like Mod+J/K and only at the
    // column's end becomes the follow-the-fingers workspace swipe.
    static std::string s_requestedHandoff;

  private:
    std::unique_ptr<ITrackpadGesture> m_handoff;
    bool                              m_delegating = false;

    int  m_luaStartFnId  = -2;
    int  m_luaUpdateFnId = -2;
    int  m_luaEndFnId    = -2;
    bool m_legacyEndOnly = false;
};
