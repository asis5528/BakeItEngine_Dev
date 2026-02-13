#version 450
#extension GL_EXT_multiview : enable
//layout(early_fragment_tests) in;

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view[2];
    mat4 proj[2];
} ubo;

layout(binding = 2) uniform sampler3D texSampler;
layout(binding = 1) uniform BasicUbo {
   vec4 color;
} ubo2;

layout( binding = 3) readonly buffer BrickBuffer {
    float bricks[];   // single int
};


layout( std430,binding = 4) readonly buffer BlockBuffer {
    int blocks[];   // single int
};
layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec3 localCoord;
layout(location=3) in vec3 cameraPos;
layout(location=4) in vec3 worldPosition;
layout(location=5 ) flat in int brickIndex2;
layout(location=6) in vec3 camLocal;
layout(location=7) flat in float blockEmissive;
layout(location=8) flat in vec3 blockColor;
layout(location = 0) out vec4 outColor;

const int BLOCK_EMISSIVE_FLAG = (1 << 30);
const int BLOCK_INDEX_MASK = BLOCK_EMISSIVE_FLAG - 1;


vec4 rndC(vec3 u) {
    vec3 R = vec3(128.,128.,128.),
         U = u*R + .5,
         F = fract(U);
    U = floor(U) + F*F*(3.-2.*F); 
 // U = floor(U) + F*F*F*(F*(F*6.-15.)+10.);   // use if you want smooth gradients
    return texture( texSampler, u );
    // return texelFetch( texSampler, ivec3(u*R),0 );
}


vec3 cubicWarp(vec3 f) {
    // C1 continuous smootherstep
   // return f*f*(3.0-2.0*f);
   // return mix(f,f*f*(3.0 - 2.0*f),0.5);
    return f;
    // or use this for C2 (smoother gradients):
   //  return f*f*f*(f*(f*6.0 - 15.0) + 10.0);
}

// fast tricubic sampling (1 fetch, smooth curve)
float sampleSDF(sampler3D tex, vec3 uvw, vec3 texRes) {
    vec3 coord = uvw * texRes + 0.5;
    vec3 f = fract(coord);
    f = cubicWarp(f);                    // warp fractional part
    vec3 warped = floor(coord) + f;
    return texture(tex, (warped - 0.5) / texRes).r;
}
float s_curve(float x, float p)
{
    float x1 = pow(x, p);
    float x2 = pow(1.0 - x, p);
    return x1 / (x1 + x2);
}

vec3 s_curve(vec3 x, float p)
{
    x.x = s_curve(x.x, p);
    x.y = s_curve(x.y, p);
    x.z = s_curve(x.z, p);
    return x;
}

vec4 texelFetchR (sampler3D samplerr, ivec3 p, int mip)
{
    ivec3 res = textureSize(samplerr, 0);
    
    p.x = p.x % res.x;
    p.y = p.y % res.y;
    p.z = p.z % res.z;
    
    if(p.x < 0) p.x += res.x;
    if(p.y < 0) p.y += res.y;
    if(p.z < 0) p.z += res.z;

    return texelFetch(samplerr, p, mip);
}
vec4 fetchBuffer(sampler3D samplerr, ivec3 p,int mip){
    vec4 color;
    ivec3 res = textureSize(samplerr, 0);
    float bl = blocks[p.x + p.y*res.x + p.z*res.x*res.y];

    return vec4(bl);
}
vec4 texture3(sampler3D samplerr ,vec3 uv)
{
    ivec3 res = textureSize(samplerr, 0);
    uv.xy -= 0.5 / vec3(res).xy;
    ivec3 p = ivec3(floor(uv * float(res.x)));
    
    vec3 blend = fract(uv * vec3(res));
    blend = s_curve(blend, 1.3);
    
    vec4 col_000 = texelFetch(samplerr, p, 0);
    vec4 col_100 = texelFetch(samplerr, p + ivec3(1,0,0), 0);
    vec4 col_010 = texelFetch(samplerr, p + ivec3(0,1,0), 0);
    vec4 col_110 = texelFetch(samplerr, p + ivec3(1,1,0), 0);

    vec4 col_00 = mix(col_000, col_100, blend.x);
    vec4 col_10 = mix(col_010, col_110, blend.x);
    vec4 col_0 = mix(col_00, col_10, blend.y);
    
    
    vec4 col_001 = texelFetch(samplerr, p + ivec3(0,0,1), 0);
    vec4 col_101 = texelFetch(samplerr, p + ivec3(1,0,1), 0);
    vec4 col_011 = texelFetch(samplerr, p + ivec3(0,1,1), 0);
    vec4 col_111 = texelFetch(samplerr, p + ivec3(1,1,1), 0);

    vec4 col_01 = mix(col_001, col_101, blend.x);
    vec4 col_11 = mix(col_011, col_111, blend.x);
    vec4 col_1 = mix(col_01, col_11, blend.y);
    
    
    vec4 col = mix(col_0, col_1, blend.z);
    
    return col;
}

