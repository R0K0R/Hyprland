#include "ViewHitTester.hpp"
#include "FocusState.hpp"
#include "ViewStateTracker.hpp"
#include "../view/LayerSurface.hpp"
#include "../view/WLSurface.hpp"
#include "../view/Window.hpp"
#include "../../config/ConfigValue.hpp"
#include "../../layout/space/Space.hpp"
#include "../../macros.hpp"
#include "../../protocols/LayerShell.hpp"
#include "../../protocols/XDGShell.hpp"
#include "../../protocols/core/Compositor.hpp"
#include "../../state/MonitorState.hpp"
#include "../../output/Monitor.hpp"
#include "../../state/WorkspaceState.hpp"
#include "../../xwayland/XWayland.hpp"
#include "../../managers/fullscreen/FullscreenController.hpp"

#include <cmath>
#include <ranges>
#include <tuple>

using namespace Desktop;
using namespace Desktop::View;

CViewHitTester::CViewHitTester(const IViewStateTracker& tracker) : m_tracker(tracker) {
    ;
}

PHLWINDOW CViewHitTester::windowAt(const Vector2D& pos, uint16_t properties, PHLWINDOW ignoreWindow) const {
    const auto PMONITOR = State::monitorState()->query().vec(pos).run();
    if (!PMONITOR)
        return nullptr;

    static auto PRESIZEONBORDER       = CConfigValue<Config::INTEGER>("general:resize_on_border");
    static auto PBORDERSIZE           = CConfigValue<Config::INTEGER>("general:border_size");
    static auto PBORDERGRABEXTEND     = CConfigValue<Config::INTEGER>("general:extend_border_grab_area");
    static auto PSPECIALFALLTHRU      = CConfigValue<Config::INTEGER>("input:special_fallthrough");
    static auto PMODALPARENTBLOCKING  = CConfigValue<Config::INTEGER>("general:modal_parent_blocking");
    static auto PFOLLOWMOUSESHRINK    = CConfigValue<Config::INTEGER>("input:follow_mouse_shrink");
    const auto  BORDER_GRAB_AREA      = *PRESIZEONBORDER ? *PBORDERSIZE + *PBORDERGRABEXTEND : 0;
    const bool  ONLY_PRIORITY         = properties & FOCUS_PRIORITY;
    const bool  DO_FOLLOW_MOUSE_CHECK = properties & FOLLOW_MOUSE_CHECK;
    const auto  HITBOX_SHRINK         = DO_FOLLOW_MOUSE_CHECK ? *PFOLLOWMOUSESHRINK : 0;
    const auto  LASTFOCUSED           = focusState()->window();
    const auto& WINDOWS               = m_tracker.windows();

    const auto  isShadowedByModal = [](PHLWINDOW w) -> bool {
        return *PMODALPARENTBLOCKING && w->m_xdgSurface && w->m_xdgSurface->m_toplevel && w->m_xdgSurface->m_toplevel->anyChildModal();
    };

    // pinned windows on top of floating regardless
    if (properties & ALLOW_FLOATING) {
        for (auto const& w : WINDOWS | std::views::reverse) {
            if (ONLY_PRIORITY && !w->priorityFocus())
                continue;

            if (w->m_isFloating && w->m_isMapped && w->acceptsInput() && !w->m_X11ShouldntFocus && w->m_pinned && !w->m_ruleApplicator->noFocus().valueOrDefault() &&
                w != ignoreWindow && !isShadowedByModal(w)) {
                const auto BB  = w->getWindowBoxUnified(properties);
                CBox       box = BB.copy().expand(!w->isX11OverrideRedirect() ? BORDER_GRAB_AREA : 0);
                if (HITBOX_SHRINK > 0 && w != LASTFOCUSED)
                    box = box.copy().expand(-HITBOX_SHRINK);
                if (box.containsPoint(pos))
                    return w;

                if (!w->m_isX11) {
                    if (w->hasPopupAt(pos))
                        return w;
                }
            }
        }
    }

    auto windowForWorkspace = [&](bool special) -> PHLWINDOW {
        auto floating = [&](bool aboveFullscreen) -> PHLWINDOW {
            for (auto const& w : WINDOWS | std::views::reverse) {
                if (special && !w->onSpecialWorkspace()) // because special floating may creep up into regular
                    continue;

                if (!w->m_workspace)
                    continue;

                if (ONLY_PRIORITY && !w->priorityFocus())
                    continue;

                const auto PWINDOWMONITOR = w->m_monitor.lock();

                // to avoid focusing windows behind special workspaces from other monitors
                if (!*PSPECIALFALLTHRU && PWINDOWMONITOR && PWINDOWMONITOR->m_activeSpecialWorkspace && w->m_workspace != PWINDOWMONITOR->m_activeSpecialWorkspace) {
                    const auto BB = w->getWindowBoxUnified(properties);
                    if (BB.x >= PWINDOWMONITOR->m_position.x && BB.y >= PWINDOWMONITOR->m_position.y &&
                        BB.x + BB.width <= PWINDOWMONITOR->m_position.x + PWINDOWMONITOR->m_size.x && BB.y + BB.height <= PWINDOWMONITOR->m_position.y + PWINDOWMONITOR->m_size.y)
                        continue;
                }

                if (w->m_isFloating && w->m_isMapped && w->m_workspace->isVisible() && w->acceptsInput() && !w->m_pinned && !w->m_ruleApplicator->noFocus().valueOrDefault() &&
                    w != ignoreWindow && (!aboveFullscreen || w->isAllowedOverFullscreen()) && !isShadowedByModal(w)) {
                    // OR windows should add focus to parent
                    if (w->m_X11ShouldntFocus && !w->isX11OverrideRedirect())
                        continue;

                    const auto BB  = w->getWindowBoxUnified(properties);
                    CBox       box = BB.copy().expand(!w->isX11OverrideRedirect() ? BORDER_GRAB_AREA : 0);
                    if (HITBOX_SHRINK > 0 && w != LASTFOCUSED)
                        box = box.copy().expand(-HITBOX_SHRINK);
                    if (box.containsPoint(pos)) {
                        if (w->m_isX11 && w->isX11OverrideRedirect() && !w->m_xwaylandSurface->wantsFocus()) {
                            // Override Redirect
                            return focusState()->window(); // we kinda trick everything here.
                            // TODO: this is wrong, we should focus the parent, but idk how to get it considering it's nullptr in most cases.
                        }

                        return w;
                    }

                    if (!w->m_isX11) {
                        if (w->hasPopupAt(pos))
                            return w;
                    }
                }
            }

            return nullptr;
        };

        if (properties & ALLOW_FLOATING) {
            // first loop over floating cuz they're above, m_lWindows should be sorted bottom->top, for tiled it doesn't matter.
            auto found = floating(true);
            if (found)
                return found;
        }

        if (properties & FLOATING_ONLY)
            return floating(false);

        const WORKSPACEID WSPID      = special ? PMONITOR->activeSpecialWorkspaceID() : PMONITOR->activeWorkspaceID();
        const auto        PWORKSPACE = State::workspaceState()->query().id(WSPID).run();

        if (Fullscreen::controller()->hasFullscreen(PWORKSPACE) && !(properties & SKIP_FULLSCREEN_PRIORITY) && !ONLY_PRIORITY) {
            const auto FS_WINDOW = Fullscreen::controller()->getFullscreenWindow(PWORKSPACE);

            if (!FS_WINDOW)
                return nullptr;

            // for maximized windows, don't return a window if we are not directly on it.
            if (!Fullscreen::controller()->isFullscreen(FS_WINDOW, Fullscreen::FSMODE_MAXIMIZED) || FS_WINDOW->getWindowBoxUnified(properties).containsPoint(pos))
                return Fullscreen::controller()->getFullscreenWindow(PWORKSPACE);
            else
                return nullptr;
        }

        auto found = floating(false);
        if (found)
            return found;

        // for windows, we need to check their extensions too, first.
        for (auto const& w : WINDOWS) {
            if (ONLY_PRIORITY && !w->priorityFocus())
                continue;

            if (special != w->onSpecialWorkspace())
                continue;

            if (!w->m_workspace)
                continue;

            if (!w->m_isX11 && !w->m_isFloating && w->m_isMapped && w->workspaceID() == WSPID && w->acceptsInput() && !w->m_X11ShouldntFocus &&
                !w->m_ruleApplicator->noFocus().valueOrDefault() && w != ignoreWindow && !isShadowedByModal(w)) {
                if (w->hasPopupAt(pos))
                    return w;
            }
        }

        for (auto const& w : WINDOWS) {
            if (ONLY_PRIORITY && !w->priorityFocus())
                continue;

            if (special != w->onSpecialWorkspace())
                continue;

            if (!w->m_workspace)
                continue;

            if (!w->m_isFloating && w->m_isMapped && w->workspaceID() == WSPID && w->acceptsInput() && !w->m_X11ShouldntFocus && !w->m_ruleApplicator->noFocus().valueOrDefault() &&
                w != ignoreWindow && !isShadowedByModal(w)) {
                CBox box = (properties & USE_PROP_TILED) ? w->getWindowBoxUnified(properties) : w->layoutBox();
                if ((properties & INPUT_EXTENTS) && BORDER_GRAB_AREA > 0 && !w->isX11OverrideRedirect()) {
                    const auto WORKAREA                    = PWORKSPACE->m_space->workArea();
                    auto       isWindowCloseToWorkAreaEdge = [&](const Math::eDirection dir) -> bool {
                        constexpr double STICK_THRESHOLD = 2.0; // This constant is taken from isAdjacent in CWindowQuery::inDirection
                        double           aEdge           = -1;
                        double           bEdge           = -1;

                        switch (dir) {
                            case Math::DIRECTION_LEFT:
                                aEdge = WORKAREA.x;
                                bEdge = box.x;
                                break;
                            case Math::DIRECTION_RIGHT:
                                aEdge = WORKAREA.x + WORKAREA.width;
                                bEdge = box.x + box.width;
                                break;
                            case Math::DIRECTION_UP:
                                aEdge = WORKAREA.y;
                                bEdge = box.y;
                                break;
                            case Math::DIRECTION_DOWN:
                                aEdge = WORKAREA.y + WORKAREA.height;
                                bEdge = box.y + box.height;
                                break;
                            default: break;
                        }
                        const double delta = aEdge - bEdge;
                        return std::abs(delta) < STICK_THRESHOLD;
                    };

                    if (isWindowCloseToWorkAreaEdge(Math::eDirection::DIRECTION_LEFT)) {
                        box.x -= BORDER_GRAB_AREA;
                        box.width += BORDER_GRAB_AREA;
                    }

                    if (isWindowCloseToWorkAreaEdge(Math::eDirection::DIRECTION_RIGHT))
                        box.width += BORDER_GRAB_AREA;

                    if (isWindowCloseToWorkAreaEdge(Math::eDirection::DIRECTION_UP)) {
                        box.y -= BORDER_GRAB_AREA;
                        box.height += BORDER_GRAB_AREA;
                    }

                    if (isWindowCloseToWorkAreaEdge(Math::eDirection::DIRECTION_DOWN))
                        box.height += BORDER_GRAB_AREA;
                }
                if (HITBOX_SHRINK > 0 && w != LASTFOCUSED)
                    box = box.copy().expand(-HITBOX_SHRINK);
                if (box.containsPoint(pos))
                    return w;
            }
        }

        return nullptr;
    };

    // special workspace
    if (PMONITOR->m_activeSpecialWorkspace && !*PSPECIALFALLTHRU)
        return windowForWorkspace(true);

    if (PMONITOR->m_activeSpecialWorkspace) {
        const auto PWINDOW = windowForWorkspace(true);

        if (PWINDOW)
            return PWINDOW;
    }

    return windowForWorkspace(false);
}

