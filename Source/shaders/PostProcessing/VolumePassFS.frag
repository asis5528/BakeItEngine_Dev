#version 450

layout(location = 0) in vec2 texcoord;
layout(location = 0) out vec4 outColor;
layout(std140, binding = 0) uniform CameraUBO {
  mat4 invView;
  mat4 invProj;
  mat4 currViewProj;
  mat4 prevViewProj;
  vec3 camPos;
  vec2 resolution;
  float nearPlane;
  float farPlane;
  vec4 brushData;
  vec4 brushMeta; // x: action, y: shape mode (0 cube, 1 sphere, 2 plane)
  vec4 taaData;   // x,y current jitter, z,w previous jitter
  vec4 taaState;  // x: history valid (0/1)
};
layout(binding = 1) uniform sampler2DArray texSampler;
layout(binding = 2) uniform sampler2DArray depthSampler;
layout(binding = 3) uniform sampler2DArray historySampler;
layout(binding = 4) readonly buffer BrickBuffer {
  float bricks[];  // single int
};
layout(std430, binding = 5) readonly buffer BlockBuffer {
  int blocks[];  // single int
};

const int BLOCK_EMISSIVE_FLAG = (1 << 30);
const int BLOCK_INDEX_MASK = BLOCK_EMISSIVE_FLAG - 1;
void getRay(out vec3 rayOrigin, out vec3 rayDir) {
  // NDC in [-1, 1]
  vec2 ndc = (gl_FragCoord.xy / resolution.xy) * 2.0 - 1.0;
  ndc.y = -ndc.y;  // keep if your projection expects this

  // Point on near plane in clip space
  // For Vulkan, clip Z is 0 at near. For OpenGL, it's -1.
  vec4 rayClip = vec4(ndc, 0.0, 1.0);

  // To view space
  vec4 viewPos = invProj * rayClip;
  viewPos /= viewPos.w;

  // To world space
  vec4 worldPos = invView * viewPos;

  // Ray origin & direction in world space
  rayOrigin = camPos;  // world-space camera pos
  rayDir = normalize(worldPos.xyz - camPos);
}
vec2 rayBoxDst(vec3 boundsMi, vec3 boundsMa, vec3 rayOrigin, vec3 invRaydir) {
  // Adapted from: http://jcgt.org/published/0007/03/04/
  vec3 t0 = (boundsMi - rayOrigin) * invRaydir;
  vec3 t1 = (boundsMa - rayOrigin) * invRaydir;
  vec3 tmin = min(t0, t1);
  vec3 tmax = max(t0, t1);

  float dstA = max(max(tmin.x, tmin.y), tmin.z);
  float dstB = min(tmax.x, min(tmax.y, tmax.z));

  // CASE 1: ray intersects box from outside (0 <= dstA <= dstB)
  // dstA is dst to nearest intersection, dstB dst to far intersection

  // CASE 2: ray intersects box from inside (dstA < 0 < dstB)
  // dstA is the dst to intersection behind the ray, dstB is dst to forward
  // intersection

  // CASE 3: ray misses box (dstA > dstB)

  float dstToBox = max(0, dstA);
  float dstInsideBox = max(0, dstB - dstToBox);
  return vec2(dstToBox, dstInsideBox);
}

float brushSdfBox(vec3 p, vec3 halfExtents) {
    vec3 q = abs(p) - halfExtents;
    return length(max(q, vec3(0.0))) + min(max(q.x, max(q.y, q.z)), 0.0);
}

float brushSdfSphere(vec3 p, float radius) {
    return length(p) - radius;
}

float brushSdfPlanePatch(vec3 p, float radius, float halfThickness) {
    return brushSdfBox(p, vec3(radius, 0.001, radius));
}

float brushSdf(vec3 p) {
    float r = max(brushData.w, 0.0005);
    if (brushMeta.x < 0.5) {
        r *= 1.15; // keep remove preview consistent with remove behavior
    }

    vec3 pq = p + brushData.xyz;
    int shapeMode = int(floor(brushMeta.y + 0.5));
    if (shapeMode == 1) {
        return brushSdfSphere(pq, r);
    }
    if (shapeMode == 2) {
        return brushSdfPlanePatch(pq, r, max(r * 0.08, 0.0005));
    }
    return brushSdfBox(pq, vec3(r));
}