vec4 textureTrilinear(sampler3D sam, vec3 uv)
{
    ivec3 ires = textureSize(sam, 0);
    vec3  fres = vec3(ires);

    // texel space
    vec3 st = uv * fres - 0.5;
    ivec3 i = ivec3(floor(st));
    vec3  w = fract(st);

    // clamp to avoid OOB fetch
    i = clamp(i, ivec3(0), ires - ivec3(2));

    // fetch neighbors
    vec4 a = texelFetch(sam, i + ivec3(0,0,0), 0);
    vec4 b = texelFetch(sam, i + ivec3(1,0,0), 0);
    vec4 c = texelFetch(sam, i + ivec3(0,1,0), 0);
    vec4 d = texelFetch(sam, i + ivec3(1,1,0), 0);

    vec4 e = texelFetch(sam, i + ivec3(0,0,1), 0);
    vec4 f = texelFetch(sam, i + ivec3(1,0,1), 0);
    vec4 g = texelFetch(sam, i + ivec3(0,1,1), 0);
    vec4 h = texelFetch(sam, i + ivec3(1,1,1), 0);
   //w = w*w*(3.0-2.0*w);
  // w = 2.*w;
  // w = pow(w,vec3(2.));
   
    // interpolate
    vec4 slice0 = mix(mix(a, b, w.x), mix(c, d, w.x), w.y);
    vec4 slice1 = mix(mix(e, f, w.x), mix(g, h, w.x), w.y);
    return mix(slice0, slice1, w.z);
}
float fetchBricks(int brickIndex,ivec3 p){


 p = clamp(p, ivec3(0), ivec3(8) );

int localIndex = p.x + p.y*9 + p.z*9*9;

    return bricks[brickIndex+localIndex];

}

vec2 map(vec3 p,int brickks){
float k = 1./64.;
vec3 cpos =  p+vec3(0.5);
vec3 ppos = cpos;


vec3 cpos2 =  p+vec3(0.5+0.000);
vec3 texRes = vec3(textureSize(texSampler, 0));
vec4 h1 = texture( texSampler, ppos);;
//vec3 h1 = texelFetch(texSampler,ivec3(cpos.x*64.-1.,cpos.y*64.,cpos.z*64.),0).rgb;
float time = ubo2.color.a;

//float h = h1.r;
float h = 0.;

int blockPerAxis = 64;
ivec3 ires = ivec3(blockPerAxis,blockPerAxis,blockPerAxis);
    vec3  fres = vec3(ires);

    // texel space
    vec3 st = clamp(cpos * fres - 0.5,0.,fres.x-1);
    vec3 st2 = clamp(cpos2 * fres - 0.5,0.,fres.x-1);
    ivec3 i = ivec3(floor(st2)+0.5);
    i = max(i,ivec3(0));
   // bricks[i.x+i.y*8+i.z*64] = h1.r;
  // blocks[i.x+i.y*8+i.z*64] = int(h1.r*255.);
  float mat = 0.;
//h = textureTrilinear( texSampler, cpos).r;
int blockRaw = blocks[i.x+i.y*blockPerAxis+i.z*blockPerAxis*blockPerAxis];
int brickIndex = (blockRaw & BLOCK_INDEX_MASK);
int oldBrick = brickIndex;
brickIndex = brickks;
if(oldBrick!=brickIndex){
   brickIndex = oldBrick;
   mat = 1.0;
}
 //brickIndex = brickks;
if(i.x<=7||i.z<=7||i.y<=7){
   // brickIndex = blocks[i.x+i.y*blockPerAxis+i.z*blockPerAxis*blockPerAxis];
}
 //brickIndex = brickks;
//int brickIndex = bricksInd;
//int brickIndex = brickIndex;
//h = 0.;
//int brickIndex = brickIndex2;
if(blockRaw>=0){
vec3 newcord = cpos * fres;

ivec3 localGrid = ivec3(floor(fract(newcord+0.5)*8));
vec3 localfract = vec3(fract(fract(newcord+0.)*8));

vec3 fw = fract((fract(st))*9*(8./9.));
ivec3  w = ivec3(floor(fract(st)*8.));

fw = localfract;
w = localGrid;
//w = w*int(floor(7./8.));
    int localIndex = w.x + w.y*9 + w.z*9*9;
    bool np = false; 
  
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

    h = a;
    //h=fetchBricks(brickIndex,w);
}
else if(blockRaw!=-999999){
    h = float(blockRaw);
}

else{
    h = 0.01;
}


vec3 q = p;
float y = sin(time*0.02);
float off = sin((1.57075-abs(y-0.1)));
q.y+=y*0.3;
q.x+=(sin(time)*0.3)*off;

q.z+=(cos(time)*0.3)*off;


vec3 pq = p;
pq+=h1.gba;
float eraser  = length(pq)-(0.02);

float d =  h;
if(eraser<d&&time>0.){
//mat = 1.5;
//d = eraser;
}
return vec2(d,mat);
}

