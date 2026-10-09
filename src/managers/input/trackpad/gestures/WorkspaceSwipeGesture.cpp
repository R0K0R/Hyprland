#include "WorkspaceSwipeGesture.hpp"

#include "../../../../Compositor.hpp"
#include "../../../../state/WorkspaceState.hpp"
#include "../../../../desktop/state/FocusState.hpp"
#include "../../../../render/Renderer.hpp"

#include "../../UnifiedWorkspaceSwipeGesture.hpp"
#include "../TrackpadGestures.hpp"
#include "../../../../output/Monitor.hpp"

void CWorkspaceSwipeGesture::begin(const ITrackpadGesture::STrackpadGestureBegin& e) {
    ITrackpadGesture::begin(e);

    static auto PSWIPENEW = CConfigValue<Config::INTEGER>("gestures:workspace_swipe_create_new");

    if (g_pSessionLockManager->isSessionLocked() || g_pUnifiedWorkspaceSwipe->isGestureInProgress())
        return;

    int onMonitor = 0;
    for (auto const& w : State::workspaceState()->workspaces()) {
        if (w->m_monitor == Desktop::focusState()->monitor() && !State::workspaceState()->isSpecial(w->m_id))
            onMonitor++;
    }

    if (onMonitor < 2 && !*PSWIPENEW)
        return; // disallow swiping when there's 1 workspace on a monitor

    g_pUnifiedWorkspaceSwipe->begin();
}

void CWorkspaceSwipeGesture::update(const ITrackpadGesture::STrackpadGestureUpdate& e) {
    if (!g_pUnifiedWorkspaceSwipe->isGestureInProgress())
        return;

    float        DELTA = distance(e);

    static auto  PSWIPEINVR = CConfigValue<Config::INTEGER>("gestures:workspace_swipe_invert");

    // A touchscreen swipe fed in as a trackpad one (keystone/11) moves by the finger's
    // logical px, while the unified swipe maps gestures:workspace_swipe_distance --
    // touchpad units, 300 by default -- onto a whole monitor: the workspace ran about
    // four times ahead of the finger on the 1200px panel. Scale THIS STEP so a full
    // monitor of finger travel is a full workspace. Only the step: the running total
    // below is the unified swipe's own m_delta, and scaling the sum on every update
    // (the first version of this, in UnifiedWorkspaceSwipeGesture::update) decayed it
    // geometrically -- the workspace crept a few px, tracking speed, not position.
    if (g_pTrackpadGestures->m_fromTouchscreen && g_pUnifiedWorkspaceSwipe->m_monitor && g_pUnifiedWorkspaceSwipe->m_workspaceBegin) {
        static auto PSWIPEDIST    = CConfigValue<Config::INTEGER>("gestures:workspace_swipe_distance");
        static auto PWORKSPACEGAP = CConfigValue<Config::INTEGER>("general:gaps_workspaces");
        const auto  STYLE         = g_pUnifiedWorkspaceSwipe->m_workspaceBegin->m_renderOffset->getStyle();
        const bool  VERT          = STYLE == "slidevert" || STYLE.starts_with("slidefadevert");
        const auto  MON           = g_pUnifiedWorkspaceSwipe->m_monitor.lock();
        const double EXTENT       = (VERT ? MON->m_size.y : MON->m_size.x) + *PWORKSPACEGAP;
        if (EXTENT > 0)
            DELTA *= sc<float>(std::clamp(*PSWIPEDIST, sc<int64_t>(1LL), sc<int64_t>(UINT32_MAX)) / EXTENT);
    }

    const double D = g_pUnifiedWorkspaceSwipe->m_delta + (*PSWIPEINVR ? -DELTA : DELTA);
    g_pUnifiedWorkspaceSwipe->update(D);
}

void CWorkspaceSwipeGesture::end(const ITrackpadGesture::STrackpadGestureEnd& e) {
    if (!g_pUnifiedWorkspaceSwipe->isGestureInProgress())
        return;

    g_pUnifiedWorkspaceSwipe->end();
}

bool CWorkspaceSwipeGesture::isDirectionSensitive() {
    return true;
}
