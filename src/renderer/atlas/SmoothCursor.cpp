// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "SmoothCursor.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Microsoft::Console::Render::Atlas;

// Simulates a critically damped spring (also known as a PD controller) just like Neovide does:
// https://gdcvault.com/play/1027059/Math-In-Game-Development-Summit
// Returns true if the spring is still moving.
static bool stepSpring(f32& position, f32& velocity, f32 dt, f32 animationLength) noexcept
{
    if (animationLength <= dt)
    {
        position = 0;
        velocity = 0;
        return false;
    }
    if (position == 0)
    {
        return false;
    }

    // < 1 underdamped, 1 critically damped, > 1 overdamped
    static constexpr f32 zeta = 1.0f;
    // omega is chosen like in Neovide: About 9% of the distance is left once animationLength has passed
    // ((1 + 4) * e^-4 when starting at rest). The spring keeps decaying until it's below 0.01 pixel.
    const auto omega = 4.0f / (zeta * animationLength);

    // The analytical solution of a critically damped harmonic oscillator.
    // a and b are the initial conditions, obtained by setting dt to zero and solving for position and velocity.
    const auto a = position;
    const auto b = position * omega + velocity;
    const auto c = std::exp(-omega * dt);

    position = (a + b * dt) * c;
    velocity = c * (-a * omega - b * dt * omega + b);

    if (std::abs(position) < 0.01f)
    {
        position = 0;
        velocity = 0;
        return false;
    }
    return true;
}

bool SmoothCursor::Active() const noexcept
{
    return _active;
}

void SmoothCursor::Reset() noexcept
{
    _valid = false;
    _active = false;
}

void SmoothCursor::Step() noexcept
{
    // Stops us from teleporting if we haven't rendered in a while (e.g. due to a hidden window).
    static constexpr f32 maxStep = 0.05f;

    const auto now = std::chrono::steady_clock::now();
    const auto dt = std::min(std::chrono::duration<f32>(now - _lastStep).count(), maxStep);
    _lastStep = now;

    auto animating = false;

    for (size_t i = 0; i < 4; ++i)
    {
        auto& corner = _corners[i];
        for (size_t axis = 0; axis < 2; ++axis)
        {
            auto& spring = corner.spring[axis];
            animating |= stepSpring(spring.position, spring.velocity, dt, corner.animationLength);
            corner.current[axis] = static_cast<f32>(_target[i * 2 + axis]) - spring.position;
        }
    }

    _active = animating;
}

void SmoothCursor::MoveTo(const std::array<i32, 8>& target, u16x2 cellSize, bool shear, u32 durationMs) noexcept
{
    // Whether the corners arrive separately (= shear) is up to the user.
    _shear = shear;

    if (!_valid || _cellSize != cellSize)
    {
        // Start (or restart) at the target without animating.
        for (size_t i = 0; i < 4; ++i)
        {
            _corners[i] = {};
            for (size_t axis = 0; axis < 2; ++axis)
            {
                const auto destination = static_cast<f32>(target[i * 2 + axis]);
                _corners[i].current[axis] = destination;
                _corners[i].previousDestination[axis] = destination;
            }
        }
        _target = target;
        _cellSize = cellSize;
        _valid = true;
        _active = false;
    }
    else if (_target != target)
    {
        _retarget(target, durationMs);
        _target = target;
        _lastStep = std::chrono::steady_clock::now();
        _active = true;
        // Take the first step right away (with a dt of about zero). Corners that have an animation length
        // of 0 (the leading ones, if shearing) snap to their destination, while the others stay behind.
        // Otherwise this frame would still show the cursor at its old position, and the stretched
        // cursor would only appear in the next frame, which cuts off the start of the trail.
        Step();
    }
}

