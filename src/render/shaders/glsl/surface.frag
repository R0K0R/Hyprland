#version 300 es
#define ALLOW_INCLUDES
#extension GL_ARB_shading_language_include : enable

#include "defines.h"

precision         highp float;
in vec2           v_texcoord;
in vec2           v_boxUV; // keystone: unit-quad pos (0..1), warped to the trapezoid
uniform sampler2D tex;
#if USE_BLUR
uniform vec2      uvSize;
uniform vec2      uvOffset;
uniform sampler2D blurredBG;
uniform float     blurAlpha;
#endif
#if USE_BLUR_MATTE
uniform sampler2D blurAlphaMatte;
#endif

uniform float alpha;

// keystone dock windows: anti-aliased rounded-corner coverage. keystoneRound is the
// corner radius in the same (logical px) units as keystoneSize; <= 0.5 disables it (all
// non-dock windows, set to 0 from OpenGL.cpp). See keystoneCoverage() below.
uniform float keystoneRound;
uniform vec2  keystoneSize;
// keystone frost pass: > 0 = sample tex at gl_FragCoord / ksScreenUV (the framebuffer size)
// rather than at v_texcoord. tex is then the blurred copy of this same framebuffer, so each
// pixel of the warped card gets the blur of what is really behind it, not of the flat box.
uniform vec2  ksScreenUV;

// Signed-distance rounded-rect coverage in window-local space. v_boxUV (0..1) is the
// surface's unit quad, warped to the trapezoid by the vertex shader's proj, so the
// rounded + feathered edge follows the 3D shape instead of the flat box. fwidth() gives
// a ~1px screen-space feather (the anti-aliasing) regardless of the warp. Applied to BOTH
// the window texture and the separate frosted-blur pass, so their edges coincide.
float keystoneCoverage() {
    if (keystoneRound <= 0.5)
        return 1.0;
    vec2  p = v_boxUV * keystoneSize;
    vec2  h = keystoneSize * 0.5;
    vec2  q = abs(p - h) - (h - keystoneRound);
    float d = min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - keystoneRound;
    float aa = max(fwidth(d), 1.0e-4); // ~1px screen feather; guard degenerate frags
    return 1.0 - smoothstep(-aa, aa, d);
}

// keystone dock windows: the perspective warp minifies + shears the texture, so a single
// bilinear tap (no mipmaps) aliases the INNER CONTENT and the warped edges (the diagonal
// stair-stepping). Average a rotated 2x2 grid across the fragment's texcoord footprint
// (dFdx/dFdy give the exact footprint) -> a cheap ~4x supersample. Gated at the call site by
// keystoneSize>0 (dock windows only), so every normal window keeps its single fast sample.
vec4 ksSampleAA(sampler2D t, vec2 uv) {
    vec2 dx = dFdx(uv);
    vec2 dy = dFdy(uv);
    return 0.25 * (texture(t, uv - 0.375 * dx - 0.125 * dy) + texture(t, uv - 0.125 * dx + 0.375 * dy) +
                   texture(t, uv + 0.375 * dx + 0.125 * dy) + texture(t, uv + 0.125 * dx - 0.375 * dy));
}

#if USE_DISCARD
uniform bool  discardOpaque;
uniform bool  discardAlpha;
uniform float discardAlphaValue;
#endif

#if USE_TINT
uniform vec3 tint;
#endif

#if USE_ROUNDING
uniform float radius;
uniform float roundingPower;
uniform vec2  topLeft;
uniform vec2  fullSize;
#include "rounding.glsl"
#else
const float radius        = 0.0;
const float roundingPower = 2.0;
#endif

#if USE_MOTION_BLUR
uniform vec4  motionPrevBox;
uniform vec4  motionCurrBox;
uniform vec4  motionSourceBox;
uniform vec2  motionSourceTexSize;
uniform int   motionSamples;
#include "motion_blur.glsl"
#endif

#if USE_CM
uniform int sourceTF; // eTransferFunction
uniform int targetTF; // eTransferFunction

#if USE_TONEMAP || USE_SDR_MOD
uniform mat3 targetPrimariesXYZ;
#else
const mat3 targetPrimariesXYZ = mat3(0.0);
#endif

#include "CM.glsl"
#endif

