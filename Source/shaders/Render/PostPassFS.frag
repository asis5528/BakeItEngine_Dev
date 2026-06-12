#version 450
#extension GL_EXT_multiview : enable

// Post pass: scene passthrough + brush indicator overlay.
// Multiview-aware: per-eye matrices indexed with gl_ViewIndex (0 on desktop,
// where the pass renders without a multiview render pass).
// Two cooperating cues, both ray-traced/evaluated against the same
// world-space brush the stamp uses:
//  1. Shape outline (cube/plane = wireframe edges, sphere = silhouette
//     ring), depth-tested against the resolved scene depth: bright in front
//     of the surface, faint ghost behind it.
//  2. Surface footprint: the brush SDF evaluated at the reconstructed
//     scene-surface position - the surface region inside the brush volume is
//     tinted and its boundary gets a contour line. A visible footprint patch
//     means the brush intersects the surface there; no patch, no contact.

layout(location = 0) in vec2 texcoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform IndicatorUbo {
    mat4 invProj[2];        // per eye; GL-convention NDC on every platform
    mat4 invView[2];
    vec4 camPos[2];         // xyz: eye position WS
    vec4 brushCenterRadius; // xyz: brush center WS, w: effective radius
    vec4 brushParams;       // x: shape (0 cube, 1 sphere, 2 plane), y: visible, z: action (1 add, 0 remove)
    vec4 brushColor;        // rgb: brush color
    vec4 hudA;              // x: show HUD, y: fps, z: used bricks, w: atlas usage %
    vec4 hudB;              // x: radius*1000, y: smoothness*10000
} ubo;

layout(binding = 1) uniform sampler2DArray srcColor;
layout(binding = 2) uniform sampler2DArray sceneDepth;

// Wireframe factor of an axis-aligned box: 1 on the edges of the hit face.
// No early-outs: fwidth needs uniform control flow, misses are masked.
// tHit receives the ray distance of the outline point (for depth testing).
float boxOutline(vec3 ro, vec3 rd, vec3 c, vec3 he, out float tHit)
{
    vec3 s = step(0.0, rd) * 2.0 - 1.0;
    vec3 inv = s / max(abs(rd), vec3(1e-6));
    vec3 t0 = (c - he - ro) * inv;
    vec3 t1 = (c + he - ro) * inv;
    vec3 tmin = min(t0, t1);
    vec3 tmax = max(t0, t1);
    float tEnter = max(max(tmin.x, tmin.y), tmin.z);
    float tExit = min(min(tmax.x, tmax.y), tmax.z);
    float hit = (tExit >= max(tEnter, 0.0)) ? 1.0 : 0.0;

    float t = (tEnter > 0.0) ? tEnter : tExit; // inside the box: use exit face
    tHit = t;
    vec3 q = abs(ro + rd * t - c) / he;        // hit face: largest component ~= 1
    // Median component reaches 1 exactly on the face edges.
    float m = min(max(q.x, q.y), min(max(q.y, q.z), max(q.z, q.x)));
    float w = max(fwidth(m), 1e-4) * 1.5;
    return hit * (1.0 - smoothstep(0.0, w, 1.0 - m));
}

// Brush SDFs - must match SdfBrushCS so the footprint shows the exact stamp.
float sdfBox(vec3 p, vec3 halfExtents)
{
    vec3 q = abs(p) - halfExtents;
    return length(max(q, vec3(0.0))) + min(max(q.x, max(q.y, q.z)), 0.0);
}

float brushSdf(vec3 p, vec3 c, float r, int shape)
{
    vec3 q = p - c;
    if (shape == 1) {
        return length(q) - r;
    } else if (shape == 2) {
        return sdfBox(q, vec3(r, 0.001, r));
    }
    return sdfBox(q, vec3(r));
}

// ------------------------------------------------------------- debug HUD ---
// 7-segment digits, evaluated analytically (no font texture). Quest-only:
// there is no ImGui on device, so fps / brush / atlas stats render here.

float segBox(vec2 p, vec2 c, vec2 he)
{
    vec2 d = abs(p - c) - he;
    return (max(d.x, d.y) <= 0.0) ? 1.0 : 0.0;
}

// p in the digit's local [0,1]^2 (y down).
float sevenSegDigit(vec2 p, int d)
{
    if (p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0) return 0.0;
    // bit 0 top, 1 top-right, 2 bottom-right, 3 bottom, 4 bottom-left,
    // 5 top-left, 6 middle
    const int masks[10] = int[10](0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F);
    int m = masks[clamp(d, 0, 9)];
    const float t = 0.10; // segment half-thickness
    float r = 0.0;
    if ((m & 1) != 0)  r = max(r, segBox(p, vec2(0.50, 0.06), vec2(0.34, t)));
    if ((m & 2) != 0)  r = max(r, segBox(p, vec2(0.86, 0.28), vec2(t, 0.20)));
    if ((m & 4) != 0)  r = max(r, segBox(p, vec2(0.86, 0.72), vec2(t, 0.20)));
    if ((m & 8) != 0)  r = max(r, segBox(p, vec2(0.50, 0.94), vec2(0.34, t)));
    if ((m & 16) != 0) r = max(r, segBox(p, vec2(0.14, 0.72), vec2(t, 0.20)));
    if ((m & 32) != 0) r = max(r, segBox(p, vec2(0.14, 0.28), vec2(t, 0.20)));
    if ((m & 64) != 0) r = max(r, segBox(p, vec2(0.50, 0.50), vec2(0.34, t)));
    return r;
}