vec2 map2(vec3 p){
float mat = 0.0;
p = vec3(p.x,p.y,p.z);
//p = max(p,-0.5);
//p = clamp(p,-0.5,0.5);
ivec3 localGrid = ivec3(floor((p+0.50)*8.));

   ivec3 w = localGrid;
  // w = abs(w);
  vec3 pp = (p+0.5)*8.;
   vec3 fw = fract(pp);
//   fw = pow(fw,vec3(3.0));
   fw = clamp(fw, vec3(0.00), vec3(1.));
 //  fw = pow(fw,vec3(.0));
   int brickIndex = brickIndex2;
    float a = fetchBricks(brickIndex,w + ivec3(0,0,0));
    float b = fetchBricks(brickIndex,w + ivec3(1,0,0));
    float c = fetchBricks(brickIndex,w + ivec3(0,1,0));
    float d = fetchBricks(brickIndex,w + ivec3(1,1,0));

    float e = fetchBricks(brickIndex,w + ivec3(0,0,1));
    float f = fetchBricks(brickIndex,w + ivec3(1,0,1));
    float g = fetchBricks(brickIndex,w + ivec3(0,1,1));
    float k = fetchBricks(brickIndex,w + ivec3(1,1,1));

    float slice0 = mix(mix(a, b, fw.x), mix(c, d, fw.x), fw.y);
    float slice1 = mix(mix(e, f, fw.x), mix(g, k, fw.x), fw.y);
    float hn =  mix(slice0,slice1 , fw.z);


   float h = fetchBricks(brickIndex,w + ivec3(0,0,0));// bricks[blockIndex] ;
   bool np = false;


   h = hn;
   if(np) h = hn;


return vec2( h*64., mat);
}


vec3 calcNormalTetra(vec3 p, int bricksInd, float e)
{
    // 4 offset directions (tetrahedron)
    vec3 k1 = vec3( 1, -1, -1);
    vec3 k2 = vec3(-1, -1,  1);
    vec3 k3 = vec3(-1,  1, -1);
    vec3 k4 = vec3( 1,  1,  1);

    float d1 = map2(p + e*k1).x;
    float d2 = map2(p + e*k2).x;
    float d3 = map2(p + e*k3).x;
    float d4 = map2(p + e*k4).x;

    vec3 g = k1*d1 + k2*d2 + k3*d3 + k4*d4;
    return normalize(g);
}

vec3 calcNormal(vec3 pos,int bricksInd) {
    vec3 texRes = vec3(textureSize(texSampler, 0));
    float e = 1.0 / (max(max(texRes.x, texRes.y), texRes.z)); // one voxel step
    vec3 eps = vec3(e, 0.0, 0.0);
    //bricksInd = -20;
    return normalize(vec3(
        map(pos + eps.xyy,bricksInd).x - map(pos - eps.xyy,bricksInd).x,
        map(pos + eps.yxy,bricksInd).x - map(pos - eps.yxy,bricksInd).x,
        map(pos + eps.yyx,bricksInd).x - map(pos - eps.yyx,bricksInd).x
    ));
}