SmoothCursorFrame SmoothCursor::Frame() const noexcept
{
    std::array<i32, 8> c;
    for (size_t i = 0; i < 8; ++i)
    {
        c[i] = static_cast<i32>(std::lround(_corners[i / 2].current[i % 2]));
    }

    // The quad is described by its bounding box and how far the corners are shifted away from it:
    // The bounding box's left edge is formed by the top-left/bottom-left corners, its right edge by
    // the top-right/bottom-right ones, and so on (see LayoutSmoothCursor).
    return {
        .enabled = true,
        .shear = _shear,
        .rect = {
            std::min(c[0], c[6]),
            std::min(c[1], c[3]),
            std::max(c[2], c[4]),
            std::max(c[5], c[7]),
        },
        .corners = c,
        // Everything the quad might touch (including its anti-aliased edges),
        // in case it isn't perfectly described by the bounding box above.
        .damage = {
            std::min({ c[0], c[2], c[4], c[6] }) - 1,
            std::min({ c[1], c[3], c[5], c[7] }) - 1,
            std::max({ c[0], c[2], c[4], c[6] }) + 1,
            std::max({ c[1], c[3], c[5], c[7] }) + 1,
        },
    };
}

// Called when the destination of the cursor changes. Gives each corner an animation length,
// depending on how much it lines up with the direction we're heading in (this is Neovide's Corner::jump).
// Then it turns the distance each corner still has to travel into the offset of its springs.
void SmoothCursor::_retarget(const std::array<i32, 8>& target, u32 durationMs) noexcept
{
    // The time (in seconds) the slowest corner takes to get close to its destination.
    const auto animationLength = static_cast<f32>(durationMs) * 0.001f;
    // Jumps of at most 2 columns within a row (typically when typing or holding a key) use this length,
    // if it's shorter, so they feel snappy instead of smeared.
    static constexpr f32 shortAnimationLength = 0.04f;
    // 1 = The leading corners arrive immediately (maximum shear). 0 = All corners take equally long (no shear).
    const auto trailSize = _shear ? 1.0f : 0.0f;

    // The size of the cursor's cell box, in which jumps are measured.
    const auto width = std::max(static_cast<f32>(target[2] - target[0]), 1.0f);
    const auto height = std::max(static_cast<f32>(target[5] - target[3]), 1.0f);

    // The corners' offsets from the center of the cursor, normalized: (-1,-1), (1,-1), (1,1), (-1,1).
    static constexpr f32 sqrtHalf = 0.70710678f;
    static constexpr std::array<f32, 4> offsetX{ -sqrtHalf, sqrtHalf, sqrtHalf, -sqrtHalf };
    static constexpr std::array<f32, 4> offsetY{ -sqrtHalf, -sqrtHalf, sqrtHalf, sqrtHalf };

    // How much a corner points into the direction of travel (-1 to 1). Corners that lead move faster than those that trail.
    // A corner that doesn't move (e.g. the top-left one when the cursor grows to the right) results in NaN.
    std::array<f32, 4> alignment{};
    auto minAlignment = std::numeric_limits<f32>::infinity();
    auto maxAlignment = -std::numeric_limits<f32>::infinity();
    for (size_t i = 0; i < 4; ++i)
    {
        const auto travelX = static_cast<f32>(target[i * 2 + 0]) - _corners[i].previousDestination[0];
        const auto travelY = static_cast<f32>(target[i * 2 + 1]) - _corners[i].previousDestination[1];
        const auto travelLength = std::sqrt(travelX * travelX + travelY * travelY);
        alignment[i] = travelLength > 0.0f ? (offsetX[i] * travelX + offsetY[i] * travelY) / travelLength : std::numeric_limits<f32>::quiet_NaN();
        // fmin/fmax ignore NaN.
        minAlignment = std::fmin(minAlignment, alignment[i]);
        maxAlignment = std::fmax(maxAlignment, alignment[i]);
    }
    const auto alignmentRange = maxAlignment - minAlignment;

    for (size_t i = 0; i < 4; ++i)
    {
        auto& corner = _corners[i];
        const auto jumpX = (static_cast<f32>(target[i * 2 + 0]) - corner.previousDestination[0]) / width;
        const auto jumpY = (static_cast<f32>(target[i * 2 + 1]) - corner.previousDestination[1]) / height;

        if (std::abs(jumpX) <= 2.001f && std::abs(jumpY) <= 0.001f)
        {
            corner.animationLength = std::min(animationLength, shortAnimationLength);
        }
        else
        {
            auto normalized = (alignment[i] - minAlignment) / alignmentRange;
            normalized = std::isfinite(normalized) ? std::clamp(normalized, 0.0f, 1.0f) : 1.0f;
            const auto leading = animationLength * std::clamp(1.0f - trailSize, 0.0f, 1.0f);
            const auto trailing = animationLength;
            corner.animationLength = trailing + (leading - trailing) * normalized;
        }

        // The spring offset is the distance that's left to travel. Its velocity is retained,
        // which is what makes the cursor change course smoothly if it's retargeted midway.
        for (size_t axis = 0; axis < 2; ++axis)
        {
            const auto destination = static_cast<f32>(target[i * 2 + axis]);
            corner.spring[axis].position = destination - corner.current[axis];
            corner.previousDestination[axis] = destination;
        }
    }
}

