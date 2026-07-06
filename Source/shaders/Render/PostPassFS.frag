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
    vec4 brushParams;       // x: shape (low 4 bits) + orientation*16, y: visible, z: action (1 add, 0 remove)
    vec4 brushColor;        // rgb: brush color
    vec4 hudA;              // x: show HUD, y: fps, z: used bricks, w: atlas usage %
    vec4 hudB;              // x: radius*1000, y: smoothness*10000, z: cage, w: deferred
    vec4 chunkMin;          // sculptable bounds, world space
    vec4 chunkMax;
    vec4 depthFovTan[2];    // Quest depth-cam FOV tangents (tanL,tanR,tanD,tanU) per eye
    vec4 depthCtrl;         // x: occlusion on (depth bound), y: fullscreen viz on
    mat4 brushRot;          // world -> shape-local sample rotation (matches SdfBrushTexCS)
} ubo;

layout(binding = 1) uniform sampler2DArray srcColor;
layout(binding = 2) uniform sampler2DArray sceneDepth;
// Deferred-shading G-buffer (desktop): the brick shader's world-space normal in
// xyz, coverage in w (1 surface, 0 cleared background). On Quest this binds the
// color image as a placeholder and is never sampled (hudB.w = 0).
layout(binding = 3) uniform sampler2DArray gNormal;
// Quest environment depth (XR_META_environment_depth), linearized to meters by
// QuestDepthVizCS. Drawn as a debug corner only when brushParams.w is set (Quest);
// a placeholder on desktop, never sampled there.
layout(binding = 4) uniform sampler2DArray depthDisplay;

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

// The rest of the brush shapes (mirror SdfBrushCS::brushShapeSdf so the surface
// footprint matches the actual stamp). sdfBox is defined above.
float sdfSphere(vec3 p, float r) { return length(p) - r; }
float sdfRoundBox(vec3 p, float r) { float e = r * 0.3; return sdfBox(p, vec3(r - e)) - e; }
float sdfPlanePatch(vec3 p, float r) { return sdfBox(p, vec3(r, 0.001, r)); }
float sdfTorus(vec3 p, float r) { vec2 q = vec2(length(p.xz) - r * 0.65, p.y); return length(q) - r * 0.35; }
float sdfCylinder(vec3 p, float r) {
    vec2 d = abs(vec2(length(p.xz), p.y)) - vec2(r * 0.7, r);
    return min(max(d.x, d.y), 0.0) + length(max(d, vec2(0.0)));
}
float sdfCappedCone(vec3 p, float h, float r1, float r2) {
    vec2 q = vec2(length(p.xz), p.y);
    vec2 k1 = vec2(r2, h);
    vec2 k2 = vec2(r2 - r1, 2.0 * h);
    vec2 ca = vec2(q.x - min(q.x, (q.y < 0.0) ? r1 : r2), abs(q.y) - h);
    vec2 cb = q - k1 + k2 * clamp(dot(k1 - q, k2) / dot(k2, k2), 0.0, 1.0);
    float s = (cb.x < 0.0 && ca.y < 0.0) ? -1.0 : 1.0;
    return s * sqrt(min(dot(ca, ca), dot(cb, cb)));
}
float sdfCone(vec3 p, float r) { return sdfCappedCone(p, r, r * 0.8, r * 0.05); }
float sdfCapsule(vec3 p, float r) { vec3 a = p; a.y -= clamp(a.y, -r * 0.55, r * 0.55); return length(a) - r * 0.45; }
float sdfOctahedron(vec3 p, float r) { p = abs(p); return (p.x + p.y + p.z - r) * 0.57735027; }
float sdfHexPrism(vec3 p, float r) {
    const vec3 k = vec3(-0.8660254, 0.5, 0.57735);
    p = abs(p);
    p.xy -= 2.0 * min(dot(k.xy, p.xy), 0.0) * k.xy;
    vec2 h = vec2(r * 0.7, r);
    vec2 d = vec2(length(p.xy - vec2(clamp(p.x, -k.z * h.x, k.z * h.x), h.x)) * sign(p.y - h.x), p.z - h.y);
    return min(max(d.x, d.y), 0.0) + length(max(d, vec2(0.0)));
}
float sdfEllipsoid(vec3 p, float r) {
    vec3 rad = vec3(r * 0.95, r * 0.6, r * 0.95);
    float k0 = length(p / rad); float k1 = length(p / (rad * rad));
    return k0 * (k0 - 1.0) / max(k1, 1e-6);
}
float sdfPyramid(vec3 p, float r) {
    vec3 q = p / r; q.y += 0.5;
    const float h = 1.0; const float m2 = h * h + 0.25;
    q.xz = abs(q.xz); q.xz = (q.z > q.x) ? q.zx : q.xz; q.xz -= 0.5;
    vec3 w = vec3(q.z, h * q.y - 0.5 * q.x, h * q.x + 0.5 * q.y);
    float s = max(-w.x, 0.0);
    float t = clamp((w.y - 0.5 * q.z) / (m2 + 0.25), 0.0, 1.0);
    float a = m2 * (w.x + s) * (w.x + s) + w.y * w.y;
    float b = m2 * (w.x + 0.5 * t) * (w.x + 0.5 * t) + (w.y - m2 * t) * (w.y - m2 * t);
    float d2 = (min(w.y, -w.x * m2 - w.y * 0.5) > 0.0) ? 0.0 : min(a, b);
    return sqrt((d2 + w.z * w.z) / m2) * sign(max(w.z, -q.y)) * r;
}

