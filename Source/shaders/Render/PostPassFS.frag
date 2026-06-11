#version 450

// Post pass: scene passthrough + brush indicator overlay.
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
    mat4 invProj;
    mat4 invView;
    vec4 camPos;            // xyz: camera position WS
    vec4 brushCenterRadius; // xyz: brush center WS, w: effective radius
    vec4 brushParams;       // x: shape (0 cube, 1 sphere, 2 plane), y: visible, z: action (1 add, 0 remove)
    vec4 brushColor;        // rgb: brush color
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
    vec4 scene = texture(srcColor, vec3(texcoord, 0.0));
    outColor = scene;

    if (ubo.brushParams.y < 0.5) {
        return;
    }

    // Per-pixel view ray; same NDC convention as the CPU brush-picking ray.
    vec2 ndc = vec2(texcoord.x * 2.0 - 1.0, 1.0 - texcoord.y * 2.0);
    vec4 rayView = ubo.invProj * vec4(ndc, -1.0, 1.0);
    rayView = vec4(rayView.xy, -1.0, 0.0);
    vec3 rd = normalize((ubo.invView * rayView).xyz);
    vec3 ro = ubo.camPos.xyz;

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
    float depth = texelFetch(sceneDepth, ivec3(ivec2(gl_FragCoord.xy), 0), 0).r;
    vec4 viewPos = ubo.invProj * vec4(ndc, depth, 1.0);
    viewPos /= viewPos.w;
    vec3 scenePos = (ubo.invView * vec4(viewPos.xyz, 1.0)).xyz;
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
}