vec3 normalFromDerivatives(float dist, vec3 pos)
{
    vec3 dpdx = dFdx(pos);
    vec3 dpdy = dFdy(pos);

    float ddx = dFdx(dist);
    float ddy = dFdy(dist);

    // gradient in world/object space (up to scale)
    vec3 g = ddx * dpdx + ddy * dpdy;

    return normalize(g);
}
vec3 calcNormal2(vec3 pos) {
    vec3 texRes = vec3(textureSize(texSampler, 0));
    float e = 1.0 / (8.0);
    vec3 eps = vec3(e, 0.0, 0.0);

    return normalize(vec3(
        map2(pos + eps.xyy).x - map2(pos - eps.xyy).x,
        
        map2(pos + eps.yxy).x - map2(pos - eps.yxy).x,
        map2(pos + eps.yyx).x - map2(pos - eps.yyx).x
    ));
}


vec3 norm(vec3 u){
    float ep = 0.001;
    return normalize(vec3(
    rndC(vec3(u.x+ep,u.y,u.z)).w-rndC(vec3(u.x-ep,u.y,u.z)).w,
    rndC(vec3(u.x,u.y+ep,u.z)).w-rndC(vec3(u.x,u.y-ep,u.z)).w,
    rndC(vec3(u.x,u.y,u.z+ep)).w-rndC(vec3(u.x,u.y,u.z-ep)).w
    ));

}
 
    float voxelValue(vec3 pos, ivec3 dims) {
    // pos is in [0..1] cube, scaled to grid
    vec3 st = pos;
    vec3 gridPos = pos * vec3(dims); 
    ivec3 base = ivec3(floor(gridPos));
    vec3 f = fract(gridPos);

    // clamp to bounds
    base = clamp(base, ivec3(0), dims-ivec3(2)); 

    // fetch 8 neighbors
    int idx000 = base.x + base.y*dims.x + base.z*dims.x*dims.y;


    float v000 = float(blocks[idx000]);

  float texResolution = float(textureSize(texSampler,0).x);
    pos = pos*texResolution + 0.5;
    vec3 i = floor(pos);
    vec3 f1 = pos - i;
    pos = i + f1*f1*f1*(f1*(f1*6.0-15.0)+10.0);
    pos = (pos - 0.5)/texResolution;
    return textureTrilinear(texSampler,st).g;
}

int voxelAt(ivec3 c) {
        // clamp to grid bounds
        int dimX = 8;
        int dimY = 8;
        c = clamp(c, ivec3(0), ivec3(dimX-1, dimY-1, 8-1));
        int idx = int(c.x + c.y * dimX + c.z * dimX * dimY);
        return int(blocks[idx]);
    }
vec3 voxelNormalFaces(ivec3 coord, int dimX, int dimY) {
    vec3 n = vec3(0.0);
    int idx = coord.x + coord.y * dimX + coord.z * dimX * dimY;
    int strideY = dimX;
    int strideZ = dimX * dimY;

    if (blocks[idx+1] == 0) n += vec3( 1,0,0);
    if (blocks[idx-1] == 0) n += vec3(-1,0,0);
    if (blocks[idx+strideY] == 0) n += vec3(0, 1,0);
    if (blocks[idx-strideY] == 0) n += vec3(0,-1,0);
    if (blocks[idx+strideZ] == 0) n += vec3(0,0, 1);
    if (blocks[idx-strideZ] == 0) n += vec3(0,0,-1);

    return normalize(n);
}
vec3 colorRamp(float v, float minVal, float maxVal)
{
    // normalize v into [0,1]
    float t = clamp((v - minVal) / (maxVal - minVal), 0.0, 1.0);

    vec3 blue   = vec3(0.0, 0.0, 1.0); // start
    vec3 yellow = vec3(1.0, 1.0, 0.0); // middle
    vec3 red    = vec3(1.0, 0.0, 0.0); // end

    // transition points at 1/3 and 2/3
    float t1 = 1.0 / 3.0;
    float t2 = 2.0 / 3.0;

    // rescale to [0,1] within segment
    vec3 col;
    if (t < t1) {
        col = mix(blue, yellow, t / t1);           // blue → yellow
    } else if (t < t2) {
        col = mix(yellow, red, (t - t1) / (t2 - t1)); // yellow → red
    } else {
        col = red; // clamp end
    }

    return col;
}