float raymarchBrushCursor(vec3 ro, vec3 rd, float tMin, float tMax) {
    float t = tMin;
    for (int i = 0; i < 96 && t < tMax; ++i) {
        vec3 p = ro + rd * t;
        float d = brushSdf(p);

        if (abs(d) < 0.0015) {
            return t;
        }

        t += clamp(abs(d), 0.001, 0.03);
    }
    return -1.0;
}

vec3 brushNormal(vec3 p) {
    float e = 0.0015;
    vec3 ex = vec3(e, 0.0, 0.0);
    vec3 ey = vec3(0.0, e, 0.0);
    vec3 ez = vec3(0.0, 0.0, e);
    return normalize(vec3(
        brushSdf(p + ex) - brushSdf(p - ex),
        brushSdf(p + ey) - brushSdf(p - ey),
        brushSdf(p + ez) - brushSdf(p - ez)
    ));
}

float linearizeDepth(float depth, float nearPlane, float farPlane) {
float z = depth * 2.0 - 1.0; // back to NDC 
return (2.0 * nearPlane * farPlane) / (farPlane + nearPlane - z * (farPlane - nearPlane));
}
float linearDepthFromBuffer(float depth)
{
    vec2 fragCoord = gl_FragCoord.xy; // use actual pixel
    vec2 ndc = (fragCoord / resolution.xy) * 2.0 - 1.0;
    ndc.y = -ndc.y;

    vec4 clip = vec4(ndc, depth, 1.0); // Vulkan
    vec4 view = invProj * clip;
    view /= view.w;

    // choose one convention; I'll use Euclidean distance
    return length(view.xyz);
}

float fetchBricks(int brickIndex, ivec3 p) {
  p = clamp(p, ivec3(0), ivec3(8));

  int localIndex = p.x + p.y * 9 + p.z * 9 * 9;

  return bricks[brickIndex + localIndex];
}
vec2 map(vec3 p) {
 // float k = 1. / 64.;
  vec3 cpos = p + vec3(0.5);
  //cpos *=0.5;
  vec3 ppos = cpos;
  vec3 texRes = vec3(textureSize(texSampler, 0));
 // vec4 h1 = texture(texSampler, ppos);
 //float h = h1.r;
float h = 0.;
float mat = 0.;
int blockPerAxis = 64;
ivec3 ires = ivec3(blockPerAxis,blockPerAxis,blockPerAxis);
 vec3  fres = vec3(ires);

// texel space
vec3 st = clamp(cpos * fres-0.5 ,0.,fres.x-1);
    ivec3 i = ivec3(floor(st));
    
   // bricks[i.x+i.y*8+i.z*64] = h1.r;
  // blocks[i.x+i.y*8+i.z*64] = int(h1.r*255.);

//h = textureTrilinear( texSampler, cpos).r;
int blockRaw = blocks[i.x+i.y*blockPerAxis+i.z*blockPerAxis*blockPerAxis];
int brickIndex = (blockRaw & BLOCK_INDEX_MASK);
//h = 0.;
if(blockRaw>=0){
//mat = .0;
vec3 newcord = cpos * fres;

ivec3 localGrid = ivec3(floor(fract(newcord-0.5)*8));
vec3 localfract = vec3(fract(fract(newcord)*8));

vec3 fw = fract((fract(st))*9*(8./9.));
ivec3  w = ivec3(floor(fract(st)*8.));

fw = localfract;
w = localGrid;
//w = w*int(floor(7./8.));
    int localIndex = w.x + w.y*9 + w.z*9*9;
  //  w = clamp(w, ivec3(0), 8 - ivec3(2));

    float a = fetchBricks(brickIndex,w + ivec3(0,0,0));
    float b = fetchBricks(brickIndex,w + ivec3(1,0,0));
    float c = fetchBricks(brickIndex,w + ivec3(0,1,0));
    float d = fetchBricks(brickIndex,w + ivec3(1,1,0));

    float e = fetchBricks(brickIndex,w + ivec3(0,0,1));
    float f = fetchBricks(brickIndex,w + ivec3(1,0,1));
    float g = fetchBricks(brickIndex,w + ivec3(0,1,1));
    float k = fetchBricks(brickIndex,w + ivec3(1,1,1));
   //w = w*w*(3.0-2.0*w);
  // w = 2.*w;
  // w = pow(w,vec3(2.));
   
    // interpolate
    float slice0 = mix(mix(a, b, fw.x), mix(c, d, fw.x), fw.y);
    float slice1 = mix(mix(e, f, fw.x), mix(g, k, fw.x), fw.y);
    float hn =  mix(slice0, slice1, fw.z);

    h = hn/(64.*8.);
    //h=fetchBricks(brickIndex,w);
}
else if(blockRaw!=-999999){
    h = float(blockRaw)/255.;
}

else{
    h = 0.01;
}


vec3 q = p;






//vec3 pq = p;
//pq+=h1.gba;
//float eraser  = length(pq)-(0.02);

float d =  h;


return vec2(d,d);
}
float rayMarchSdf(vec3 ro,vec3 rd,float maxDist){
  float t = 0.0;
  bool hitting = false;
    for (int i = 0; i < 556; i++) {
          vec3 p = ro + rd * t;
          // sm+=0.01;
          vec2 h = map(p);
          if (h.x < 0.00000001) {
            // hit = 1.;
            hitting = true;
            // mat = h.y;
            break;
          }
          t += h.x;
          if(t >maxDist)
          {
          t = -1.0; 
          break;
          }
        }
        return t;
    
}
vec3 calcNormal(vec3 pos) {
    vec3 texRes = vec3(textureSize(texSampler, 0));
    float e = 1.0 / (64.*8.); // one voxel step
    vec3 eps = vec3(e, 0.0, 0.0);

    return normalize(vec3(
        map(pos + eps.xyy).x - map(pos - eps.xyy).x,
        map(pos + eps.yxy).x - map(pos - eps.yxy).x,
        map(pos + eps.yyx).x - map(pos - eps.yyx).x
    ));
}
vec3 calculateSdfLighting(vec3 ro,vec3 rd, float t){
    vec3 n=normalize(calcNormal( ro+rd*t));
      //vec3 n = norVoxel;

      vec3 light = vec3(10.,-10.,10.);

      float dif = clamp(dot(normalize(light),n),0.,1.);

      vec3 reflectDir = reflect(normalize(light), n);
      float spec = pow(max(dot(rd, reflectDir), 0.0), 32.);
      //dif = 1.0;
      //spec = 1.0;
      vec3 outColor = vec3(0.5,0.5,0.2)*dif*0.7+0.1+clamp(dot(rd,n)+1.,0.,1.)*0.5*vec3(1.,1.,1.)+spec*0.5;
 outColor = n;
 vec3 worldPosition = ro+rd*t;
   vec3 localPosition = mod(((worldPosition+0.5)*64),1.0);
   float c = smoothstep(0.5,0.,length(localPosition.xz-vec2(0.5)))+smoothstep(0.5,0.,length(localPosition.yz-vec2(0.5)));
 //  outColor.rgb = vec3(c);
   
   return outColor.rgb;
}


