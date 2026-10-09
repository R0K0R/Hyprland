#pragma once

#include "ITrackpadGesture.hpp"
#include "../../../../config/lua/LuaRef.hpp"

#include <memory>
#include <string>

class CLuaFunctionGesture : public ITrackpadGesture {
  public:
    explicit CLuaFunctionGesture(Config::Lua::CLuaRef&& fn);
    CLuaFunctionGesture(Config::Lua::CLuaRef&& start, Config::Lua::CLuaRef&& update, Config::Lua::CLuaRef&& end);
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

    Config::Lua::CLuaRef m_luaStartFn;
    Config::Lua::CLuaRef m_luaUpdateFn;
    Config::Lua::CLuaRef m_luaEndFn;
    bool                 m_legacyEndOnly = false;
};