// Dock keystone (input side): the renderer warps dock-tagged windows into a
// perspective trapezoid via a homography H (OpenGL.cpp ksDockKeystone). Input
// still hits the rectangular surface, so a tap on the visible trapezoid would
// land at the wrong surface point. Undo it: map the box-relative hit through
// H^-1, so pointer / touch / pen-as-pointer follow the transformed window. Same
// inset/shrink config as the renderer; identity for any non-dock/un-tuned window.
static Vector2D ksHitInverse(const PHLWINDOW& window, Vector2D rel, bool missSentinel = true) {
    if (!window->m_ruleApplicator || !window->m_ruleApplicator->m_tagKeeper.isTagged("dock", false))
        return rel;
    static auto PINSET  = CConfigValue<Config::FLOAT>("decoration:keystone_inset");
    static auto PSHRINK = CConfigValue<Config::FLOAT>("decoration:keystone_shrink");
    const float inset = *PINSET, shrink = *PSHRINK;
    if (inset <= 0.0001f && shrink <= 0.0001f)
        return rel;
    const Vector2D sz = window->size(Desktop::View::IGeometric::GEOMETRIC_GOAL);
    if (sz.x <= 0 || sz.y <= 0)
        return rel;
    // forward homography H (unit square -> trapezoid), identical to the renderer
    const float x0 = inset, y0 = shrink, x1 = 1.0f, y1 = 0.0f, x2 = 1.0f, y2 = 1.0f, x3 = inset, y3 = 1.0f - shrink;
    const float sx = x0 - x1 + x2 - x3, sy = y0 - y1 + y2 - y3;
    const float dx1 = x1 - x2, dx2 = x3 - x2, dy1 = y1 - y2, dy2 = y3 - y2;
    const float den = dx1 * dy2 - dx2 * dy1;
    if (den == 0.0f)
        return rel;
    const float g = (sx * dy2 - sy * dx2) / den;
    const float h = (dx1 * sy - dy1 * sx) / den;
    float Hm[9] = {x1 - x0 + g * x1, x3 - x0 + h * x3, x0, y1 - y0 + g * y1, y3 - y0 + h * y3, y0, g, h, 1.0f};
    // the same physical-edge orientation as the renderer (OpenGL.cpp ksOrientToEdge)
    {
        const auto MON  = window->m_monitor.lock();
        const int  EDGE = MON ? (sc<int>(MON->m_transform) % 4) : 0;
        if (EDGE != 0) {
            static const float T[9] = {0, 1, 0, 1, 0, 0, 0, 0, 1};
            static const float M[9] = {-1, 0, 1, 0, 1, 0, 0, 0, 1};
            auto               mul  = [](const float* A, const float* B, float* R) {
                for (int r = 0; r < 3; ++r)
                    for (int cc = 0; cc < 3; ++cc) {
                        float s = 0.0f;
                        for (int k = 0; k < 3; ++k)
                            s += A[r * 3 + k] * B[k * 3 + cc];
                        R[r * 3 + cc] = s;
                    }
            };
            float R[9], Ri[9], tmp[9];
            if (EDGE == 1) {
                for (int k = 0; k < 9; ++k)
                    R[k] = Ri[k] = T[k];
            } else if (EDGE == 2) {
                for (int k = 0; k < 9; ++k)
                    R[k] = Ri[k] = M[k];
            } else {
                mul(T, M, R);
                mul(M, T, Ri);
            }
            mul(Hm, Ri, tmp);
            mul(R, tmp, Hm);
        }
    }
    const float a = Hm[0], b = Hm[1], c = Hm[2];
    const float d = Hm[3], e = Hm[4], ff = Hm[5];
    const float g2 = Hm[6], h2 = Hm[7], i = Hm[8];
    // inverse of H (adjugate / determinant)
    const float det = a * (e * i - ff * h2) - b * (d * i - ff * g2) + c * (d * h2 - e * g2);
    if (det == 0.0f)
        return rel;
    const float ia = (e * i - ff * h2) / det, ib = (c * h2 - b * i) / det, ic = (b * ff - c * e) / det;
    const float id = (ff * g2 - d * i) / det, ie = (a * i - c * g2) / det, iff = (c * d - a * ff) / det;
    const float ig = (d * h2 - e * g2) / det, ih = (b * g2 - a * h2) / det, ii = (a * e - b * d) / det;
    const float uu = rel.x / sz.x, vv = rel.y / sz.y;
    const float ww = ig * uu + ih * vv + ii;
    if (ww == 0.0f)
        return rel;
    const float su = (ia * uu + ib * vv + ic) / ww;
    const float sv = (id * uu + ie * vv + iff) / ww;
    // A hit in the vacated area beside the trapezoid maps outside the surface;
    // signal a miss (sentinel) so the window is not interacted with where nothing
    // is drawn -- otherwise it would clamp to the edge and read as a left offset.
    if (missSentinel && (su < -0.001f || su > 1.001f || sv < -0.001f || sv > 1.001f))
        return Vector2D(-1.0e6f, -1.0e6f);
    return Vector2D(su * sz.x, sv * sz.y);
}