vec3 ReconstructWorldPos(vec2 uv, float depth01)
{
    // Vulkan NDC: x,y in [-1,1], z in [0,1]
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth01, 1.0);
    //  mat4 invView;
  //mat4 invProj;
    vec4 world = invProj * ndc;
    return world.xyz / world.w;
}


vec3 ReconstructViewPos(vec2 uv, float depth01, mat4 invProj)
{
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth01, 1.0);
    vec4 view = invProj * ndc;
    return view.xyz / view.w;
}

vec3 ReconstructViewPos(vec2 uv, float depth01)
{
    // Vulkan NDC: z in [0,1]
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth01, 1.0);
    vec4 v = invProj * ndc;
    return v.xyz / v.w;
}
vec3 ReconstructWorldPoss(vec2 uv, float depth01)
{
    vec3 viewPos = ReconstructViewPos(uv, depth01, invProj);
    vec4 worldH = invView * vec4(viewPos, 1.0);
    return worldH.xyz;
}


vec4 textureGood( sampler2DArray sam, vec3 uv )
{
    ivec2 ires = textureSize( sam, 0 ).xy;
    vec2  fres = vec2( ires );

    vec2 st = uv.xy*fres - 0.5;
    vec2 i = floor( st );
    vec2 w = fract( st );

    vec4 a = texture( sam, vec3((i+vec2(0.5,0.5))/fres,0.) );
    vec4 b = texture( sam, vec3((i+vec2(1.5,0.5))/fres,0.) );
    vec4 c = texture( sam, vec3((i+vec2(0.5,1.5))/fres,0.) );
    vec4 d = texture( sam, vec3((i+vec2(1.5,1.5))/fres,0.) );

    return mix(mix(a, b, w.x), mix(c, d, w.x), w.y);
}


