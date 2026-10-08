// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <array>
#include <chrono>

#include "common.h"

namespace Microsoft::Console::Render::Atlas
{
    // A rectangle that is part of the cursor (e.g. one of the lines of the empty box cursor) in pixel.
    struct CursorRect
    {
        i16x2 position;
        u16x2 size;
        u32 background;
        u32 foreground;
    };

    // A quad of the sheared smooth cursor. position and size are its bounding box in pixel.
    // shear contains 4 signed bytes that describe how far the corners are shifted away from that box
    // (left, right in x and top, bottom in y). See shader_vs.hlsl.
    struct SmoothCursorQuad
    {
        i16x2 position;
        u16x2 size;
        u16x2 shear;
        u32 color;
    };

    // The state of the smooth cursor animation. It's a port of Neovide's cursor renderer:
    // Each of the 4 corners of the cursor's cell box is pulled towards its destination by two
    // critically damped springs (one per axis). When the cursor moves, every corner is given
    // an animation length depending on how well it lines up with the direction of travel:
    // Leading corners arrive (almost) immediately while trailing ones take the full duration.
    // This stretches and shears the cursor while it moves and makes it contract again once it arrives.
    // Without shear all corners use the same length and the cursor just slides.
    //
    // This class doesn't know anything about rendering. AtlasEngine feeds it the cell box the cursor
    // should be at and gets the animated corners back (see Frame()), which it hands to the backend
    // via RenderingPayload::smoothCursor. This is only accessed by the thread that calls StartPaint(),
    // PaintCursor(), etc.
    class SmoothCursor
    {
    public:
        // True if the corners haven't reached their destination yet.
        bool Active() const noexcept;
        // Forgets the position. The next MoveTo() places the cursor without animating.
        void Reset() noexcept;
        // Advances the animation by the time since the last call.
        void Step() noexcept;
        // Sets the cell box the cursor should be at (in pixel: top-left, top-right, bottom-right, bottom-left).
        // The cursor snaps to it the first time and if the cell size changed, otherwise it animates.
        // shear lets the corners arrive separately. durationMs is Neovide's animation_length.
        void MoveTo(const std::array<i32, 8>& target, u16x2 cellSize, bool shear, u32 durationMs) noexcept;
        // The current (animated) shape of the cursor.
        SmoothCursorFrame Frame() const noexcept;

    private:
        // The offset between a corner's position and its destination in pixel,
        // which the spring drives towards zero. The velocity is kept when retargeting.
        struct Spring
        {
            f32 position = 0;
            f32 velocity = 0;
        };
        struct Corner
        {
            // x, y in pixel.
            std::array<f32, 2> current{};
            std::array<f32, 2> previousDestination{};
            std::array<Spring, 2> spring{};
            // How long (in seconds) the corner takes to get close to its destination (about 9% are left).
            f32 animationLength = 0;
        };

        void _retarget(const std::array<i32, 8>& target, u32 durationMs) noexcept;

        // Corners are ordered top-left, top-right, bottom-right, bottom-left.
        std::array<Corner, 4> _corners{};
        // The destinations of the corners (x, y in pixel).
        std::array<i32, 8> _target{};
        u16x2 _cellSize{};
        std::chrono::steady_clock::time_point _lastStep;
        // True if _corners contains a meaningful value.
        bool _valid = false;
        bool _active = false;
        // True if the corners arrive separately (shear) while the cursor moves.
        bool _shear = false;
    };

    // The backend lays out the cursor in the cell(s) it is at (this is cursorRect), the same way as if
    // it wasn't animated. This maps these rectangles into the animated cursor, so that every cursor shape
    // (bar, underscore, box, ...) animates:
    // - Without shear the rects are moved and scaled into frame.rect.
    // - With shear every rect becomes a quad and `quads` receives it. The text is inverted using rectangles
    //   only, so `rects` is replaced with the largest rectangle inside of each quad.
    // `position` (the bounding box of `rects`) is updated to the animated one in both cases.
    void LayoutSmoothCursor(const SmoothCursorFrame& frame, til::small_vector<CursorRect, 6>& rects, til::rect& position, til::small_vector<SmoothCursorQuad, 6>& quads);
}
