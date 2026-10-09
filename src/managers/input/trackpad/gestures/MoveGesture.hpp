#pragma once

#include "ITrackpadGesture.hpp"

#include "../../../../desktop/DesktopTypes.hpp"

class CMoveTrackpadGesture : public ITrackpadGesture {
  public:
    CMoveTrackpadGesture()          = default;
    virtual ~CMoveTrackpadGesture() = default;

    virtual void begin(const ITrackpadGesture::STrackpadGestureBegin& e);
    virtual void update(const ITrackpadGesture::STrackpadGestureUpdate& e);
    virtual void end(const ITrackpadGesture::STrackpadGestureEnd& e);

  private:
    PHLWINDOWREF m_window;
    Vector2D     m_lastDelta;
    // began on a side-dock pile card: the swipe slides the pile, gestures:dock_swipe_exec settles it
    struct SDockCard {
        PHLWINDOWREF window;
        int          depth   = 0;   // dockd<N>: 0 = front
        double       applied  = 0.0; // canonical offset already applied by this swipe, toward the edge
        double       appliedY = 0.0; // ...and along it (only the front, once it is being pulled out)
    };
    bool                   m_dockSwipe     = false;
    bool                   m_dockFromTouch = false; // deltas are logical (touchscreen), not physical (touchpad)
    int                    m_dockEdge      = 0;     // monitor transform % 4: which logical edge is the panel's right
    int                    m_dockMaxDepth  = 0;
    Vector2D               m_dockTravel;            // canonical: x toward the dock edge, y along it
    double                 m_dockExtent = 0.0;      // the monitor's length along canonical x, logical px
    std::vector<SDockCard> m_dockCards;
    PHLWINDOWREF           m_dockFront;
};