vec3 NormalFromDepth(vec2 uv)
{
    float d = textureGood(depthSampler, vec3(uv,0.)).r;

    // If depth is invalid (skybox/cleared), you may want to early out
    // (choose a threshold based on your clear value)
    // if (d >= 0.999999) return vec3(0,0,1);

    vec3 P  = ReconstructWorldPoss(uv, d);
    vec3 dPdx = dFdx(P);
    vec3 dPdy = dFdy(P);
   
    vec3 N = normalize(cross(dPdx, dPdy));

    // Optional: make it face the camera (avoids flipped normals on some triangles)
    // vec3 V = normalize(pc.camPosWS - P);
    // if (dot(N, V) < 0.0) N = -N;

    return N;
}
float PickClosest(float center, float a, float b)
{
    return (abs(a - center) < abs(b - center)) ? a : b;
}


float DepthFetch(sampler2D depthTex, ivec2 p, int lod)
{
    return texelFetch(depthTex, p, lod).r;
}
float DepthFetchBilinear( vec2 uv, int lod)
{
    ivec3 size = textureSize(depthSampler, lod);
    vec2 p = uv * vec2(size) - 0.5;

    ivec2 i0 = ivec2(floor(p));
    vec2  f  = fract(p);

    ivec2 i1 = i0 + ivec2(1, 0);
    ivec2 i2 = i0 + ivec2(0, 1);
    ivec2 i3 = i0 + ivec2(1, 1);

    ivec2 lo = ivec2(0);
    ivec2 hi = ivec2(size) - ivec2(1);
    i0 = clamp(i0, lo, hi);
    i1 = clamp(i1, lo, hi);
    i2 = clamp(i2, lo, hi);
    i3 = clamp(i3, lo, hi);

    float d00 = texelFetch(depthSampler,ivec3( i0,0), lod).r;
    float d10 = texelFetch(depthSampler, ivec3(i1,0), lod).r;
    float d01 = texelFetch(depthSampler,ivec3( i2,0), lod).r;
    float d11 = texelFetch(depthSampler,ivec3( i3,0), lod).r;

    float dx0 = mix(d00, d10, f.x);
    float dx1 = mix(d01, d11, f.x);
    return mix(dx0, dx1, f.y);
}
vec3 NormalFromDepth_Forward(
    sampler2D depthTex,
    mat4 invViewProj,
    ivec2 pixel,
    int lod)
{
    ivec2 size = textureSize(depthTex, lod);
    ivec2 px = ivec2(min(pixel.x + 1, size.x - 1), pixel.y);
    ivec2 py = ivec2(pixel.x, min(pixel.y + 1, size.y - 1));

    float d  = texelFetch(depthSampler, ivec3(pixel,0), lod).r;
    float dx = texelFetch(depthSampler, ivec3(px,0),    lod).r;
    float dy = texelFetch(depthSampler, ivec3(py,0),    lod).r;

    vec2 uv  = (vec2(pixel) + 0.5) / vec2(size);
    vec2 uvx = (vec2(px)    + 0.5) / vec2(size);
    vec2 uvy = (vec2(py)    + 0.5) / vec2(size);

    vec3 P  = ReconstructViewPos( uv,  d);
    vec3 Px = ReconstructViewPos( uvx, dx);
    vec3 Py = ReconstructViewPos( uvy, dy);

    vec3 N = normalize(cross(Px - P, Py - P));
    return N;
}