vec3 voxelNormalInterp(vec3 pos, ivec3 dims) {
    float e = 0.01; // one voxel step
    float dx = voxelValue(pos + vec3(e,0,0), dims) - voxelValue(pos - vec3(e,0,0), dims);
    float dy = voxelValue(pos + vec3(0,e,0), dims) - voxelValue(pos - vec3(0,e,0), dims);
    float dz = voxelValue(pos + vec3(0,0,e), dims) - voxelValue(pos - vec3(0,0,e), dims);
    return normalize(vec3(dx, dy, dz));
}

const vec3 sundir = vec3(-0.7071,1.0,1.7071);

void main() {
   // outColor = mix(vec4(fragColor,1.),texture(texSampler, fragTexCoord),0.99)*(sin(uboo.time1*5.));
   /*
   vec3 CamPos = cameraPos;
   vec3 vpos = localCoord;
   vec3 dir = normalize(localCoord-cameraPos);
   bool hitting = false;
    
   float t = 0.;
    vec3 p = vpos;
    vec3 p2 = vpos;
    vec3 norPos = vpos+0.5;
    float hit = 0.;
    float mat = 0.;
    //counter = blocks[128];
    ivec3 Dimension =  ivec3(textureSize(texSampler, 0));;
    float xm = fract(norPos.x*8.);
    float ym = fract(norPos.y*8.);
    int ind = int((floor(norPos.x*8.)+floor(norPos.y*8.)*8.+floor(4.)*64.));
  int b = int(blocks[ind]);
  float n = 0.0;
  if(b>0){
   n = 1.0;
  }
   // atomicMax(counter,b);
 //   atomicMax(counter,blocks[256]);
 float dist = 0.0;
 float sum = 0.0;
  for(int i = 0;i<556;i++){
     if(max(abs(p.x), max(abs(p.y), abs(p.z))) < 0.50001){
     
        p2=vpos+dir*t;
        vec3 p2nor = p2+0.5;
       //  int ind = int((floor(p2nor.x*8.)+floor(p2nor.y*8.)*8.+floor(p2nor.z*8.)*64.));
  float b = voxelValue(p2+0.5,Dimension);
  if(b<50.){
    sum = 1.0;
    break;
  }
        t+=0.01;
     }
  
  }
  vec3 norVoxel = vec3(0.);
  if(sum>0.9){
 // vec3 p2nor = p2+0.5;
         
  // norVoxel = voxelNormalFaces(ivec3((p2+0.5)*8.), 8, 8);
    norVoxel = voxelNormalInterp(p2+0.5,Dimension);


  }
  float sm = 0.0;
  float hn = 0.0;
   for(int i = 0;i<256;i++){
  

       if(max(abs(p.x), max(abs(p.y), abs(p.z))) < 0.50001){
        
           p= vpos+dir*t;
           //sm+=0.01;
           vec2 h = map(p);
           if(h.x<0.0001){ 
           hit = 1.; 
           hitting = true;
           mat = h.y;
            break;
         
     }
          t+=h.x;
       }
   }
   
   vec4 col = vec4(0.);
   /*
   int found = 0;
    for(int i = 0;i<128;i++){
  

       if(max(abs(p.x), max(abs(p.y), abs(p.z))) < 0.50001){
        
        p= vpos+dir*t;
        vec3 pos = p+0.5;
            float den = rndC(pos ).w;
            if(den>0.0){
            if(found==0){
                //gl_FragDepth = t;
              //  found ++;
            }

                 float sunDiffuse = clamp((den - rndC(pos+1.*sundir).w)/1., 0.0, 1.0 ); 

            den*=(0.25);   
		vec3  finalShade = vec3(1.,0.5,0.1)*sunDiffuse+vec3(0.8,0.8,1.0)*0.1; 
	
        vec4  c = vec4( mix( vec3(0.8,0.75,0.65), vec3(0.25,0.3,0.85), den*0.01 ), den ); 
        
       
        c.a*=1.;
   //c.xyz = vec3(1.);
	c.xyz = c.xyz*0.5+finalShade*0.7;
		//c.xyz = vec3(1.)*(max(dot(norm(pos),vec3(0.3,1.0,-1.0)),0.0)+den);



     //   c.w *= 0.4; 
     c.rgb *= c.a; 
		col += c*(1.0-col.a); 
        if(col.a>=0.999) break;
      
            }
           // if(col.a>0.99) break;
           // if(t>0.7) break;
            t+=0.01;
            }
       }
       col.rgb*=1.+col.a*0.35;
       
       //co.rgb = mix(vec3(1.,0.7,0.7),col.rgb,0.7);
      // col.rgb = pow(col.rgb,vec3())
    //   col *= vec4(abs(p),1.);
   vec3 ro = localCoord;
   */
   /*
    outColor.rgb = vec3(0.);
    outColor.a = 0.;
    float disst = length((vpos+dir*t)-CamPos);
    if(hit>0.){
     vec3 n=normalize(calcNormal( vpos+dir*t));
      //vec3 n = norVoxel;

      vec3 light = vec3(10.,10.,10.);

      float dif = clamp(dot(normalize(light),n),0.,1.);

      vec3 reflectDir = reflect(normalize(light), n);
      float spec = pow(max(dot(dir, reflectDir), 0.0), 32.);
      //dif = 1.0;
      //spec = 1.0;
      outColor.rgb = vec3(0.8,0.5,0.2)*dif*0.7+0.1+clamp(dot(dir,n)+1.,0.,1.)*0.5*vec3(1.,1.,1.)+spec*0.5;
      // outColor.rgb = normalize(vpos+dir*t);
      if(mat>1.){
      outColor.rgb = vec3(0.8,0.0,0.0)*dif*0.5+0.5+clamp(dot(dir,n)+1.,0.,1.)*0.3* vec3(0.8,0.0,0.0);
     
      }
      outColor.rgb = pow(outColor.rgb,vec3(1./1.5));
     // outColor.rgb  = abs(vpos+dir*t);
    
    }

    int blk = blocks[ind];
       // outColor.rgb = vec3(max(blk,0.)*0.0005);
      //  vec3 texRes = vec3(textureSize(texSampler, 0));
     //outColor.rgb = col.rgb;
    //outColor.a = col.a*10.;
   //  outColor.rgb = vec3(norVoxel);
  // outColor.rgb =mix(colorRamp(sm,0.,1.)*hit,outColor.rgb,1.-sm) ;
 //outColor.rgb = vec3(n);
 //outColor.rgb = vec3(hn);
// outColor.rgb  = vec3(gl_FragCoord.xy/vec2(512.,512.),0.);
//hit = 1.0;
outColor.a = 1.0;
   //outColor.a =  hit;
   if (!hitting) {
   // discard;        // no color, no depth written
}
    

   //outColor.rgb*=outColor.a;
  // outColor.rgb = vec3(abs((blocks[int(700)]))*100.);
   //outColor.rgb = vec3(1.,0.5,0.2);
//   outColor.rgb= vec3(t);
 //outColor.rgb = vpos+dir*t;
     // float depth = gl_FragCoord.z;   // 0→1 depth
    float bias  = -0.000;          // negative pulls closer
    if(hit<0.5){
    //t = 0.0;
    }
    float nearPlane  = 0.1;
    float farPlane   = 1000.0;
    float linear = length(vpos-(vpos+dir*t));
    float z = (nearPlane + farPlane - (2.0 * nearPlane * farPlane) / linear) 
              / (farPlane - nearPlane);

    z=  (z + 1.0) * 0.5; // convert NDC [-1,1] → depth [0,1]
     //vec4 clip = ubo.proj[0] * ubo.view[0] * vec4((CamPos- vpos+dir*t), 1.0);
    // float md = vpos+dir*t;
    // NDC in Vulkan: z is already 0..1 after /w
    //float ndcDepth = clip.y / clip.w;
  //  outColor.rgb = vec3(z);
    outColor.rgb = vpos;
    outColor.a = 1.0;
    if(hitting){
    gl_FragDepth = gl_FragCoord.z;
    }else{
    gl_FragDepth = gl_FragCoord.z;
    }*/
  //  gl_FragDepth = 0.5;

  vec3 CamPos = cameraPos;
   vec3 vpos = localCoord;

   vec3 dirLocal = normalize(localCoord - camLocal);

   vec3 dir = normalize(worldPosition-cameraPos.xyz);
   vpos = worldPosition;
   dir = vec3(dir.x,dir.y,dir.z);
  // ivec3 localGrid = ivec3(floor(fract(newcord-0.5)*8));
   float t = 0.0;
   bool hit = false;
   float iter = 0.0;
   float sum = 0.0;
   vec3 p = vpos;

    vec3 pos = localCoord;
   vec3 vpos2 = vpos+vec3(0.005,0.005,0.005)*0.;
    sum = 0.0;
    float tn = 0.0;
   

    
    int brickIndexx;
    float cnt = 0.0;
    t = 0.0;
    float minStep = 0.001;
float maxStep = 0.05;
float dis = length(worldPosition-cameraPos.xyz);
int siz = int(132/max(dis*4.,1.));
float tPrev = 0.0;
float dPrev = 1e9;      // previous distance sample

for (int i = 0; i < siz; i++)
{
    vec3 pos = localCoord + dirLocal * t;

    if (max(abs(pos.x), max(abs(pos.y), abs(pos.z))) >= 0.505)
        break;

    float d = map2(pos).x;   // your SDF-ish distance (whatever units map2 returns)

    // If we are "close enough" (or crossed), refine using linear interpolation
    if (d < 0.0005)
    {
        // We want approximate t where d == 0.
        // Interpolate between previous sample (tPrev, dPrev) and current (t, d).
        // Note: if dPrev was also already < 0.001 (or invalid), fall back.
        float denom = (dPrev - d);

        float alpha = 0.0;
        if (abs(denom) > 1e-8)
            alpha = clamp(dPrev / denom, 0.0, 1.0); // fraction from prev->current where d hits ~0

        float tHit = mix(tPrev, t, alpha);

        t = tHit;
        hit = true;
        break;
    }

    // store previous sample BEFORE advancing
    tPrev = t;
    dPrev = d;

    // advance (your original scaling)
    t += max(d, 0.0005);
}

    /*
   for(int i = 0;i<8;i++){
  
  cnt+=1./10.;
       if(max(abs(p.x), max(abs(p.y), abs(p.z))) < 0.50001){
        
           p= vpos2+dirLocal*t;


           vec3 cpos =  vpos2+vec3(0.5);
vec3 ppos = cpos;




int blockPerAxis = 64;

ivec3 ires = ivec3(blockPerAxis,blockPerAxis,blockPerAxis);
    vec3  fres = vec3(ires);

    // texel space
    vec3 st = clamp(cpos * fres - 0.5,0.,fres.x-1);
    ivec3 i = ivec3(floor(st));
    
   // bricks[i.x+i.y*8+i.z*64] = h1.r;
  // blocks[i.x+i.y*8+i.z*64] = int(h1.r*255.);

//h = textureTrilinear( texSampler, cpos).r;
  brickIndexx = blocks[i.x+i.y*blockPerAxis+i.z*blockPerAxis*blockPerAxis];

           //sm+=0.01;
           vec2 h = map(p,brickIndexx);
           
           float den = -h.x;
           if(den>0.0){
              //  sum+=den*50.*(1.-sum);           
           }
           if(h.y>0.5) sum+=1.;
           if(h.x<-0.00){ 
           if(!hit) tn = t;
           hit = true; 
        //   hitting = true;
          // mat = h.y;
            break;
         
     }
     if(t>0.015625) break;
    // t+=0.001953125;
    //t+=h.x;
    t+=max(h.x,0.001953125);
   // t+=h.x;
  //  t+=max(h.x,0.001953125);
  //        t+=0.001953125*2.;
       }
       }*/
        
  float c = smoothstep(0.5,0.,length(localCoord.yz))+smoothstep(0.5,0.,length(localCoord.xy));
//  outColor.rgb = vec3(c*0.2);
  //outColor.rgb = smoothstep(0.5,0.0,length(worldPosition.xy))*vec3(1.);
   //outColor.a = 1.;
  //outColor.rgb = vec3(sum);
  //outColor.a = sum;
  //outColor.rgb*=outColor.a;
  float border = float(abs(localCoord.x)>0.45 && abs(localCoord.x)<0.49) +float(abs(localCoord.y)>0.45&& abs(localCoord.y)<0.49)+float(abs(localCoord.z)>0.45&& abs(localCoord.z)<0.49);
 //border = 0.0;
 if(hit){
// t=  tn;
  vec3 cpos =  vpos+dir*t+vec3(0.5);
vec3 ppos = cpos;




 int blockPerAxis = 64;

ivec3 ires = ivec3(blockPerAxis,blockPerAxis,blockPerAxis);
    vec3  fres = vec3(ires);

    // texel space
    vec3 st = clamp(cpos * fres - 0.5,0.,fres.x-1);
    ivec3 i = ivec3(floor(st));

 brickIndexx = (blocks[i.x+i.y*blockPerAxis+i.z*blockPerAxis*blockPerAxis] & BLOCK_INDEX_MASK);
 brickIndexx = brickIndex2;
 //float d = map(vpos2+dir*t, brickIndexx).x;

// vpos2+dir*t

//vec3 n = normalFromDerivatives(t/64., worldPosition+dir*(t/64.));
//n = calcNormalTetra( localCoord+dirLocal*t,brickIndexx,1./(8.));
  vec3 n = normalize(calcNormal2( localCoord+dirLocal*t));

  vec3 light = vec3(10.,10.,10.);

      float dif = clamp(dot(normalize(light),n),0.,1.);

       vec3 reflectDir = reflect(normalize(light), n);
       float spec = pow(max(dot(dir, reflectDir), 0.0), 32.);
       //dif = 1.0;
       //spec = 1.0;
       vec3 baseColor = blockColor;
       outColor.rgb = baseColor * dif * 0.7 + 0.1 + clamp(dot(dir, n) + 1., 0., 1.) * 0.5 * vec3(1.) + spec * 0.5;
       if (blockEmissive > 0.5) {
           // Per-block emissive shading: only blocks created in emissive mode glow.
           outColor.rgb = baseColor * (1.8 + 0.2 * dif);
       }
//outColor.rgb = normalize(n);
float nt = length((localCoord+dirLocal*t));
 //outColor.rgb = (t*1000.)*vec3(1.);
  //outColor.rgb = mix(outColor.rgb,vec3(1.,0.,0.),min(sum,1.));
  //outColor.rgb = vec3(1.)*(sin(length(vpos2+dir*t)*100.));
  // outColor.rgb*=cnt;
    // outColor.rgb+=border;
      // outColor.rgb = mix(outColor.rgb,vec3(0.,0.,0.),border);
       //gl_FragDepth = gl_FragDepth+t;
   // outColor.rgb =n+border;
     //outColor.rgb  = vec3(dir.x,abs(dir.y),dir.z);
     //gl_FragDepth = gl_FragDepth;
//   outColor.a = float(min(sum*20.,1.0));
   outColor.a = 1.0;
  // outColor.rgb*=outColor.a;
  // gl_FragDepth = gl_FragCoord.z;
  
    vec4 clip = ubo.proj[gl_ViewIndex] *
                  ubo.view[gl_ViewIndex] *
                // ubo.model*
                  vec4(worldPosition+dir*(t/64.), 1.0);
   gl_FragDepth = clip.z / clip.w;    
  //gl_FragDepth = gl_FragCoord.z;
outColor.a = 1.0;
  }else{
    gl_FragDepth = gl_FragCoord.z;
 // discard;
  // outColor.a = 1.;
//  discard;
if(border<=0.0){

discard;
//outColor.a = 0.0;
} //discard;
outColor.a = 1.0;

//outColor.a = 0.0;
//discard;
//outColor.a = 1.0;
//discard;
//gl_FragDepth = 100.0;
//gl_FragDepth = gl_FragCoord.z;
 
 // outColor.rgb = vec3(iter);
 //  outColor.rgb  = vec3(dir);
  // outColor.rgb  = localCoord;
  outColor.rgb = vec3(0.,0.,0.);
  outColor.rgb*=outColor.a;
  }
  
  
 //outColor.rgb = vec3(float(brickIndex2));
 //outColor.a = 1.0;
 // outColor.rgb = vec3(worldPosition.y);
  //outColor.rgb = sin(float(brickIndex))*vec3(1.);
 //brickIndex
 //outColor.a =1.0;
 // outColor.a = 1.0;
//  if(localCoord.z>0.4) discard;
  //outColor.rgb = worldPosition.xyz;
 // vec3 p = localCoord;
  //vec3 fw = fract((p+0.5)*8.);
  //outColor.rgb = vec3(1.)*fw.y;
 // outColor.rgb = (localCoord.z+0.5)*vec3(1.);
  //outColor.a = 1.0;
//  outColor.rgb = dir;
 // outColor.rgb =n*vec3(0.5);
   
}