layout(location = 0) out vec4 fragColor;
#if USE_MIRROR
layout(location = 1) out vec4 mirrorColor;
#endif
void main() {
#if USE_MOTION_BLUR
    vec4 pixColor = motionBlurSample(tex, motionPrevBox, motionCurrBox, motionSourceBox, motionSourceTexSize, motionSamples, USE_RGBA == 1);
#if USE_BLUR_MATTE
    float blurAlphaMask = clamp(motionBlurSample(blurAlphaMatte, motionPrevBox, motionCurrBox, motionSourceBox, motionSourceTexSize, motionSamples, true).r, 0.0, 1.0);
#endif
#else
#if USE_RGBA
    vec4 pixColor = (ksScreenUV.x > 0.5) ? texture(tex, gl_FragCoord.xy / ksScreenUV) :
        (keystoneSize.x > 0.5) ? ksSampleAA(tex, v_texcoord) : texture(tex, v_texcoord);
#else
    vec4 pixColor = vec4(((ksScreenUV.x > 0.5) ? texture(tex, gl_FragCoord.xy / ksScreenUV) :
        (keystoneSize.x > 0.5) ? ksSampleAA(tex, v_texcoord) : texture(tex, v_texcoord)).rgb, 1.0);
#endif
#if USE_BLUR_MATTE
    float blurAlphaMask = clamp(texture(blurAlphaMatte, v_texcoord).r, 0.0, 1.0);
#endif
#endif

#if USE_DISCARD && !USE_BLUR
    if (discardOpaque && pixColor.a * alpha == 1.0)
        discard;

    if (discardAlpha && pixColor.a <= discardAlphaValue)
        discard;
#endif

#if USE_CM
#if USE_MIRROR
    vec4[2] pixColors =
#else
    pixColor =
#endif
        doColorManagement(pixColor, alpha, sourceTF, targetTF, convertMatrix, srcTFRange, dstTFRange
#if USE_ICC
                          ,
                          iccLut3D, iccLutSize
#else
#if USE_TONEMAP || USE_SDR_MOD
                          ,
                          targetPrimariesXYZ
#endif
#if USE_TONEMAP
                          ,
                          maxLuminance, dstMaxLuminance, dstRefLuminance, srcRefLuminance, tonemapMode
#endif
#if USE_SDR_MOD
                          ,
                          sdrSaturation, sdrBrightnessMultiplier
#endif
#endif
        );
#endif
#if USE_MIRROR
#if USE_CM
    pixColor    = pixColors[0];
    mirrorColor = pixColors[1];
#else
    mirrorColor = pixColor;
#endif
#endif

#if USE_TINT
    pixColor.rgb = pixColor.rgb * tint;
#endif

#if USE_ROUNDING && !USE_MOTION_BLUR
    pixColor = rounding(pixColor, radius, roundingPower, topLeft, fullSize);
#endif
#if !USE_CM
    pixColor *= alpha;
#endif
#if USE_BLUR
#if USE_BLUR_MATTE
    float pixBlurAlphaMask = blurAlphaMask * blurAlpha;
#if USE_DISCARD
    if (discardAlpha && pixColor.a <= discardAlphaValue)
        pixBlurAlphaMask = 0.0;
#endif
    vec3 blurredPixColor = mix(texture(blurredBG, v_texcoord * uvSize + uvOffset).rgb, pixColor.rgb, pixColor.a);
    pixColor             = vec4(mix(pixColor.rgb, blurredPixColor, pixBlurAlphaMask), max(pixColor.a, pixBlurAlphaMask));
#else
#if USE_BLUR_ALPHA_MASK
    if (pixColor.a <= 0.0)
        discard;
#endif
#if USE_DISCARD
    pixColor = mix(pixColor, vec4(mix(texture(blurredBG, v_texcoord * uvSize + uvOffset).rgb, pixColor.rgb, pixColor.a), 1.0),
                   discardAlpha && (pixColor.a <= discardAlphaValue) ? 0.0 : 1.0);
#else
    pixColor = vec4(mix(texture(blurredBG, v_texcoord * uvSize + uvOffset).rgb, pixColor.rgb, pixColor.a), 1.0);
#endif
#endif
#endif

    float ksCov = keystoneCoverage();
    // DISCARD the vacated corners (coverage ~0) outright. An OPAQUE window (easyeffects, XRGB
    // GTK4) is drawn with GL blending OFF, so multiplying its alpha by the coverage does nothing
    // -> square corners. discard removes the fragment regardless of blend state, so the rounded
    // corner shows the backdrop. Blended (ARGB) windows still get the alpha feather below for AA;
    // discarding only the fully-outside pixels keeps their smooth edge.
    if (keystoneRound > 0.5 && ksCov < 0.004)
        discard;
    pixColor *= ksCov;
    fragColor = pixColor;
#if USE_MIRROR
#if USE_TINT
    mirrorColor.rgb = mirrorColor.rgb * tint;
#endif

#if USE_ROUNDING && !USE_MOTION_BLUR
    mirrorColor = rounding(mirrorColor, radius, roundingPower, topLeft, fullSize);
#endif
#if !USE_CM
    mirrorColor *= alpha;
#endif
#if USE_BLUR
#if USE_BLUR_MATTE
    float mirrorBlurAlphaMask = blurAlphaMask * blurAlpha;
#if USE_DISCARD
    if (discardAlpha && mirrorColor.a <= discardAlphaValue)
        mirrorBlurAlphaMask = 0.0;
#endif
    vec3 blurredMirrorColor = mix(texture(blurredBG, v_texcoord * uvSize + uvOffset).rgb, mirrorColor.rgb, mirrorColor.a);
    mirrorColor             = vec4(mix(mirrorColor.rgb, blurredMirrorColor, mirrorBlurAlphaMask), max(mirrorColor.a, mirrorBlurAlphaMask));
#else
#if USE_BLUR_ALPHA_MASK
    if (mirrorColor.a > 0.0) {
#endif
#if USE_DISCARD
        mirrorColor = mix(mirrorColor, vec4(mix(texture(blurredBG, v_texcoord * uvSize + uvOffset).rgb, mirrorColor.rgb, mirrorColor.a), 1.0),
                          discardAlpha && (mirrorColor.a <= discardAlphaValue) ? 0.0 : 1.0);
#else
        mirrorColor = vec4(mix(texture(blurredBG, v_texcoord * uvSize + uvOffset).rgb, mirrorColor.rgb, mirrorColor.a), 1.0);
#endif
#if USE_BLUR_ALPHA_MASK
    } else
        mirrorColor = vec4(0.0);
#endif
#endif
#endif

    mirrorColor *= ksCov;
#endif
}