// Dock keystone (input side, X11): the Wayland pointer path runs through windowSurfaceAt(),
// which applies ksHitInverse -- but that function asserts !m_isX11, so X11 dock windows
// (e.g. Wine apps) never got the inverse and their clicks hit the FLAT rectangle: the right
// edge coincides with the trapezoid, the inset left edge does not, so hits drift left. This
// gives InputManager the trapezoid-mapped coord for X11 dock windows; a hit in the vacated
// strip (or any non-dock window) falls back to the original flat coord, so nothing else moves.
// Dock keystone, edge resizing: a global point mapped into the window's UNWARPED frame,
// with no miss sentinel -- points just outside the drawn trapezoid map just outside the
// window box, which is exactly what Hyprland's border-resize test (InputManager) wants.
// Without it the grab zone stayed around the flat box, which is not where the card's
// visible (inset) edges are, so a PiP could not be resized by its edges.
Vector2D CViewHitTester::keystoneUnwarpGlobal(const Vector2D& pos, PHLWINDOW window) const {
    if (!window || !window->m_ruleApplicator || !window->m_ruleApplicator->m_tagKeeper.isTagged("dock", false))
        return pos;
    const auto WPOS = window->position(Desktop::View::IGeometric::GEOMETRIC_GOAL);
    return WPOS + ksHitInverse(window, pos - WPOS, false);
}

