// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "shader_common.hlsl"

cbuffer ConstBuffer : register(b0)
{
    float2 positionScale;
}

// clang-format off
PSData main(VSData data)
// clang-format on
{
    PSData output;
    output.color = data.color;
    output.shadingType = data.shadingType;
    output.renditionScale = data.renditionScale;
    // positionScale is expected to be float2(2.0f / sizeInPixel.x, -2.0f / sizeInPixel.y). Together with the
    // addition below this will transform our "position" from pixel into normalized device coordinate (NDC) space.
    output.position.xy = (data.position + data.vertex.xy * data.size) * positionScale + float2(-1.0f, 1.0f);
    output.position.zw = float2(0, 1);
    output.texcoord = data.texcoord + data.vertex.xy * data.size;
    output.corners0 = float4(0, 0, 0, 0);
    output.corners1 = float4(0, 0, 0, 0);

    if (data.shadingType == SHADING_TYPE_CURSOR_QUAD)
    {
        // For the cursor quad the texcoord isn't a texture coordinate. It contains the following
        // 4 signed bytes which describe how far the quad's corners are shifted away from the instance's
        // bounding box (see BackendD3D::_drawCursorBackground):
        //   x = (left, right), y = (top, bottom)
        // - left:   top-left.x  - bottom-left.x
        // - right:  top-right.x - bottom-right.x
        // - top:    top-right.y - top-left.y
        // - bottom: bottom-right.y - bottom-left.y
        // Sign extension of the lower 8 bits: shift them to the top of an int, then shift them back.
        int4 d = asint(uint4(data.texcoord.x, data.texcoord.x >> 8, data.texcoord.y, data.texcoord.y >> 8) << 24) >> 24;
        float2 size = data.size;

        // top-left, top-right
        output.corners0 = float4(max(d.x, 0), max(-d.z, 0), size.x + min(d.y, 0), max(d.z, 0));
        // bottom-right, bottom-left
        output.corners1 = float4(size.x - max(d.y, 0), size.y + min(d.w, 0), max(-d.x, 0), size.y - max(d.w, 0));
        // The pixel shader needs the position relative to the top-left of the instance.
        output.texcoord = data.vertex.xy * size;
    }

    return output;
}
