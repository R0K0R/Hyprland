#include "InputManager.hpp"
#include "../SessionLockManager.hpp"
#include "../../protocols/SessionLock.hpp"
#include "../../Compositor.hpp"
#include "../../desktop/view/LayerSurface.hpp"
#include "../../desktop/state/FocusState.hpp"
#include "../../config/ConfigValue.hpp"
#include "../../output/Monitor.hpp"
#include "../../state/MonitorState.hpp"
#include "../../devices/ITouch.hpp"
#include "../../event/EventBus.hpp"
#include "../SeatManager.hpp"
#include "../../protocols/core/DataDevice.hpp"
#include "debug/log/Logger.hpp"
#include "UnifiedWorkspaceSwipeGesture.hpp"

#include <algorithm>

/*
  Touch gestures live in Lua (features/hyprland/touch.lua), not here. This file only
  tracks the current touch sequence (m_touchSeq) and honours CLAIMS: a claimed
  sequence is cancelled for its client and its fingers go to Lua only (the
  input.touch.* events, emitted from the event bus at the top of each handler, see
  config/lua/LuaEventHandler.cpp). Claimed automatically once
  gestures:touch_claim_fingers fingers are down at once, or by hl.touch_claim.

  A finger that starts a border resize (keystone/10) keeps resizing: the resize branch
  runs before the claim check on move and up, and that finger is not counted towards
  the claim threshold. Lua still sees it (resizing = true on its move events).
*/
bool CInputManager::claimTouchSequence() {
    if (m_touchSeq.down.empty())
        return false;
    if (m_touchSeq.claimed)
        return true;
    m_touchSeq.claimed = true;
    // wl_touch.cancel is per sequence: every delivered finger is withdrawn at once. Their
    // ups are still sent later -- the seat counts each delivered down (m_touchLocks).
    if (std::ranges::any_of(m_touchSeq.down, [](const auto& kv) { return kv.second; }))
        g_pSeatManager->sendTouchCancel();
    return true;
}

void CInputManager::onTouchCancelAll() {
    for (const auto& [id, _] : m_touchSeq.down) {
        Event::SCallbackInfo info;
        Event::bus()->m_events.input.touch.cancel.emit(ITouch::SCancelEvent{.timeMs = 0, .touchID = id}, info);
    }
    const bool DELIVERED   = std::ranges::any_of(m_touchSeq.down, [](const auto& kv) { return kv.second; });
    const bool WASCLAIMED  = m_touchSeq.claimed; // its client was cancelled already
    m_touchSeq.down.clear();
    m_touchSeq.claimed = false;
    if (m_touchBorderResizeID) {
        m_touchBorderResizeID.reset();
        endBorderResize();
    }
    if (DELIVERED && !WASCLAIMED)
        g_pSeatManager->sendTouchCancel();
}