Vector2D CViewHitTester::keystoneSurfaceLocal(const Vector2D& pos, PHLWINDOW window) const {
    if (window->m_ruleApplicator && window->m_ruleApplicator->m_tagKeeper.isTagged("dock", false)) {
        // Either a real hit inside the trapezoid, OR the sentinel (x < -1e5) for a hit in the
        // vacated strip beside it. Return the sentinel too (don't fall back to the flat coord):
        // the caller leaves foundSurface null on a sentinel so the empty margin is a clean MISS
        // instead of registering offset. Non-dock windows get the plain flat coord below.
        // A back card drawn shrunk (scale-to-fit, CWindow::fitTransform) maps on into the
        // client's logical surface coords; InputManager applies the X11 factor after.
        auto rel = ksHitInverse(window, pos - window->position(Desktop::View::IGeometric::GEOMETRIC_GOAL));
        if (const auto FIT = window->fitTransform(); FIT.active && rel.x > -1.0e5)
            rel = (rel - FIT.offset) / FIT.scale;
        return rel;
    }
    return pos - window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
}

SP<CWLSurfaceResource> CViewHitTester::windowSurfaceAt(const Vector2D& pos, PHLWINDOW window, Vector2D& surfaceLocal) const {
    if (!validMapped(window))
        return nullptr;

    RASSERT(!window->m_isX11, "Cannot call windowSurfaceAt on an X11 window!");

    // try popups first
    const auto PPOPUP = window->m_popupHead->at(pos);

    if (PPOPUP) {
        const auto OFF = PPOPUP->coordsRelativeToParent();
        surfaceLocal   = pos - window->position(Desktop::View::IGeometric::GEOMETRIC_GOAL) - OFF;
        return PPOPUP->wlSurface()->resource();
    }

    Vector2D ksRel = pos - window->position(Desktop::View::IGeometric::GEOMETRIC_GOAL);
    ksRel          = ksHitInverse(window, ksRel);
    if (ksRel.x < -1.0e5) // keystone: hit landed outside the trapezoid -> a miss
        return nullptr;
    // scale-to-fit: window-box coords -> the client's unscaled surface-tree coords; the
    // letterbox margin around the scaled content is a miss
    if (const auto FIT = window->fitTransform(); FIT.active) {
        ksRel = (ksRel - FIT.offset) / FIT.scale;
        const Vector2D CONTENT = window->wlSurface()->resource()->m_current.size;
        if (ksRel.x < 0 || ksRel.y < 0 || ksRel.x > CONTENT.x || ksRel.y > CONTENT.y)
            return nullptr;
    }
    auto [surf, local] = window->wlSurface()->resource()->at(ksRel, true);
    if (surf) {
        surfaceLocal = local;
        return surf;
    }

    return nullptr;
}