vec3 NormalFromDepth_BilinearOffsets(
  
   
    vec2 uv,
    int lod)
{
    ivec3 size = textureSize(depthSampler, 0);
    vec2 texel = 1.0 / vec2(resolution);

    // Bilinear depth at small offsets
    float dC = DepthFetchBilinear( uv, lod);
    float dR = DepthFetchBilinear( uv + vec2(texel.x, 0.0), lod);
    float dL = DepthFetchBilinear( uv - vec2(texel.x, 0.0), lod);
    float dU = DepthFetchBilinear( uv + vec2(0.0, texel.y), lod);
    float dD = DepthFetchBilinear( uv - vec2(0.0, texel.y), lod);

    vec3 PC = ReconstructWorldPoss( uv, dC);
    vec3 PR = ReconstructWorldPoss( uv + vec2(texel.x, 0.0), dR);
    vec3 PL = ReconstructWorldPoss( uv - vec2(texel.x, 0.0), dL);
    vec3 PU = ReconstructWorldPoss(uv + vec2(0.0, texel.y), dU);
    vec3 PD = ReconstructWorldPoss( uv - vec2(0.0, texel.y), dD);

    vec3 dPdx = PR - PL;
    vec3 dPdy = PU - PD;

    return normalize(cross(dPdx, dPdy));
}
vec3 NormalFromDepth2(vec2 uv)
{
    vec2 texel = 1.0 / vec2(textureSize(depthSampler, 0).xy);
    //texel*=0.5;
    // ---- sample depth (slice 0) ----
    float dC = texture(depthSampler, vec3(uv, 0.0)).r;
    float dR = texture(depthSampler, vec3(uv + vec2(texel.x, 0), 0.0)).r;
    float dL = texture(depthSampler, vec3(uv - vec2(texel.x, 0), 0.0)).r;
    float dU = texture(depthSampler, vec3(uv + vec2(0, texel.y), 0.0)).r;
    float dD = texture(depthSampler, vec3(uv - vec2(0, texel.y), 0.0)).r;

    // Optional early out (sky / cleared depth)
    // if (dC >= 0.999999) return vec3(0, 0, 1);

    // ---- edge-aware depth selection ----
    float dX = PickClosest(dC, dR, dL);
    float dY = PickClosest(dC, dU, dD);

    vec2 uvX = (dX == dR) ? uv + vec2(texel.x, 0) : uv - vec2(texel.x, 0);
    vec2 uvY = (dY == dU) ? uv + vec2(0, texel.y) : uv - vec2(0, texel.y);

    // ---- reconstruct positions ----
    vec3 Pc = ReconstructViewPos(uv,  dC);
    vec3 Px = ReconstructViewPos(uvX, dX);
    vec3 Py = ReconstructViewPos(uvY, dY);

    vec3 dx = Px - Pc;
    vec3 dy = Py - Pc;

    // ---- normal ----
    vec3 N = normalize(cross(dx, dy));

    // Flip if needed (depends on handedness / projection)
    // N = -N;

    return N;
}

vec3 NormalVSFromDepth(vec2 uv, mat4 invProj)
{
    float d = texture(depthSampler, vec3(uv,0.)).r;
    vec3 P  = ReconstructViewPos(uv, d, invProj);
    return normalize(cross(dFdx(P), dFdy(P)));
}
vec3 ReconstructWorldPos(float dp,vec2 uv)
{
    float depth01 = dp;

    // NDC: x,y in [-1,1], z in [0,1] for Vulkan/D3D
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth01, 1.0);

    // To view space
    vec4 view = invProj * ndc;
    view.xyz /= view.w;

    // To world space
    vec4 world = invView * vec4(view.xyz, 1.0);
    return world.xyz;
}

bool ProjectWorldToUV(vec3 worldPos, mat4 viewMat, mat4 projMat, out vec2 uv, out float ndcZ)
{
    vec4 clip = projMat * viewMat * vec4(worldPos, 1.0);
    if (clip.w <= 0.000001) {
        uv = vec2(-1.0);
        ndcZ = 2.0;
        return false;
    }

    vec3 ndc = clip.xyz / clip.w;
    uv = ndc.xy * 0.5 + 0.5;
    ndcZ = ndc.z;
    return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
}

vec3 ComputeSSR(vec2 uv, vec3 worldPos, vec3 normalW, vec3 baseColor, out float ssrWeight)
{
    ssrWeight = 0.0;

    mat4 viewMat = inverse(invView);
    mat4 projMat = inverse(invProj);

    vec3 V = normalize(worldPos - camPos);   // camera -> surface
    vec3 R = normalize(reflect(V, normalize(normalW)));

    // Avoid grazing-away/backward rays that rarely give stable hits in SSR
    if (dot(R, normalize(normalW)) <= 0.01) {
        return baseColor;
    }

    const int maxSteps = 56;
    const float startStep = 0.02;
    const float stepGrow = 1.08;
    const float depthThickness = 0.02;

    float t = startStep;
    vec2 hitUV = vec2(-1.0);

    for (int i = 0; i < maxSteps; ++i) {
        vec3 rayPos = worldPos + R * t;

        vec2 suv;
        float ndcZ;
        if (!ProjectWorldToUV(rayPos, viewMat, projMat, suv, ndcZ)) {
            break;
        }

        float sceneD = texture(depthSampler, vec3(suv, 0.0)).r;
        if (sceneD >= 0.99999) {
            t *= stepGrow;
            continue;
        }

        vec3 scenePos = ReconstructWorldPoss(suv, sceneD);

        // Compare camera-space travel distance to detect crossing against scene depth.
        float rayDepth = length(rayPos - camPos);
        float sceneDepth = length(scenePos - camPos);
        float delta = rayDepth - sceneDepth;

        if (delta > -depthThickness && delta < depthThickness * 4.0) {
            hitUV = suv;
            break;
        }

        t *= stepGrow;
    }

    if (hitUV.x < 0.0) {
        return baseColor;
    }

    vec3 reflectedColor = texture(texSampler, vec3(hitUV, 0.0)).rgb;
    float fresnel = pow(1.0 - clamp(dot(normalize(normalW), -V), 0.0, 1.0), 5.0);
    float edgeFade = 1.0 - smoothstep(0.75, 1.0, max(abs(hitUV.x * 2.0 - 1.0), abs(hitUV.y * 2.0 - 1.0)));

    ssrWeight = clamp((0.15 + 0.85 * fresnel) * edgeFade, 0.0, 0.65);
    return mix(baseColor, reflectedColor, ssrWeight);
}

float hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

mat3 makeTBN(vec3 n)
{
    vec3 up = (abs(n.y) < 0.999) ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 t = normalize(cross(up, n));
    vec3 b = cross(n, t);
    return mat3(t, b, n);
}

vec3 sampleHemisphere(float u1, float u2)
{
    float r = sqrt(max(u1, 0.000001));
    float phi = 6.2831853 * u2;
    float x = r * cos(phi);
    float y = r * sin(phi);
    float z = sqrt(max(0.0, 1.0 - u1));
    return vec3(x, y, z);
}

float Luminance(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

vec3 ExtractEmissive(vec3 sceneColor)
{
    // Treat only values above this threshold as emissive energy.
    // Non-emissive shaded surfaces stay black for GI.
    const float emissiveThreshold = 1.05;
    const float emissiveBoost = 2.0;
    float lum = Luminance(sceneColor);
    float w = smoothstep(emissiveThreshold, emissiveThreshold + 0.35, lum);
    vec3 energy = max(sceneColor - vec3(emissiveThreshold), vec3(0.0));
    return energy * w * emissiveBoost;
}

vec3 ComputeSSGI(vec2 uv, vec3 worldPos, vec3 normalW)
{
    mat4 viewMat = inverse(invView);
    mat4 projMat = inverse(invProj);

    const int sampleCount = 8;
    const float baseRadius = 0.10;
    const float thickness = 0.03;

    mat3 tbn = makeTBN(normalize(normalW));
    vec3 giAccum = vec3(0.0);
    float wAccum = 0.0;

    for (int i = 0; i < sampleCount; ++i) {
        float fi = float(i);
        float u1 = hash12(uv * resolution.xy + vec2(fi * 17.0, fi * 3.0));
        float u2 = hash12(uv.yx * resolution.xy + vec2(fi * 11.0, fi * 5.0));

        vec3 dirLocal = sampleHemisphere(u1, u2);
        vec3 dir = normalize(tbn * dirLocal);

        float t = baseRadius * mix(0.25, 1.0, (fi + 0.5) / float(sampleCount));
        vec3 samplePos = worldPos + dir * t;

        vec2 suv;
        float ndcZ;
        if (!ProjectWorldToUV(samplePos, viewMat, projMat, suv, ndcZ)) {
            continue;
        }

        float sceneD = texture(depthSampler, vec3(suv, 0.0)).r;
        if (sceneD >= 0.99999) {
            continue;
        }

        vec3 scenePos = ReconstructWorldPoss(suv, sceneD);
        float proximity = length(scenePos - samplePos);
        if (proximity > thickness) {
            continue;
        }

        vec3 incoming = normalize(scenePos - worldPos);
        float nDotL = max(dot(normalize(normalW), incoming), 0.0);
        float atten = 1.0 - clamp(proximity / thickness, 0.0, 1.0);

        vec3 bounce = ExtractEmissive(texture(texSampler, vec3(suv, 0.0)).rgb);
        if (Luminance(bounce) <= 1e-5) {
            continue;
        }
        float w = nDotL * atten;
        giAccum += bounce * w;
        wAccum += w;
    }

    if (wAccum <= 1e-5) {
        return vec3(0.0);
    }

    return giAccum / wAccum;
}

vec3 DenoiseSSGI(vec2 uv, vec3 centerWorldPos, vec3 centerNormal, vec3 centerGI)
{
    vec2 texel = 1.0 / resolution.xy;

    vec3 accum = centerGI;
    float wsum = 1.0;

    const vec2 taps[8] = vec2[](
        vec2( 1, 0), vec2(-1, 0), vec2( 0, 1), vec2( 0,-1),
        vec2( 1, 1), vec2(-1, 1), vec2( 1,-1), vec2(-1,-1)
    );

    for (int i = 0; i < 8; ++i) {
        vec2 suv = uv + taps[i] * texel;
        if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0)))) {
            continue;
        }

        float d = texture(depthSampler, vec3(suv, 0.0)).r;
        if (d >= 0.99999) {
            continue;
        }

        vec3 p = ReconstructWorldPoss(suv, d);
        vec3 n = NormalFromDepth(suv);
        vec3 giN = ComputeSSGI(suv, p, n);

        float wSpatial = (i < 4) ? 0.8 : 0.55;
        float wDepth = exp(-length(p - centerWorldPos) * 8.0);
        float wNormal = pow(max(dot(normalize(centerNormal), normalize(n)), 0.0), 8.0);
        float w = wSpatial * wDepth * wNormal;

        accum += giN * w;
        wsum += w;
    }

    return accum / max(wsum, 1e-5);
}