void CInputManager::onTouchDown(ITouch::SDownEvent e) {
    m_lastInputTouch = true;

    static auto PSWIPETOUCH  = CConfigValue<Config::INTEGER>("gestures:workspace_swipe_touch");
    static auto PGAPSOUTDATA = CConfigValue<Config::IComplexConfigValue>("general:gaps_out");
    auto* const PGAPSOUT     = sc<Config::CCssGapData*>((PGAPSOUTDATA.ptr()));
    // TODO: WORKSPACERULE.gapsOut.value_or()
    auto                 gapsOut     = *PGAPSOUT;
    static auto          PBORDERSIZE = CConfigValue<Config::INTEGER>("general:border_size");
    static auto          PSWIPEINVR  = CConfigValue<Config::INTEGER>("gestures:workspace_swipe_touch_invert");
    static auto          PCLAIM      = CConfigValue<Config::INTEGER>("gestures:touch_claim_fingers");

    // the touch sequence (see the top of this file). A finger whose up never came must not
    // wedge it: a sequence with no new finger for 10s is dropped.
    if (!m_touchSeq.down.empty() && e.timeMs - m_touchSeq.lastDownMs > 10000) {
        m_touchSeq.down.clear();
        m_touchSeq.claimed = false;
    }
    if (m_touchSeq.down.empty())
        m_touchSeq.claimed = false;
    m_touchSeq.down[e.touchID] = false;
    m_touchSeq.lastDownMs      = e.timeMs;
    if (*PCLAIM > 0) {
        const size_t COUNTED = m_touchSeq.down.size() - (m_touchBorderResizeID && m_touchSeq.down.contains(*m_touchBorderResizeID) ? 1 : 0);
        if (COUNTED >= sc<size_t>(*PCLAIM))
            claimTouchSequence();
    }

    Event::SCallbackInfo info;
    Event::bus()->m_events.input.touch.down.emit(e, info);
    if (info.cancelled)
        return;

    auto PMONITOR = State::monitorState()->query().name(!e.device->m_boundOutput.empty() ? e.device->m_boundOutput : "").run();

    PMONITOR = PMONITOR ? PMONITOR : Desktop::focusState()->monitor();

    if (PMONITOR != Desktop::focusState()->monitor())
        Desktop::focusState()->rawMonitorFocus(PMONITOR);

    const auto TOUCH_COORDS = PMONITOR->m_position + (e.pos * PMONITOR->m_size);

    m_touchData.lastTouchPos = TOUCH_COORDS;

    refocus(TOUCH_COORDS);

    // claimed (by the threshold above or by Lua during the emit): Lua's alone
    if (m_touchSeq.claimed)
        return;

    if (m_clickBehavior == CLICKMODE_KILL) {
        IPointer::SButtonEvent e;
        e.state = WL_POINTER_BUTTON_STATE_PRESSED;
        g_pInputManager->processMouseDownKill(e);
        return;
    }

    // Don't propagate new touches when a workspace swipe is in progress.
    if (g_pUnifiedWorkspaceSwipe->isGestureInProgress()) {
        return;
        // TODO: Don't swipe if you touched a floating window.
    } else if (*PSWIPETOUCH && (m_foundLSToFocus.expired() || m_foundLSToFocus->m_layer <= 1) && !g_pSessionLockManager->isSessionLocked()) {
        const auto   PWORKSPACE  = PMONITOR->m_activeWorkspace;
        const auto   STYLE       = PWORKSPACE->m_renderOffset->getStyle();
        const bool   VERTANIMS   = STYLE == "slidevert" || STYLE.starts_with("slidefadevert");
        const double TARGETLEFT  = ((VERTANIMS ? gapsOut.m_top : gapsOut.m_left) + *PBORDERSIZE) / (VERTANIMS ? PMONITOR->m_size.y : PMONITOR->m_size.x);
        const double TARGETRIGHT = 1 - (((VERTANIMS ? gapsOut.m_bottom : gapsOut.m_right) + *PBORDERSIZE) / (VERTANIMS ? PMONITOR->m_size.y : PMONITOR->m_size.x));
        const double POSITION    = (VERTANIMS ? e.pos.y : e.pos.x);
        if (POSITION < TARGETLEFT || POSITION > TARGETRIGHT) {
            g_pUnifiedWorkspaceSwipe->begin();
            g_pUnifiedWorkspaceSwipe->m_touchID = e.touchID;
            // Set the initial direction based on which edge you started from
            if (POSITION > 0.5)
                g_pUnifiedWorkspaceSwipe->m_initialDirection = *PSWIPEINVR ? -1 : 1;
            else
                g_pUnifiedWorkspaceSwipe->m_initialDirection = *PSWIPEINVR ? 1 : -1;
            return;
        }
    }

    // resize_on_border for touch: a finger landing on a window's border grab zone (wider
    // than the mouse's, general:touch_border_grab_area) resizes it like a mouse drag there.
    // One finger at a time; it never reaches the client, and other fingers are unaffected.
    if (!m_touchBorderResizeID && beginBorderResizeAt(TOUCH_COORDS)) {
        m_touchBorderResizeID = e.touchID;
        return;
    }

    // could have abovelock surface, thus only use lock if no ls found
    if (g_pSessionLockManager->isSessionLocked() && m_foundLSToFocus.expired()) {
        m_touchData.touchFocusLockSurface = g_pSessionLockManager->getSessionLockSurfaceForMonitor(PMONITOR->m_id);
        if (!m_touchData.touchFocusLockSurface)
            Log::logger->log(Log::WARN, "The session is locked but can't find a lock surface");
        else
            m_touchData.touchFocusSurface = m_touchData.touchFocusLockSurface->surface->surface();
    } else {
        m_touchData.touchFocusLockSurface.reset();
        m_touchData.touchFocusWindow  = m_foundWindowToFocus;
        m_touchData.touchFocusSurface = m_foundSurfaceToFocus;
        m_touchData.touchFocusLS      = m_foundLSToFocus;
    }

    Vector2D local;
    m_touchData.touchSurfaceScale = 1.0;

    if (m_touchData.touchFocusLockSurface) {
        local                          = TOUCH_COORDS - PMONITOR->m_position;
        m_touchData.touchSurfaceOrigin = TOUCH_COORDS - local;
    } else if (!m_touchData.touchFocusWindow.expired()) {
        // Dock keystone: a dock card is drawn as a trapezoid, so the finger is mapped back
        // through the card's inverse warp (keystoneUnwarpGlobal; the identity for any other
        // window) and the origin is kept in that UNWARPED frame. onTouchMove re-maps every
        // point the same way -- a perspective warp is not a fixed offset, so mapping only the
        // first point let a drag drift away from the finger. X11 cards (Wine) took the flat
        // box before, so a tap on a tilted card landed beside where it was drawn.
        const auto UNWARPED = Desktop::viewState()->hitTest().keystoneUnwarpGlobal(TOUCH_COORDS, m_touchData.touchFocusWindow.lock());
        if (m_touchData.touchFocusWindow->m_isX11) {
            // scale-to-fit (an X11 dock back card): logical surface coords first, then the
            // usual X11 factor -- onTouchMove divides by touchSurfaceScale the same way
            const auto FIT                 = m_touchData.touchFocusWindow->fitTransform();
            m_touchData.touchSurfaceScale  = FIT.active ? FIT.scale : 1.0;
            m_touchData.touchSurfaceOrigin = m_touchData.touchFocusWindow->position(Desktop::View::IGeometric::GEOMETRIC_GOAL) + (FIT.active ? FIT.offset : Vector2D{});
            local = (UNWARPED - m_touchData.touchSurfaceOrigin) / m_touchData.touchSurfaceScale * m_touchData.touchFocusWindow->m_X11SurfaceScaledBy;
        } else {
            Desktop::viewState()->hitTest().windowSurfaceAt(TOUCH_COORDS, m_touchData.touchFocusWindow.lock(), local);
            // scale-to-fit: local is in the client's unscaled coords, the origin in global ones
            const auto FIT                 = m_touchData.touchFocusWindow->fitTransform();
            m_touchData.touchSurfaceScale  = FIT.active ? FIT.scale : 1.0;
            m_touchData.touchSurfaceOrigin = UNWARPED - local * m_touchData.touchSurfaceScale;
        }
    } else if (!m_touchData.touchFocusLS.expired()) {
        PHLLS    foundSurf;
        Vector2D foundCoords;
        auto     surf = Desktop::viewState()->hitTest().layerPopupSurfaceAt(TOUCH_COORDS, PMONITOR, &foundCoords, &foundSurf);
        if (surf) {
            local                         = foundCoords;
            m_touchData.touchFocusSurface = surf;
        } else
            local = TOUCH_COORDS - m_touchData.touchFocusLS->m_geometry.pos();

        m_touchData.touchSurfaceOrigin = TOUCH_COORDS - local;
    } else
        return; // oops, nothing found.

    m_touchSeq.down[e.touchID] = true;
    g_pSeatManager->sendTouchDown(m_touchData.touchFocusSurface.lock(), e.timeMs, e.touchID, local);
}