// Brush orientation: world -> shape-local rotation matrix (the R-rotate
// operator, CPU-composed - matches SdfBrushTexCS exactly so the indicator
// shows exactly what the stamp will do).
vec3 orientBrush(vec3 d)
{
    return mat3(ubo.brushRot) * d;
}

float brushSdf(vec3 p, vec3 c, float r, int mode)
{
    vec3 q = orientBrush(p - c);
    if (mode == 1)  return sdfSphere(q, r);
    if (mode == 2)  return sdfPlanePatch(q, r);
    if (mode == 3)  return sdfRoundBox(q, r);
    if (mode == 4)  return sdfTorus(q, r);
    if (mode == 5)  return sdfCylinder(q, r);
    if (mode == 6)  return sdfCone(q, r);
    if (mode == 7)  return sdfCapsule(q, r);
    if (mode == 8)  return sdfOctahedron(q, r);
    if (mode == 9)  return sdfHexPrism(q, r);
    if (mode == 10) return sdfEllipsoid(q, r);
    if (mode == 11) return sdfPyramid(q, r);
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

// Edge factor of one box face (the median trick: the median of |q| reaches 1
// exactly on the face's edges). Branch-free for fwidth safety.
float cageFace(vec3 ro, vec3 rd, vec3 c, vec3 he, float t, float sceneT)
{
    vec3 q = abs(ro + rd * t - c) / he;
    float m = min(max(q.x, q.y), min(max(q.y, q.z), max(q.z, q.x)));
    float w = max(fwidth(m), 1e-4) * 1.5;
    float edge = 1.0 - smoothstep(0.0, w, 1.0 - m);
    float visible = (t >= 0.0) ? 1.0 : 0.0;
    float fade = (t > sceneT + 1e-4) ? 0.3 : 1.0; // ghost behind the surface
    return edge * visible * fade;
}

// Boundary cage of the sculptable chunk: the edges of both the entry and
// exit faces of the bounds box along this pixel's ray, depth-faded against
// the scene like the brush outline.
float boundsCage(vec3 ro, vec3 rd, vec3 c, vec3 he, float sceneT)
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
    float cage = max(
        cageFace(ro, rd, c, he, tEnter, sceneT),
        cageFace(ro, rd, c, he, tExit, sceneT));
    return cage * hit;
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
    outColor.a = max(outColor.a, v); // visible over passthrough
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

// Generic silhouette outline for ANY brush shape (used for 3..11): march the
// brush SDF through its bounding sphere, track the ray's closest signed approach
// to the surface, and draw the rim where that approach crosses zero - i.e. the
// shape's actual silhouette. Same idea as sphereOutline (ray-to-surface
// distance), generalized to arbitrary SDFs via marching. Cost is bounded to the
// brush's bounding sphere (most pixels miss it and return immediately).
float shapeOutline(vec3 ro, vec3 rd, vec3 c, float r, int shape, out float tHit)
{
    vec3 oc = ro - c;
    float R = 1.85 * r;                       // bounding sphere (cube corner ~1.73r)
    float bb = dot(oc, rd);
    float disc = bb * bb - (dot(oc, oc) - R * R);
    tHit = 1e9;
    if (disc < 0.0) return 0.0;               // ray misses the brush volume
    float sd = sqrt(disc);
    float t = max(-bb - sd, 0.0);
    float tEnd = -bb + sd;
    float minD = R;
    float tMin = t;
    float minStep = R * 0.04;
    for (int i = 0; i < 40; i++) {
        if (t > tEnd) break;
        float d = brushSdf(ro + rd * t, c, r, shape);
        if (d < minD) { minD = d; tMin = t; }
        t += max(abs(d), minStep);            // sphere-trace, floored to bound steps
    }
    tHit = tMin;
    float w = max(fwidth(minD), 1e-5) * 1.5;
    return 1.0 - smoothstep(0.0, w, abs(minD));
}

void main()
{
    const int vi = gl_ViewIndex;
    vec4 scene = texture(srcColor, vec3(texcoord, float(vi)));

    // Deferred shading (ubo.hudB.w; desktop only): the brick shader computed the
    // world normal (cache-coherent) and wrote it + albedo to the G-buffer; do
    // only the lighting here. No atlas, no normal reconstruction.
    if (ubo.hudB.w > 0.5) {
        vec4 gn = texture(gNormal, vec3(texcoord, float(vi)));
        if (gn.w > 0.5) {
            // MSAA averages the normal across samples (and with cleared zeros at
            // edges); normalize() recovers the direction.
            vec3 n = normalize(gn.xyz);

            // View direction = this pixel's camera ray (no depth needed).
            vec2 ndc = vec2(texcoord.x * 2.0 - 1.0, 1.0 - texcoord.y * 2.0);
            vec4 rv = ubo.invProj[vi] * vec4(ndc, -1.0, 1.0);
            rv = vec4(rv.xy, -1.0, 0.0);
            vec3 dir = normalize((ubo.invView[vi] * rv).xyz);

            vec3 albedo = scene.rgb;
            vec3 light = normalize(vec3(10.0, 10.0, 10.0));
            float dif = clamp(dot(light, n), 0.0, 1.0);
            vec3 reflectDir = reflect(light, n);
            float spec = pow(max(dot(dir, reflectDir), 0.0), 32.0);
            float baseLum = dot(albedo, vec3(0.2126, 0.7152, 0.0722));
            float baseChroma = max(albedo.r, max(albedo.g, albedo.b)) - min(albedo.r, min(albedo.g, albedo.b));
            bool matteFloorLike = (baseLum > 0.82 && baseChroma < 0.08);
            vec3 lit;
            if (matteFloorLike) {
                lit = albedo * (0.18 + 0.82 * dif);
            } else {
                lit = albedo * dif * 0.7 + 0.1 + clamp(dot(dir, n) + 1.0, 0.0, 1.0) * 0.5 * vec3(1.0) + spec * 0.5;
            }
            scene.rgb = lit;
        }
    }

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
    // brushParams.x carries the shape (rotation travels as the brushRot matrix).
    int packed = int(floor(ubo.brushParams.x + 0.5));
    int shape = packed & 15;

    float tHit;
    float outline;
    // Box-edge outlines (cube, plane slab) run in SHAPE-LOCAL space: rotate
    // the ray with the same brushRot the SDF uses and keep the box axis-
    // aligned - the wireframe then tracks the R-rotate exactly. Rotations
    // preserve length, so tHit stays a valid world ray distance.
    vec3 roL = mat3(ubo.brushRot) * (ro - c);
    vec3 rdL = mat3(ubo.brushRot) * rd;
    if (shape == 0) {
        outline = boxOutline(roL, rdL, vec3(0.0), vec3(r), tHit);          // cube: edge wireframe
    } else if (shape == 1) {
        outline = sphereOutline(ro, rd, c, r, tHit);                       // sphere: silhouette ring
    } else if (shape == 2) {
        outline = boxOutline(roL, rdL, vec3(0.0), vec3(r, 0.001, r), tHit); // plane: canonical thin-Y slab
    } else {
        outline = shapeOutline(ro, rd, c, r, shape, tHit);                 // 3..11: generic silhouette
    }

    // Scene distance along this pixel's ray, reconstructed from the resolved
    // depth buffer (post FB matches the scene FB size, so texel == fragment).
    float depth = texelFetch(sceneDepth, ivec3(ivec2(gl_FragCoord.xy), vi), 0).r;
    vec4 viewPos = ubo.invProj[vi] * vec4(ndc, depth, 1.0);
    viewPos /= viewPos.w;
    vec3 scenePos = (ubo.invView[vi] * vec4(viewPos.xyz, 1.0)).xyz;
    float sceneT = dot(scenePos - ro, rd);

    // Real-world depth occlusion (Quest): if the environment depth at this pixel is
    // nearer than the SDF surface, the real world is in front - drop the SDF pixel
    // (alpha 0 shows passthrough). -viewPos.z is the SDF's metric eye-space depth;
    // depthDisplay.a is the Quest metric depth, reprojected through the depth FOV.
    // depthCtrl.x gates it (depth bound on Quest); desktop never sets it.
    if (ubo.depthCtrl.x > 0.5 && depth < 1.0) {
        vec4 rvD = ubo.invProj[vi] * vec4(ndc, -1.0, 1.0); // rvD.xy = eye-ray tangents
        vec4 dft = ubo.depthFovTan[vi];
        float du = (rvD.x - dft.x) / (dft.y - dft.x);
        float dv = (rvD.y - dft.z) / (dft.w - dft.z);
        if (du >= 0.0 && du <= 1.0 && dv >= 0.0 && dv <= 1.0) {
            float qd = texture(depthDisplay, vec3(du, dv, float(vi))).a; // meters
            // 3 cm bias so coincident surfaces don't z-fight into passthrough.
            if (qd > 0.05 && qd < -viewPos.z - 0.03) {
                scene.a = 0.0;
            }
        }
    }

    // In front of the surface: bright. Behind it: faded ghost.
    float occlusionFade = (tHit > sceneT + 1e-4) ? 0.18 : 1.0;

    // Surface footprint: brush SDF at the visible surface point. Negative =
    // this surface pixel is inside the brush volume (the stamp will hit it).
    float sd = brushSdf(scenePos, c, r, shape);
    float cw = max(fwidth(sd), 1e-5) * 1.5;
    bool hasSurface = depth < 1.0;
    float footprintFill = (hasSurface && sd < 0.0) ? 0.25 : 0.0;
    float footprintContour = hasSurface ? (1.0 - smoothstep(0.0, cw, abs(sd))) : 0.0;

    // Add = brush color, remove = red. Alpha rises with the overlay so the
    // indicator stays visible over the passthrough background (alpha 0
    // pixels show the real world on Quest).
    vec3 lineColor = (ubo.brushParams.z >= 0.5) ? ubo.brushColor.rgb : vec3(1.0, 0.15, 0.15);
    float overlay = max(outline * occlusionFade * 0.9, max(footprintFill, footprintContour * 0.95));
    outColor = vec4(mix(scene.rgb, lineColor, overlay), max(scene.a, overlay));

    // Sculptable-bounds cage: subtle blue box edges marking where stamps can
    // land. Drawn after the composite above (which rebuilds from scene.rgb).
    if (ubo.hudB.z > 0.5) {
        vec3 boundsCenter = 0.5 * (ubo.chunkMin.xyz + ubo.chunkMax.xyz);
        vec3 boundsHalf = 0.5 * (ubo.chunkMax.xyz - ubo.chunkMin.xyz);
        float cage = boundsCage(ro, rd, boundsCenter, boundsHalf, sceneT);
        outColor.rgb = mix(outColor.rgb, vec3(0.2, 0.45, 0.9), cage * 0.55);
        outColor.a = max(outColor.a, cage * 0.55);
    }

    // HUD draws last: anything composited from scene.rgb above must not
    // wipe it (that exact bug hid the digits on Quest, where the indicator
    // is always visible).
    drawHud();

    // DEBUG (Quest): the real-world environment depth viz (grayscale depth, world
    // position, or normal - chosen in QuestDepthVizCS), FULLSCREEN and reprojected
    // to line up with the camera view. The depth map was captured with the depth
    // camera's own FOV, so for each pixel we take its eye-ray tangents (rvD.xy in
    // GL view space, -z forward) and remap them through the depth FOV to find the
    // matching texel. The compute already mapped the value to a color, so just
    // display it. Outside the depth FOV the scene shows through.
    // depthCtrl.y: fullscreen debug viz on/off (Y cycles it). Desktop never sets it.
    if (ubo.depthCtrl.y > 0.5) {
        vec2 ndcD = vec2(texcoord.x * 2.0 - 1.0, 1.0 - texcoord.y * 2.0);
        vec4 rvD = ubo.invProj[vi] * vec4(ndcD, -1.0, 1.0); // rvD.xy = eye-ray x/y tangents
        vec4 dft = ubo.depthFovTan[vi];                     // tanL, tanR, tanD, tanU
        float du = (rvD.x - dft.x) / (dft.y - dft.x);
        float dv = (rvD.y - dft.z) / (dft.w - dft.z);
        if (du >= 0.0 && du <= 1.0 && dv >= 0.0 && dv <= 1.0) {
            vec3 col = texture(depthDisplay, vec3(du, dv, float(vi))).rgb;
            outColor = vec4(col, 1.0);
        }
    }
}