Vector2D CViewHitTester::surfaceLocalAt(const Vector2D& pos, PHLWINDOW window, SP<CWLSurfaceResource> surface) const {
    if (!validMapped(window))
        return {};

    if (window->m_isX11)
        return pos - window->position(Desktop::View::IGeometric::GEOMETRIC_GOAL);

    const auto PPOPUP = window->m_popupHead->at(pos);
    if (PPOPUP)
        return pos - PPOPUP->coordsGlobal();

    std::tuple<SP<CWLSurfaceResource>, Vector2D> iterData = {surface, {-1337, -1337}};

    window->wlSurface()->resource()->breadthfirst(
        [](SP<CWLSurfaceResource> surf, const Vector2D& offset, void* data) {
            const auto PDATA = sc<std::tuple<SP<CWLSurfaceResource>, Vector2D>*>(data);
            if (surf == std::get<0>(*PDATA))
                std::get<1>(*PDATA) = offset;
        },
        &iterData);

    CBox geom = window->m_xdgSurface->m_current.geometry;

    // scale-to-fit: undo the window's scale before taking the surface offset
    Vector2D rel = pos - window->position(Desktop::View::IGeometric::GEOMETRIC_GOAL);
    if (const auto FIT = window->fitTransform(); FIT.active)
        rel = (rel - FIT.offset) / FIT.scale;

    if (std::get<1>(iterData) == Vector2D{-1337, -1337})
        return rel;

    return rel - std::get<1>(iterData) + Vector2D{geom.x, geom.y};
}