void CInputManager::onTouchUp(ITouch::SUpEvent e) {
    m_lastInputTouch = true;

    Event::SCallbackInfo info;
    Event::bus()->m_events.input.touch.up.emit(e, info);
    if (info.cancelled)
        return;

    const auto SEQ       = m_touchSeq.down.find(e.touchID);
    const bool KNOWN     = SEQ != m_touchSeq.down.end();
    const bool DELIVERED = KNOWN && SEQ->second;
    const bool CLAIMED   = m_touchSeq.claimed;
    if (KNOWN)
        m_touchSeq.down.erase(SEQ);
    if (m_touchSeq.down.empty())
        m_touchSeq.claimed = false;

    if (m_touchBorderResizeID && *m_touchBorderResizeID == e.touchID) {
        m_touchBorderResizeID.reset();
        endBorderResize();
        return;
    }

    if (CLAIMED && KNOWN) {
        // the client was cancelled, but a finger it saw go down still goes up (m_touchLocks)
        if (DELIVERED && m_touchData.touchFocusSurface)
            g_pSeatManager->sendTouchUp(e.timeMs, e.touchID);
        return;
    }

    if (g_pUnifiedWorkspaceSwipe->isGestureInProgress()) {
        // If there was a swipe from this finger, end it.
        if (e.touchID == g_pUnifiedWorkspaceSwipe->m_touchID)
            g_pUnifiedWorkspaceSwipe->end();
        return;
    }

    if (m_touchData.touchFocusSurface)
        g_pSeatManager->sendTouchUp(e.timeMs, e.touchID);
}