// Right-aligned unsigned integer at `origin` (texcoord space, y down).
float hudNumber(vec2 uv, vec2 origin, int value, int digits)
{
    const vec2 cell = vec2(0.026, 0.048);
    vec2 rel = uv - origin;
    if (rel.x < 0.0 || rel.y < 0.0 || rel.y >= cell.y || rel.x >= cell.x * float(digits)) return 0.0;
    int idx = int(rel.x / cell.x);
    int divisor = 1;
    for (int i = 0; i < digits - 1 - idx; i++) divisor *= 10;
    int dig = (max(value, 0) / divisor) % 10;
    // 80% of the cell holds the digit, the rest is spacing.
    vec2 lp = vec2(fract(rel.x / cell.x) / 0.8, rel.y / cell.y);
    return sevenSegDigit(lp, dig);
}

// Stats HUD (device builds; hudA.x = 0 on desktop, which has ImGui).
// Rows top to bottom: fps | used bricks | atlas usage % | brush radius x1000
// | smoothness x10000. Must be called LAST in main(): it blends over
// whatever is already in outColor.
void drawHud()
{
    if (ubo.hudA.x < 0.5) {
        return;
    }
    const vec2 hudOrigin = vec2(0.40, 0.22);
    const float rowStep = 0.058;
    float v = 0.0;
    v = max(v, hudNumber(texcoord, hudOrigin, int(ubo.hudA.y + 0.5), 4));
    v = max(v, hudNumber(texcoord, hudOrigin + vec2(0.0, rowStep), int(ubo.hudA.z + 0.5), 5));
    v = max(v, hudNumber(texcoord, hudOrigin + vec2(0.0, rowStep * 2.0), int(ubo.hudA.w + 0.5), 3));
    v = max(v, hudNumber(texcoord, hudOrigin + vec2(0.0, rowStep * 3.0), int(ubo.hudB.x + 0.5), 3));
    v = max(v, hudNumber(texcoord, hudOrigin + vec2(0.0, rowStep * 4.0), int(ubo.hudB.y + 0.5), 3));
    outColor.rgb = mix(outColor.rgb, vec3(0.0, 0.25, 0.9), v);
}

// Silhouette ring of a sphere.
float sphereOutline(vec3 ro, vec3 rd, vec3 c, float r, out float tHit)
{
    vec3 oc = c - ro;
    float t = max(dot(oc, rd), 0.0);
    tHit = t;
    float d = length(oc - rd * t) - r; // signed distance of the ray to the silhouette
    float w = max(fwidth(d), 1e-5) * 1.5;
    float vis = (dot(oc, rd) > 0.0) ? 1.0 : 0.0;
    return vis * (1.0 - smoothstep(0.0, w, abs(d)));
}

void main()
{
    const int vi = gl_ViewIndex;
    vec4 scene = texture(srcColor, vec3(texcoord, float(vi)));
    outColor = scene;

    if (ubo.brushParams.y < 0.5) {
        drawHud();
        return;
    }

    // Per-pixel view ray; same NDC convention as the CPU brush-picking ray.
    vec2 ndc = vec2(texcoord.x * 2.0 - 1.0, 1.0 - texcoord.y * 2.0);
    vec4 rayView = ubo.invProj[vi] * vec4(ndc, -1.0, 1.0);
    rayView = vec4(rayView.xy, -1.0, 0.0);
    vec3 rd = normalize((ubo.invView[vi] * rayView).xyz);
    vec3 ro = ubo.camPos[vi].xyz;

    vec3 c = ubo.brushCenterRadius.xyz;
    float r = ubo.brushCenterRadius.w;
    int shape = int(floor(ubo.brushParams.x + 0.5));

    float tHit;
    float outline;
    if (shape == 1) {
        outline = sphereOutline(ro, rd, c, r, tHit);
    } else if (shape == 2) {
        outline = boxOutline(ro, rd, c, vec3(r, 0.001, r), tHit);
    } else {
        outline = boxOutline(ro, rd, c, vec3(r), tHit);
    }

    // Scene distance along this pixel's ray, reconstructed from the resolved
    // depth buffer (post FB matches the scene FB size, so texel == fragment).
    float depth = texelFetch(sceneDepth, ivec3(ivec2(gl_FragCoord.xy), vi), 0).r;
    vec4 viewPos = ubo.invProj[vi] * vec4(ndc, depth, 1.0);
    viewPos /= viewPos.w;
    vec3 scenePos = (ubo.invView[vi] * vec4(viewPos.xyz, 1.0)).xyz;
    float sceneT = dot(scenePos - ro, rd);

    // In front of the surface: bright. Behind it: faded ghost.
    float occlusionFade = (tHit > sceneT + 1e-4) ? 0.18 : 1.0;

    // Surface footprint: brush SDF at the visible surface point. Negative =
    // this surface pixel is inside the brush volume (the stamp will hit it).
    float sd = brushSdf(scenePos, c, r, shape);
    float cw = max(fwidth(sd), 1e-5) * 1.5;
    bool hasSurface = depth < 1.0;
    float footprintFill = (hasSurface && sd < 0.0) ? 0.25 : 0.0;
    float footprintContour = hasSurface ? (1.0 - smoothstep(0.0, cw, abs(sd))) : 0.0;

    // Add = brush color, remove = red.
    vec3 lineColor = (ubo.brushParams.z >= 0.5) ? ubo.brushColor.rgb : vec3(1.0, 0.15, 0.15);
    float overlay = max(outline * occlusionFade * 0.9, max(footprintFill, footprintContour * 0.95));
    outColor = vec4(mix(scene.rgb, lineColor, overlay), scene.a);

    // HUD draws last: anything composited from scene.rgb above must not
    // wipe it (that exact bug hid the digits on Quest, where the indicator
    // is always visible).
    drawHud();
}
