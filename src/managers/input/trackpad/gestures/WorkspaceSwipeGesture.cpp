#include "WorkspaceSwipeGesture.hpp"

#include "../../../../Compositor.hpp"
#include "../../../../state/WorkspaceState.hpp"
#include "../../../../desktop/state/FocusState.hpp"
#include "../../../../render/Renderer.hpp"

#include "../../UnifiedWorkspaceSwipeGesture.hpp"
#include "../TrackpadGestures.hpp"
#include "../../../../output/Monitor.hpp"

CWorkspaceSwipeGesture::~CWorkspaceSwipeGesture() {
    cancel();
}

void CWorkspaceSwipeGesture::cancel() {
    const auto SESSION = m_sessionID;
    m_sessionID        = 0;
    if (SESSION && g_pUnifiedWorkspaceSwipe && SESSION == g_pUnifiedWorkspaceSwipe->sessionID())
        g_pUnifiedWorkspaceSwipe->cancel();
}

void CWorkspaceSwipeGesture::begin(const ITrackpadGesture::STrackpadGestureBegin& e) {
    cancel();
    ITrackpadGesture::begin(e);

    static auto PSWIPENEW = CConfigValue<Config::INTEGER>("gestures:workspace_swipe_create_new");

    if (g_pSessionLockManager->isSessionLocked() || g_pUnifiedWorkspaceSwipe->isGestureInProgress())
        return;

    const auto MONITOR = Desktop::focusState()->monitor();
    if (!MONITOR)
        return;

    int onMonitor = 0;
    for (auto const& w : State::Workspace::state()->workspaces()) {
        if (w->m_monitor == MONITOR && w->type() != Workspace::eWorkspaceType::SPECIAL)
            onMonitor++;
    }

    if (onMonitor < 2 && !*PSWIPENEW)
        return; // disallow swiping when there's 1 workspace on a monitor

    if (g_pUnifiedWorkspaceSwipe->begin(MONITOR))
        m_sessionID = g_pUnifiedWorkspaceSwipe->sessionID();
}

void CWorkspaceSwipeGesture::update(const ITrackpadGesture::STrackpadGestureUpdate& e) {
    if (!m_sessionID || m_sessionID != g_pUnifiedWorkspaceSwipe->sessionID())
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
    const auto SESSION = m_sessionID;
    m_sessionID        = 0;
    if (!SESSION || SESSION != g_pUnifiedWorkspaceSwipe->sessionID())
        return;

    g_pUnifiedWorkspaceSwipe->end();
}

bool CWorkspaceSwipeGesture::isDirectionSensitive() {
    return true;
}