void CInputManager::onTouchMove(ITouch::SMotionEvent e) {
    m_lastInputTouch = true;

    m_lastCursorMovement.reset();

    // Cache the global touch position so listeners (in particular the dnd
    // touchMove listener emitted just below) and renderers can resolve where
    // the finger currently is in layout coordinates.
    if (const auto PMONITOR = Desktop::focusState()->monitor(); PMONITOR)
        m_touchData.lastTouchPos = PMONITOR->m_position + (e.pos * PMONITOR->m_size);

    Event::SCallbackInfo info;
    Event::bus()->m_events.input.touch.motion.emit(e, info);
    if (info.cancelled)
        return;

    if (m_touchBorderResizeID && *m_touchBorderResizeID == e.touchID) {
        borderResizeMoveTo(m_touchData.lastTouchPos);
        return;
    }

    // a claimed finger moves for Lua only
    if (m_touchSeq.claimed && m_touchSeq.down.contains(e.touchID))
        return;

    if (g_pUnifiedWorkspaceSwipe->isGestureInProgress()) {
        // Do nothing if this is using a different finger.
        if (e.touchID != g_pUnifiedWorkspaceSwipe->m_touchID)
            return;

        const auto  ANIMSTYLE     = g_pUnifiedWorkspaceSwipe->m_workspaceBegin->m_renderOffset->getStyle();
        const bool  VERTANIMS     = ANIMSTYLE == "slidevert" || ANIMSTYLE.starts_with("slidefadevert");
        static auto PSWIPEINVR    = CConfigValue<Config::INTEGER>("gestures:workspace_swipe_touch_invert");
        static auto PSWIPEDIST    = CConfigValue<Config::INTEGER>("gestures:workspace_swipe_distance");
        const auto  SWIPEDISTANCE = std::clamp(*PSWIPEDIST, sc<int64_t>(1LL), sc<int64_t>(UINT32_MAX));
        // Handle the workspace swipe if there is one
        if (g_pUnifiedWorkspaceSwipe->m_initialDirection == -1) {
            if (*PSWIPEINVR)
                // go from 0 to -SWIPEDISTANCE
                g_pUnifiedWorkspaceSwipe->update(SWIPEDISTANCE * ((VERTANIMS ? e.pos.y : e.pos.x) - 1));
            else
                // go from 0 to -SWIPEDISTANCE
                g_pUnifiedWorkspaceSwipe->update(SWIPEDISTANCE * (-1 * (VERTANIMS ? e.pos.y : e.pos.x)));
        } else if (*PSWIPEINVR)
            // go from 0 to SWIPEDISTANCE
            g_pUnifiedWorkspaceSwipe->update(SWIPEDISTANCE * (VERTANIMS ? e.pos.y : e.pos.x));
        else
            // go from 0 to SWIPEDISTANCE
            g_pUnifiedWorkspaceSwipe->update(SWIPEDISTANCE * (1 - (VERTANIMS ? e.pos.y : e.pos.x)));
        return;
    }
    // During a drag-and-drop session, repick the surface under the finger so
    // wl_data_device enter/leave/offer follow the touch point, the same way
    // cursor motion drives pointer focus during mouse drags. Touch events are
    // not delivered to surfaces during the drag (mouse drags work likewise).
    if (PROTO::data->dndActive()) {
        refocus(m_touchData.lastTouchPos);
        return;
    }
    if (m_touchData.touchFocusLockSurface) {
        const auto PMONITOR     = State::monitorState()->query().id(m_touchData.touchFocusLockSurface->iMonitorID).run();
        if (!PMONITOR) // see below: the output can be gone mid-touch
            return;
        const auto TOUCH_COORDS = PMONITOR->m_position + (e.pos * PMONITOR->m_size);
        const auto LOCAL        = TOUCH_COORDS - PMONITOR->m_position;
        g_pSeatManager->sendTouchMotion(e.timeMs, e.touchID, LOCAL);
    } else if (validMapped(m_touchData.touchFocusWindow)) {
        const auto PMONITOR     = m_touchData.touchFocusWindow->m_monitor.lock();
        // The touched window's monitor can be gone mid-touch: a lid-close handler
        // disables eDP-1, and a finger moving while the output comes back reached
        // here with no monitor -- SIGSEGV in onTouchMove (2026-10-08). Drop the motion.
        if (!PMONITOR)
            return;
        const auto TOUCH_COORDS = PMONITOR->m_position + (e.pos * PMONITOR->m_size);
        // through the dock keystone's inverse warp, as onTouchDown did (the origin is unwarped)
        const auto UNWARPED     = Desktop::viewState()->hitTest().keystoneUnwarpGlobal(TOUCH_COORDS, m_touchData.touchFocusWindow.lock());
        auto       local        = (UNWARPED - m_touchData.touchSurfaceOrigin) / m_touchData.touchSurfaceScale;
        if (m_touchData.touchFocusWindow->m_isX11)
            local = local * m_touchData.touchFocusWindow->m_X11SurfaceScaledBy;

        g_pSeatManager->sendTouchMotion(e.timeMs, e.touchID, local);
    } else if (validMapped(m_touchData.touchFocusLS)) {
        const auto PMONITOR     = m_touchData.touchFocusLS->m_monitor.lock();
        if (!PMONITOR)
            return;
        const auto TOUCH_COORDS = PMONITOR->m_position + (e.pos * PMONITOR->m_size);
        const auto LOCAL        = TOUCH_COORDS - m_touchData.touchSurfaceOrigin;

        g_pSeatManager->sendTouchMotion(e.timeMs, e.touchID, LOCAL);
    }
}
