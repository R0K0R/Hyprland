#include "OverviewGesture.hpp"

#include "../../../../render/Renderer.hpp"

// Toggle on gesture begin, so a single 5-finger swipe (either direction) opens or closes the
// overview immediately -- the "5-finger transitions" feel. update/end are intentionally no-ops
// (v1 has no live-drag reveal). The snapshot + render + close all live in the renderer.
void COverviewTrackpadGesture::begin(const ITrackpadGesture::STrackpadGestureBegin& e) {
    ITrackpadGesture::begin(e);
    if (g_pHyprRenderer)
        g_pHyprRenderer->ksOverviewToggle();
}

void COverviewTrackpadGesture::update(const ITrackpadGesture::STrackpadGestureUpdate&) {
    ;
}

void COverviewTrackpadGesture::end(const ITrackpadGesture::STrackpadGestureEnd&) {
    ;
}