vec3 NeighborhoodClamp(vec3 history, vec2 uv)
{
    vec2 texel = 1.0 / resolution.xy;
    vec3 c = texture(texSampler, vec3(uv, 0.0)).rgb;
    vec3 c1 = texture(texSampler, vec3(uv + vec2(texel.x, 0.0), 0.0)).rgb;
    vec3 c2 = texture(texSampler, vec3(uv - vec2(texel.x, 0.0), 0.0)).rgb;
    vec3 c3 = texture(texSampler, vec3(uv + vec2(0.0, texel.y), 0.0)).rgb;
    vec3 c4 = texture(texSampler, vec3(uv - vec2(0.0, texel.y), 0.0)).rgb;
    vec3 mn = min(c, min(min(c1, c2), min(c3, c4)));
    vec3 mx = max(c, max(max(c1, c2), max(c3, c4)));
    return clamp(history, mn, mx);
}

vec3 ApplyTAA(vec2 uv, float depth, vec3 currentColor)
{
    if (taaState.x < 0.5) {
        return currentColor;
    }

    if (depth >= 0.99999) {
        return currentColor;
    }

    vec3 worldPos = ReconstructWorldPoss(uv, depth);
    vec4 prevClip = prevViewProj * vec4(worldPos, 1.0);
    if (prevClip.w <= 0.000001) {
        return currentColor;
    }

    vec2 prevUV = (prevClip.xy / prevClip.w) * 0.5 + 0.5;
    // Compensate jitter mismatch between frames.
    prevUV += (taaData.xy - taaData.zw) * 0.5;

    if (any(lessThan(prevUV, vec2(0.0))) || any(greaterThan(prevUV, vec2(1.0)))) {
        return currentColor;
    }

    vec3 historyColor = texture(historySampler, vec3(prevUV, 0.0)).rgb;
    historyColor = NeighborhoodClamp(historyColor, uv);

    // Lower history weight when depth discontinuity is high.
    float prevDepth = texture(depthSampler, vec3(prevUV, 0.0)).r;
    float dz = abs(prevDepth - depth);
    float historyWeight = mix(0.90, 0.65, smoothstep(0.0005, 0.02, dz));

    return mix(currentColor, historyColor, historyWeight);
}
void main() {
  vec2 uv = texcoord;
  vec3 ro, rd;
  getRay(ro, rd);

  vec2 rayBox = rayBoxDst(vec3(-0.5,-0.5,-0.5), vec3(0.5), ro, 1.0 / rd);

  float sceneDepth = texture(depthSampler, vec3(uv, 0.)).r;
  vec3 n = NormalFromDepth(uv);
  float linearDepth = linearDepthFromBuffer(sceneDepth);
  outColor = texture(texSampler, vec3(uv, 0.));

  // Screen-space reflections from scene color + depth.
  if (sceneDepth < 0.99999) {
      vec3 worldPos = ReconstructWorldPoss(uv, sceneDepth);
      float ssrWeight = 0.0;
     // outColor.rgb = ComputeSSR(uv, worldPos, n, outColor.rgb, ssrWeight);

      // Screen-space global illumination (diffuse bounce approximation).
      vec3 giRaw = ComputeSSGI(uv, worldPos, n);
      vec3 gi = DenoiseSSGI(uv, worldPos, n, giRaw);
      outColor.rgb += gi * 0.22;
  }

  // Temporal AA with previous-frame reprojection.
  outColor.rgb = ApplyTAA(uv, sceneDepth, outColor.rgb);

  // Always-visible 3D brush cursor in post-process, centered around actual brush position.
  if (rayBox.y > 0.0) {
      vec3 brushCenter = -brushData.xyz;
      float brushRadius = max(brushData.w, 0.0005);
      if (brushMeta.x < 0.5) {
          brushRadius *= 1.15; // Match remove-mode effective radius.
      }

      float tMin = rayBox.x;
      float tMax = min(rayBox.x + rayBox.y, linearDepth);
      float tCenter = dot(brushCenter - ro, rd);
      float tWindow = brushRadius * 2.5 + 0.03;
      tMin = max(tMin, tCenter - tWindow);
      tMax = min(tMax, tCenter + tWindow);

      if (tMax > tMin) {
          float tBrush = raymarchBrushCursor(ro, rd, tMin, tMax);
          if (tBrush > 0.0) {
              vec3 pHit = ro + rd * tBrush;
              vec3 nBrush = brushNormal(pHit);
              vec3 lightDir = normalize(vec3(0.4, 0.8, 0.2));
              float ndl = max(dot(nBrush, lightDir), 0.0);
              int shapeMode = int(floor(brushMeta.y + 0.5));
              vec3 shapeCol = (shapeMode == 1) ? vec3(0.45, 0.9, 1.0) : ((shapeMode == 2) ? vec3(1.0, 0.65, 0.3) : vec3(0.95, 0.95, 0.2));
              vec3 baseCol = (brushMeta.x < 0.5) ? vec3(1.0, 0.35, 0.35) : shapeCol;
              vec3 brushCol = baseCol * (0.35 + 0.65 * ndl);
              outColor.rgb = mix(outColor.rgb, brushCol, 0.78);
              outColor.a = 1.0;
          }
      }
  }
 //outColor = linearDepth*vec4(1.);
// outColor.rgb = abs(ReconstructWorldPos(sceneDepth,uv));
  if(linearDepth<5.0){
  //outColor = vec4( n,1.);

  
  vec3 light = vec3(10.,-10.,10.);

      float dif = clamp(dot(normalize(light),n),0.,1.);

       vec3 reflectDir = reflect(normalize(light), n);
       float spec = pow(max(dot(rd, reflectDir), 0.0), 32.);
       //dif = 1.0;
       //spec = 1.0;
    //    outColor.rgb = vec3(sin(linearDepth*500.));
      // outColor.rgb = vec3(0.7,0.5,0.2)*dif*0.7+0.1+clamp(dot(rd,n)+1.,0.,1.)*0.5*vec3(1.,1.,1.)+spec*0.5;

//  outColor = vec4(1.)*sin(linearDepth*200.0);
  }else{
  //  outColor = vec4(0.2,0.,0.,1.);
  }
 // vec3 n = NormalFromDepth(uv);
  //outColor.rgb = normalize(n);
  /*
  float d = min(linearDepth, rayBox.x + rayBox.y);
  vec3 entryPoint = ro + rd * rayBox.x;
//  outColor.rgb = vec3(0.);

  if (rayBox.y > 0.0) {
    float t = rayMarchSdf(entryPoint, rd,  min(rayBox.y,linearDepth-rayBox.x));
    float sdfDis = length(camPos-(entryPoint+rd*t));

    if (t>0.0) {
     //outColor.rgb  = vec3(min(rayBox.x+t,linearDepth));
     vec3 sdfCol = calculateSdfLighting(ro, rd, t+rayBox.x);
    //  outColor.rgb = sdfCol;
  //    outColor.rgb = vec3(1.,0.,0.);
  //    outColor.rgb = vec3(1.,0.,0.);
     // outColor.rgb = pow(outColor.rgb,vec3(1./1.5));
    } else {
     // outColor.rgb *= vec3(1., 0., 1.);
    }
    //outColor.rgb =  entryPoint+rd*t;
     //outColor.rgb = vec3();
  }

  else {
  //  outColor.rgb = vec3(0.8);
  }
 // outColor.rgb = vec3(linearDepth);
//  outColor.a = 1.;
*/
  outColor.rgb *= outColor.a;
}
