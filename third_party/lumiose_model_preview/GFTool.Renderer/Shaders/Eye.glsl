#version 420 core
#include "Common/Surface.glsl"
// Eye material parameters are provided by the player's local material record.
layout(binding=0) uniform sampler2D BaseColorMap;
layout(binding=1) uniform sampler2D LayerMaskMap;
layout(binding=2) uniform sampler2D NormalMap;
layout(binding=3) uniform sampler2D HighlightMaskMap;
layout(binding=4) uniform sampler2D NormalMap1;
layout(binding=5) uniform sampler2D AOMap;
uniform bool EnableNormalMap, EnableAOMap, EnableHighlight, EnableOverrideColor;
uniform int NumMaterialLayer = 1;
uniform float LayerMaskScale1=1.0,LayerMaskScale2=1.0,LayerMaskScale3=1.0,LayerMaskScale4=1.0;
uniform vec4 BaseColor = vec4(1.0);
uniform vec4 BaseColorLayer1 = vec4(1.0), BaseColorLayer2 = vec4(1.0);
uniform vec4 BaseColorLayer3 = vec4(1.0), BaseColorLayer4 = vec4(1.0);
uniform vec4 EmissionColor = vec4(1.0), EmissionColorLayer1 = vec4(1.0);
uniform vec4 EmissionColorLayer2 = vec4(1.0), EmissionColorLayer3 = vec4(1.0);
uniform vec4 EmissionColorLayer4 = vec4(1.0), EmissionColorLayer5 = vec4(1.0);
uniform float EmissionIntensity, EmissionIntensityLayer1, EmissionIntensityLayer2;
uniform float EmissionIntensityLayer3, EmissionIntensityLayer4, EmissionIntensityLayer5;
uniform vec4 UVScaleOffset = vec4(1.0,1.0,0.0,0.0);
uniform vec4 UVScaleOffset1 = vec4(1.0,1.0,0.0,0.0);
uniform vec4 UVScaleOffsetNormal = vec4(1.0,1.0,0.0,0.0);
uniform vec4 UVCenter0, UVCenter1;
uniform float NormalHeight = 1.0;
vec2 eye_uv(vec2 uv, vec4 transform, vec2 center) {
    vec2 p = (uv-center-transform.zw)*transform.xy+center;
    return vec2(p.x,1.0-p.y);
}
#ifdef VERTEX_STAGE
void vertex() {}
#endif
#ifdef FRAGMENT_STAGE
void fragment() {
    vec2 uv = eye_uv(UV,UVScaleOffset,UVCenter0.xy);
    vec4 texel = sample_trinity(BaseColorMap,uv,0);
    vec3 color = texel.rgb * BaseColor.rgb;
    vec4 mask = clamp(sample_trinity(LayerMaskMap,uv,1)*vec4(LayerMaskScale1,LayerMaskScale2,LayerMaskScale3,LayerMaskScale4),0.0,1.0);
    if (NumMaterialLayer < 2) mask.r=0.0;
    if (NumMaterialLayer < 3) mask.g=0.0;
    if (NumMaterialLayer < 4) mask.b=0.0;
    if (NumMaterialLayer < 5) mask.a=0.0;
    vec4 colors[4] = vec4[4](BaseColorLayer1,BaseColorLayer2,BaseColorLayer3,BaseColorLayer4);
    vec3 emissions[4] = vec3[4](EmissionColorLayer1.rgb*EmissionIntensityLayer1,
        EmissionColorLayer2.rgb*EmissionIntensityLayer2,EmissionColorLayer3.rgb*EmissionIntensityLayer3,
        EmissionColorLayer4.rgb*EmissionIntensityLayer4);
    vec3 emission = EmissionColor.rgb*EmissionIntensity;
    for(int i=0;i<4;i++) {
        if(EnableOverrideColor) color=mix(color,colors[i].rgb,mask[i]);
        emission=mix(emission,emissions[i],mask[i]);
    }
    if(EnableHighlight && sampler_present[3]) {
        float highlight=sample_trinity(HighlightMaskMap,eye_uv(UV,UVScaleOffset1,UVCenter1.xy),3).r;
        color=mix(color,EmissionColorLayer5.rgb*EmissionIntensityLayer5,highlight);
    }
    if(EnableNormalMap) {
        vec2 xy=sample_trinity(NormalMap,eye_uv(UV,UVScaleOffsetNormal,vec2(0)),2).rg*2.0-1.0;
        NORMAL_MAP=vec3(xy,sqrt(max(1.0-dot(xy,xy),0.0)))*0.5+0.5;
        NORMAL_MAP_DEPTH=NormalHeight;
    }
    AO=EnableAOMap?sample_trinity(AOMap,uv,5).r:1.0;
    ALBEDO=color; EMISSION=emission; ALPHA=texel.a*BaseColor.a;
}
#endif
#ifdef VERTEX_STAGE
#include "Common/Vertex.glsl"
#endif
#ifdef FRAGMENT_STAGE
#include "Common/Fragment.glsl"
#endif
