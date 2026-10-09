#include "MoveGesture.hpp"

#include "../../../../desktop/state/FocusState.hpp"
#include "../../../../desktop/view/Window.hpp"
#include "../../../../render/Renderer.hpp"
#include "../../../../layout/LayoutManager.hpp"
#include "../../../../config/ConfigValue.hpp"
#include "../../../../config/supplementary/executor/Executor.hpp"
#include "../../../../desktop/state/WindowState.hpp"

#include "../TrackpadGestures.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

static bool ksTagged(const PHLWINDOW& w, const char* tag) {
    return w && w->m_ruleApplicator && w->m_ruleApplicator->m_tagKeeper.isTagged(tag, false);
}

// A pile card's depth from the `dockd<N>` tag features/sidedock's render() sets (0 = front),
// or -1 for a dock window that carries none.
static int ksDockDepth(const PHLWINDOW& w) {
    if (!w || !w->m_ruleApplicator)
        return -1;
    for (auto const& t : w->m_ruleApplicator->m_tagKeeper.getTags()) {
        std::string s = t;
        if (!s.empty() && s.back() == '*')
            s.pop_back();
        if (s.size() > 5 && s.starts_with("dockd") && std::all_of(s.begin() + 5, s.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return std::stoi(s.substr(5));
    }
    return -1;
}

/*
  The dock's CANONICAL frame, as features/sidedock/dock.sh's geom(): x runs toward the
  dock edge, y along it. The dock sits on the panel's PHYSICAL right edge, which the
  monitor transform puts on logical right (0), bottom (1 = 90 deg), left (2) or top (3).
  Touchpad deltas are already physical -- the touchpad's right is the panel's right -- so
  they ARE canonical. Touchscreen swipes (synthesised from logical finger travel) are
  converted, so a swipe toward the panel's right edge means the same on every rotation.
*/
static Vector2D ksLogicalToCanon(const Vector2D& d, int edge) {
    switch (edge) {
        case 1: return {d.y, d.x};
        case 2: return {-d.x, d.y};
        case 3: return {-d.y, d.x};
        default: return d;
    }
}

// the inverse of ksLogicalToCanon: a canonical (toward the edge, along it) offset as a
// logical move
static Vector2D ksCanonToLogical(const Vector2D& c, int edge) {
    switch (edge) {
        case 1: return {c.y, c.x};
        case 2: return {-c.x, c.y};
        case 3: return {c.y, -c.x};
        default: return c;
    }
}

/*
  How far a swipe AWAY from the dock edge counts as pulling the front card out of the
  dock, and how far into that the rest of the pile has gone back. Past
  gestures:dock_swipe_pullout x the monitor's extent the front card is out: it follows
  the fingers 1:1 (along the edge too), and on release it is undocked. Over the last fifth
  before that the trailing cards ease back to rest, so crossing the line never jumps.
*/
static double ksPullBlend(double travel, double threshold) {
    if (travel >= 0.0 || threshold <= 0.0)
        return 0.0;
    return std::clamp((-travel - 0.8 * threshold) / (0.2 * threshold), 0.0, 1.0);
}

/*
  The mirror image, toward the edge: past gestures:dock_swipe_dismiss x the monitor's
  extent the whole dock is being put away -- every card follows the fingers off the edge,
  the trailing ones catching up with the front over the last fifth before the line -- and
  on release it hides.
*/
static double ksDismissBlend(double travel, double threshold) {
    return ksPullBlend(-travel, threshold);
}

void CMoveTrackpadGesture::begin(const ITrackpadGesture::STrackpadGestureBegin& e) {
    ITrackpadGesture::begin(e);

    m_window    = Desktop::focusState()->window();
    m_lastDelta = {};

    m_dockSwipe     = false;
    m_dockFromTouch = false;
    m_dockEdge      = 0;
    m_dockMaxDepth  = 0;
    m_dockTravel    = {};
    m_dockExtent    = 0.0;
    m_dockCards.clear();
    m_dockFront.reset();

    /*
      Side-dock pile cards (my.sidedock: tagged `dock`, not `pip`) are never dragged out of
      the cascade. The swipe drives the pile, live, along the axis toward the dock edge, in
      the PHYSICAL frame (see ksLogicalToCanon):
        toward the edge  the FRONT card follows the fingers out; released, the card right
                         behind it comes to the front ("next").
        away from it     every card but the LAST follows, each deeper one lagging by
                         gestures:dock_swipe_follow per depth; released, the last card
                         comes to the front ("prev").
        far away         past gestures:dock_swipe_pullout of the monitor, the front card
                         comes OUT: it follows the fingers freely while the rest settle
                         back; released, it is undocked ("out").
        far toward it    past gestures:dock_swipe_dismiss of the monitor, the rest of the
                         pile catches up with the front and goes off the edge with it;
                         released, the dock hides ("hide").
      gestures:dock_swipe_exec settles it (or springs it back) from wherever it was left.
      A PiP card is an ordinary floating window to this gesture and moves normally.
    */
    const auto WIN = m_window.lock();
    if (!WIN || !ksTagged(WIN, "dock") || ksTagged(WIN, "pip"))
        return;

    m_window.reset();
    m_dockSwipe     = true;
    m_dockFromTouch = g_pTrackpadGestures->m_fromTouchscreen;
    if (const auto MON = WIN->m_monitor.lock()) {
        m_dockEdge = sc<int>(MON->m_transform) % 4;
        // canonical x is logical x when the dock edge is left/right, logical y when it is
        // top/bottom (m_size is the logical, already rotated size)
        m_dockExtent = (m_dockEdge % 2 == 0) ? MON->m_size.x : MON->m_size.y;
    }

    // the pile = the dock cards on this card's (dock) workspace, by their dockd<N> depth
    for (auto const& w : Desktop::windowState()->windows()) {
        if (!w || !w->m_isMapped || !w->m_isFloating || w->m_workspace != WIN->m_workspace || !ksTagged(w, "dock") || ksTagged(w, "pip"))
            continue;
        const int DEPTH = ksDockDepth(w);
        if (DEPTH < 0)
            continue;
        m_dockCards.push_back({.window = w, .depth = DEPTH});
        m_dockMaxDepth = std::max(m_dockMaxDepth, DEPTH);
        if (DEPTH == 0)
            m_dockFront = w;
    }
    if (m_dockCards.empty()) // untagged (an older sidedock): just this card, as the front
        m_dockCards.push_back({.window = WIN, .depth = 0});
    if (!m_dockFront)
        m_dockFront = WIN;
}

void CMoveTrackpadGesture::update(const ITrackpadGesture::STrackpadGestureUpdate& e) {
    const auto DELTA = e.swipe ? e.swipe->delta : e.pinch->delta;

    if (m_dockSwipe) {
        static auto PFOLLOW  = CConfigValue<Config::FLOAT>("gestures:dock_swipe_follow");
        static auto PPULLOUT = CConfigValue<Config::FLOAT>("gestures:dock_swipe_pullout");
        static auto PDISMISS = CConfigValue<Config::FLOAT>("gestures:dock_swipe_dismiss");
        m_dockTravel += m_dockFromTouch ? ksLogicalToCanon(DELTA, m_dockEdge) : DELTA;

        const double T      = m_dockTravel.x;
        const double FOLLOW = std::clamp(sc<double>(*PFOLLOW), 0.0, 1.0);
        // 0 = an ordinary swipe, 1 = the front card is out of the pile (see ksPullBlend)
        const double PULL = ksPullBlend(T, sc<double>(*PPULLOUT) * m_dockExtent);
        // 0 = an ordinary swipe, 1 = the whole pile is going off the edge (ksDismissBlend)
        const double AWAY = ksDismissBlend(T, sc<double>(*PDISMISS) * m_dockExtent);
        for (auto& c : m_dockCards) {
            double target = 0.0, targetY = 0.0;
            if (T > 0.0)
                target = c.depth == 0 ? T : T * AWAY; // toward the edge: the front leaves; far, all go
            else if (c.depth == 0) {
                target  = T; // away: the front follows 1:1 ...
                targetY = m_dockTravel.y * PULL; // ... and, being pulled out, along the edge too
            } else if (c.depth < m_dockMaxDepth)
                target = T * std::pow(FOLLOW, c.depth) * (1.0 - PULL); // the rest lag, then settle back
            const Vector2D STEP = {target - c.applied, targetY - c.appliedY};
            const auto     W    = c.window.lock();
            if (!W || (std::abs(STEP.x) < 0.01 && std::abs(STEP.y) < 0.01))
                continue;
            g_layoutManager->moveTarget(ksCanonToLogical(STEP, m_dockEdge), W->layoutTarget());
            W->finishAnimation();
            c.applied  = target;
            c.appliedY = targetY;
        }
        return;
    }

    if (!m_window)
        return;

    if (m_window->m_isFloating) {
        g_layoutManager->moveTarget(DELTA, m_window->layoutTarget());
        m_window->finishAnimation();
        return;
    }

    // tiled window -> displace, then execute a move dispatcher on end.

    g_pHyprRenderer->damageWindow(m_window.lock());

    // funny name but works on tiled too lmao
    m_lastDelta += DELTA;
    m_window->m_floatingOffset = (m_lastDelta * 0.5F).clamp(Vector2D{-100.F, -100.F}, Vector2D{100.F, 100.F});

    g_pHyprRenderer->damageWindow(m_window.lock());
}

void CMoveTrackpadGesture::end(const ITrackpadGesture::STrackpadGestureEnd& e) {

    if (m_dockSwipe) {
        // Release: `<gestures:dock_swipe_exec> <next|prev|out|hide|none> <front address>`. Past
        // the pull-out line away from the edge = out (undock the front card, which the fingers
        // have dragged out of the pile); past the dismiss line toward it = hide (the pile has
        // followed the fingers off the edge). Otherwise 60px+ of canonical travel, more toward/away
        // from the edge than along it, cycles: toward the edge = next (the card behind comes
        // up), away = prev (the last comes up); anything shorter springs back. The front is
        // the one captured at begin, since the drag has since moved the cards.
        static auto PDOCKEXEC = CConfigValue<Config::STRING>("gestures:dock_swipe_exec");
        static auto PPULLOUT  = CConfigValue<Config::FLOAT>("gestures:dock_swipe_pullout");
        static auto PDISMISS  = CConfigValue<Config::FLOAT>("gestures:dock_swipe_dismiss");
        const bool  OUT       = ksPullBlend(m_dockTravel.x, sc<double>(*PPULLOUT) * m_dockExtent) >= 1.0;
        const bool  HIDE      = ksDismissBlend(m_dockTravel.x, sc<double>(*PDISMISS) * m_dockExtent) >= 1.0;
        m_dockSwipe           = false;
        const std::string CMD = *PDOCKEXEC;
        const auto        FRONT = m_dockFront.lock();
        m_dockCards.clear();
        m_dockFront.reset();
        if (CMD.empty() || CMD == STRVAL_EMPTY)
            return;
        const char* dir = OUT ? "out" : HIDE ? "hide" :
            (std::abs(m_dockTravel.x) >= 60.0 && std::abs(m_dockTravel.x) > std::abs(m_dockTravel.y)) ? (m_dockTravel.x > 0 ? "next" : "prev") :
                                                                                                    "none";
        Config::Supplementary::executor()->spawnRaw(std::format("{} {} {}", CMD, dir, FRONT ? std::format("0x{:x}", rc<uintptr_t>(FRONT.get())) : std::string{""}));
        return;
    }

    if (!m_window)
        return;

    if (m_window->m_isFloating || m_lastDelta.size() < 0.1F)
        return;

    // tiled: attempt to move window in the given direction

    const auto WINDOWPOS = m_window->position(Desktop::View::IGeometric::GEOMETRIC_GOAL) + m_window->m_floatingOffset;

    m_window->m_floatingOffset = {};

    if (std::abs(m_lastDelta.x) > std::abs(m_lastDelta.y)) {
        // horizontal
        g_layoutManager->moveInDirection(m_window->layoutTarget(), m_lastDelta.x > 0 ? "r" : "l");
    } else {
        // vertical
        g_layoutManager->moveInDirection(m_window->layoutTarget(), m_lastDelta.y > 0 ? "b" : "t");
    }

    const auto GOAL = m_window->position(Desktop::View::IGeometric::GEOMETRIC_GOAL);

    m_window->positionAnimation()->setValueAndWarp(WINDOWPOS);
    m_window->move(GOAL);

    m_window.reset();
}
