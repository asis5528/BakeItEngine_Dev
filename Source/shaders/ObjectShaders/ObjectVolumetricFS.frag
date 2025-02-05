#version 450

layout(binding = 2) uniform sampler3D texSampler;

layout(binding = 1) uniform BasicUbo {
   vec4 color;
   vec4 sub;
} ubo;
layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 2) in vec3 localCoord;
layout(location=3) in vec3 cameraPos;
layout(location = 0) out vec4 outColor;


void main() {
  
   vec3 CamPos = cameraPos;
   vec3 vpos = localCoord;
   vec3 dir = normalize(localCoord-cameraPos);

  vec3 ro = CamPos;
  vec3 rd = dir;
  float t = 0.0;
    
    for(int i = 0;i<64;i++){
    
        vec3 cp = ro+rd*t;
        
        float d = length(cp)-0.1;
        float d2 = length(cp-vec3(0.3,0.,0.))-0.15;
        d = min(d,d2);
        if(d<0.01){
            break;
        }
        t+=d;
        if(t>20.0){
            break;
        }
    
    }
    
    vec3 col = vec3(0.,0.,0.);
    outColor.a = 0.0;
    if(t>0.0 && t<20.0){
    vec3 cp = ro+rd*t;
        col = normalize(cp);
        outColor.a = 1.0;
        
    }


 //  vec4 col = vec4(0.);

   vec2 uv = localCoord.zy;
   float c = smoothstep(0.3,0.2,length(uv));


   // outColor.rgb = mix(localCoord.xyz,vec3(c),0.5);
   outColor.rgb = col;
   // outColor.a = 1.;
   outColor.rgb*=outColor.a;

}