SP<CWLSurfaceResource> CViewHitTester::layerPopupSurfaceAt(const Vector2D& pos, PHLMONITOR monitor, Vector2D* surfaceCoords, PHLLS* layerFound) const {
    for (auto const& lsl : monitor->m_layerSurfaceLayers | std::views::reverse) {
        for (auto const& ls : lsl | std::views::reverse) {
            if (!ls->aliveAndVisible())
                continue;

            auto SURFACEAT = ls->m_popupHead->at(pos, true);

            if (SURFACEAT) {
                *layerFound    = ls.lock();
                *surfaceCoords = pos - SURFACEAT->coordsGlobal();
                return SURFACEAT->wlSurface()->resource();
            }
        }
    }

    return nullptr;
}

SP<CWLSurfaceResource> CViewHitTester::layerPopupSurfaceAt(const Vector2D& pos, const std::vector<PHLLSREF>* layerSurfaces, Vector2D* surfaceCoords, PHLLS* layerFound) const {
    for (auto const& ls : *layerSurfaces | std::views::reverse) {
        if (!ls->aliveAndVisible())
            continue;

        auto SURFACEAT = ls->m_popupHead->at(pos, true);

        if (SURFACEAT) {
            *layerFound    = ls.lock();
            *surfaceCoords = pos - SURFACEAT->coordsGlobal();
            return SURFACEAT->wlSurface()->resource();
        }
    }

    return nullptr;
}

SP<CWLSurfaceResource> CViewHitTester::layerSurfaceAt(const Vector2D& pos, std::vector<PHLLSREF>* layerSurfaces, Vector2D* surfaceCoords, PHLLS* layerFound,
                                                      bool aboveLockscreen) const {
    for (auto const& ls : *layerSurfaces | std::views::reverse) {
        if (!ls->aliveAndVisible() || (aboveLockscreen && ls->m_ruleApplicator->aboveLock().valueOrDefault() != 2))
            continue;

        auto [surf, local] = ls->m_layerSurface->m_surface->at(pos - ls->m_geometry.pos(), true);

        if (surf) {
            if (!surf->m_current.inputIsInfinite && surf->m_current.input.empty())
                continue;

            *layerFound = ls.lock();

            *surfaceCoords = local;

            return surf;
        }
    }

    return nullptr;
}