void Microsoft::Console::Render::Atlas::LayoutSmoothCursor(const SmoothCursorFrame& frame, til::small_vector<CursorRect, 6>& rects, til::rect& position, til::small_vector<SmoothCursorQuad, 6>& quads)
{
    quads.clear();

    if (rects.empty())
    {
        return;
    }

    if (frame.shear)
    {
        // A smooth cursor with shear is a quad whose corners arrive individually. The animated quad
        // is that of the entire cell(s) the cursor is targeting (top-left, top-right, bottom-right, bottom-left).
        // Every part of the cursor (a bar, an underscore, a block, ...) is a rectangle inside of that cell.
        // We map the 4 corners of each part into the animated quad, which gives us a quad for each of them.
        const auto& k = frame.corners;
        const auto targetLeft = static_cast<f32>(position.left);
        const auto targetTop = static_cast<f32>(position.top);
        const auto targetWidth = static_cast<f32>(position.right - position.left);
        const auto targetHeight = static_cast<f32>(position.bottom - position.top);

        // Maps a position inside the cell (u and v are between 0 and 1) onto the animated quad.
        const auto map = [&](f32 u, f32 v) {
            const auto topX = static_cast<f32>(k[0]) + static_cast<f32>(k[2] - k[0]) * u;
            const auto topY = static_cast<f32>(k[1]) + static_cast<f32>(k[3] - k[1]) * u;
            const auto bottomX = static_cast<f32>(k[6]) + static_cast<f32>(k[4] - k[6]) * u;
            const auto bottomY = static_cast<f32>(k[7]) + static_cast<f32>(k[5] - k[7]) * u;
            return std::array<i32, 2>{
                static_cast<i32>(std::lround(topX + (bottomX - topX) * v)),
                static_cast<i32>(std::lround(topY + (bottomY - topY) * v)),
            };
        };
        const auto shearByte = [](i32 v) { return static_cast<u16>(static_cast<u8>(std::clamp(v, -127, 127))); };

        const auto parts = rects;
        rects.clear();
        til::rect bounds{ til::CoordTypeMax, til::CoordTypeMax, til::CoordTypeMin, til::CoordTypeMin };

        for (const auto& part : parts)
        {
            const auto u0 = (static_cast<f32>(part.position.x) - targetLeft) / targetWidth;
            const auto u1 = (static_cast<f32>(part.position.x + part.size.x) - targetLeft) / targetWidth;
            const auto v0 = (static_cast<f32>(part.position.y) - targetTop) / targetHeight;
            const auto v1 = (static_cast<f32>(part.position.y + part.size.y) - targetTop) / targetHeight;
            const auto tl = map(u0, v0);
            const auto tr = map(u1, v0);
            const auto br = map(u1, v1);
            const auto bl = map(u0, v1);

            // The quad is described by its bounding box and how far the corners are shifted away from it:
            // The left edge of the box is formed by the top-left/bottom-left corners, the right edge by the
            // top-right/bottom-right ones, the top edge by the top-left/top-right ones, and so on.
            const auto left = std::min(tl[0], bl[0]);
            const auto top = std::min(tl[1], tr[1]);
            const auto right = std::max(tr[0], br[0]);
            const auto bottom = std::max(bl[1], br[1]);
            const auto dl = std::clamp(tl[0] - bl[0], -127, 127);
            const auto dr = std::clamp(tr[0] - br[0], -127, 127);
            const auto dt = std::clamp(tr[1] - tl[1], -127, 127);
            const auto db = std::clamp(br[1] - bl[1], -127, 127);

            quads.emplace_back(
                i16x2{ static_cast<i16>(left), static_cast<i16>(top) },
                u16x2{ static_cast<u16>(std::max(right - left, 1)), static_cast<u16>(std::max(bottom - top, 1)) },
                u16x2{
                    static_cast<u16>(shearByte(dl) | (shearByte(dr) << 8)),
                    static_cast<u16>(shearByte(dt) | (shearByte(db) << 8)),
                },
                part.background);

            bounds.left = std::min(bounds.left, left);
            bounds.top = std::min(bounds.top, top);
            bounds.right = std::max(bounds.right, right);
            bounds.bottom = std::max(bounds.bottom, bottom);

            // Text can only be inverted inside of rectangles. We use the largest axis-aligned rectangle that's fully
            // inside the quad: Each side is moved inwards by how far the corners on that side are shifted apart.
            const auto innerLeft = left + std::abs(dl);
            const auto innerTop = top + std::abs(dt);
            const auto innerRight = right - std::abs(dr);
            const auto innerBottom = bottom - std::abs(db);

            if (innerLeft < innerRight && innerTop < innerBottom)
            {
                rects.emplace_back(
                    i16x2{ static_cast<i16>(innerLeft), static_cast<i16>(innerTop) },
                    u16x2{ static_cast<u16>(innerRight - innerLeft), static_cast<u16>(innerBottom - innerTop) },
                    part.background,
                    part.foreground);
            }
        }

        position = {
            std::max(bounds.left, 0),
            std::max(bounds.top, 0),
            std::max(bounds.right, 0),
            std::max(bounds.bottom, 0),
        };
    }
    else
    {
        // Map the cell box the cursor has been laid out in onto the animated box. This way every
        // cursor shape, from the thin bar to the underscore, animates.
        const auto& g = frame.rect;
        const int64_t targetLeft = position.left;
        const int64_t targetTop = position.top;
        const int64_t targetWidth = position.right - position.left;
        const int64_t targetHeight = position.bottom - position.top;
        const int64_t smoothWidth = g.right - g.left;
        const int64_t smoothHeight = g.bottom - g.top;

        const auto mapX = [&](int64_t x) { return static_cast<i32>(g.left + (x - targetLeft) * smoothWidth / targetWidth); };
        const auto mapY = [&](int64_t y) { return static_cast<i32>(g.top + (y - targetTop) * smoothHeight / targetHeight); };

        for (auto& c : rects)
        {
            const auto left = mapX(c.position.x);
            const auto top = mapY(c.position.y);
            const auto right = mapX(c.position.x + c.size.x);
            const auto bottom = mapY(c.position.y + c.size.y);
            c.position = { static_cast<i16>(left), static_cast<i16>(top) };
            c.size = { static_cast<u16>(std::max(right - left, 1)), static_cast<u16>(std::max(bottom - top, 1)) };
        }

        position = {
            std::max(g.left, 0),
            std::max(g.top, 0),
            std::max(g.right, 0),
            std::max(g.bottom, 0),
        };
    }
}
