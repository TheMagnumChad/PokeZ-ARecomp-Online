#version 420 core
#include "Common/Surface.glsl"

layout(binding = 0) uniform sampler2D BaseColorMap ;
layout(binding = 1) uniform sampler2D BaseColorMap1 ;
layout(binding = 2) uniform sampler2D NormalMap ;
layout(binding = 3) uniform sampler2D NormalMap1 ;
layout(binding = 4) uniform sampler2D NormalMap2 ;
layout(binding = 5) uniform sampler2D MetallicMap;
layout(binding = 6) uniform sampler2D RoughnessMap;
layout(binding = 7) uniform sampler2D RoughnessMap1;
layout(binding = 8) uniform sampler2D AOMap;
layout(binding = 9) uniform sampler2D AOMap1;
layout(binding = 10) uniform sampler2D AOMap2;
layout(binding = 11) uniform sampler2D LayerMaskMap;
layout(binding = 12) uniform sampler2D ParallaxMap;
layout(binding = 13) uniform sampler2D EmissionColorMap;
layout(binding = 14) uniform sampler2D LightMap;
layout(binding = 15) uniform sampler2D WindMaskMap;

uniform bool EnableBaseColorMap;
uniform bool EnableNormalMap;
uniform bool EnableParallaxMap;
uniform bool EnableMetallicMap;
uniform bool EnableRoughnessMap;
uniform bool EnableEmissionColorMap;
uniform bool EnableAOMap;
uniform bool EnableAlphaTest;
uniform bool EnableLightMap;
uniform int NumMaterialLayer;
uniform int BillboardType;
uniform int WindReceiverType;
uniform bool EnableWindMaskMap;
uniform bool EnableBaseColorMap1;
uniform bool EnableNormalMap1;
uniform bool EnableNormalMap2;
uniform bool EnableRoughnessMap1;
uniform bool EnableAOMap1;
uniform bool EnableAOMap2;
uniform int LayerMaskSource;
uniform int LayerMaskSwizzle;
uniform int LayerBaseMaskSource;
uniform bool EnableVertexBaseColor;
uniform int NumDetailLayer;
uniform bool EnableDepthFade;
uniform bool ReceiveShadow;
uniform bool EnableDeferredRendering;
uniform int InstancingType;
uniform bool EnableGrassCollisionMap;
uniform int NumRequiredUV;
uniform bool EnableLerpBaseColorEmission;
uniform int LayerBlendType;
uniform bool EnableTriplanar;
uniform bool EnablePackedMap;
uniform bool EnableDisplacementMap;


uniform float EmissionIntensity;
uniform vec4 BaseColor = vec4(1.0), BaseColorLayer1 = vec4(1.0);
uniform vec4 UVScaleOffset1 = vec4(1.0,1.0,0.0,0.0);
uniform float NormalHeight = 1.0, NormalHeight1 = 1.0;
uniform float Metallic, Roughness = 0.5, LayerMaskScale1 = 1.0;
uniform float FresnelAlphaMin = 1.0, FresnelAlphaMax = 1.0, FresnelAngleBias = 0.5;
uniform float LocalSpecularProbeIntensity, EmissionIntensityLayer1;
layout(binding=16) uniform samplerCube LocalSpecularProbe;



uniform vec4 UVScaleOffset = vec4(1.0,1.0,0.0,0.0);
uniform vec4 EmissionColor = vec4(1.0);
vec4 sample_BaseColorMap(vec2 uv) { return sample_trinity(BaseColorMap,uv,0); }
vec4 sample_lod_BaseColorMap(vec2 uv,float lod) { return sample_trinity(BaseColorMap,uv,0); }
vec4 sample_BaseColorMap1(vec2 uv) { return sample_trinity(BaseColorMap1,uv,1); }
vec4 sample_lod_BaseColorMap1(vec2 uv,float lod) { return sample_trinity(BaseColorMap1,uv,1); }
vec4 sample_NormalMap(vec2 uv) { return sample_trinity(NormalMap,uv,2); }
vec4 sample_lod_NormalMap(vec2 uv,float lod) { return sample_trinity(NormalMap,uv,2); }
vec4 sample_NormalMap1(vec2 uv) { return sample_trinity(NormalMap1,uv,3); }
vec4 sample_lod_NormalMap1(vec2 uv,float lod) { return sample_trinity(NormalMap1,uv,3); }
vec4 sample_NormalMap2(vec2 uv) { return sample_trinity(NormalMap2,uv,4); }
vec4 sample_lod_NormalMap2(vec2 uv,float lod) { return sample_trinity(NormalMap2,uv,4); }
vec4 sample_MetallicMap(vec2 uv) { return sample_trinity(MetallicMap,uv,5); }
vec4 sample_lod_MetallicMap(vec2 uv,float lod) { return sample_trinity(MetallicMap,uv,5); }
vec4 sample_RoughnessMap(vec2 uv) { return sample_trinity(RoughnessMap,uv,6); }
vec4 sample_lod_RoughnessMap(vec2 uv,float lod) { return sample_trinity(RoughnessMap,uv,6); }
vec4 sample_RoughnessMap1(vec2 uv) { return sample_trinity(RoughnessMap1,uv,7); }
vec4 sample_lod_RoughnessMap1(vec2 uv,float lod) { return sample_trinity(RoughnessMap1,uv,7); }
vec4 sample_AOMap(vec2 uv) { return sample_trinity(AOMap,uv,8); }
vec4 sample_lod_AOMap(vec2 uv,float lod) { return sample_trinity(AOMap,uv,8); }
vec4 sample_AOMap1(vec2 uv) { return sample_trinity(AOMap1,uv,9); }
vec4 sample_lod_AOMap1(vec2 uv,float lod) { return sample_trinity(AOMap1,uv,9); }
vec4 sample_AOMap2(vec2 uv) { return sample_trinity(AOMap2,uv,10); }
vec4 sample_lod_AOMap2(vec2 uv,float lod) { return sample_trinity(AOMap2,uv,10); }
vec4 sample_LayerMaskMap(vec2 uv) { return sample_trinity(LayerMaskMap,uv,11); }
vec4 sample_lod_LayerMaskMap(vec2 uv,float lod) { return sample_trinity(LayerMaskMap,uv,11); }
vec4 sample_ParallaxMap(vec2 uv) { return sample_trinity(ParallaxMap,uv,12); }
vec4 sample_lod_ParallaxMap(vec2 uv,float lod) { return sample_trinity(ParallaxMap,uv,12); }
vec4 sample_EmissionColorMap(vec2 uv) { return sample_trinity(EmissionColorMap,uv,13); }
vec4 sample_lod_EmissionColorMap(vec2 uv,float lod) { return sample_trinity(EmissionColorMap,uv,13); }
vec4 sample_LightMap(vec2 uv) { return sample_trinity(LightMap,uv,14); }
vec4 sample_lod_LightMap(vec2 uv,float lod) { return sample_trinity(LightMap,uv,14); }
vec4 sample_WindMaskMap(vec2 uv) { return sample_trinity(WindMaskMap,uv,15); }
vec4 sample_lod_WindMaskMap(vec2 uv,float lod) { return sample_trinity(WindMaskMap,uv,15); }


#ifdef VERTEX_STAGE
void vertex() {

}
#endif


vec2 xform_uvs(vec2 uvs, vec4 scale_off)
{
	vec2 v = scale_off.xy * (uvs - scale_off.zw);
	return vec2(v.x, 1.0 - v.y);
}

vec2 get_uv(vec2 uvs) 
{
    return xform_uvs(uvs, UVScaleOffset);
}

#ifdef FRAGMENT_STAGE
void fragment() {
    vec2 uv = get_uv(UV);
    vec2 uv1 = xform_uvs(UV2,UVScaleOffset1);
    vec4 base = BaseColor * (EnableBaseColorMap ? sample_BaseColorMap(uv) : vec4(1.0));
    float facing = clamp(abs(dot(normalize(NORMAL),normalize(VIEW))),0.0,1.0);
    float fresnel = clamp(pow(1.0-facing, max(0.1,2.0*FresnelAngleBias)),0.0,1.0);
    float weight = sampler_present[11] ? clamp(sample_LayerMaskMap(uv).r*LayerMaskScale1,0.0,1.0) : fresnel;
    if(EnableBaseColorMap1) base.rgb=mix(base.rgb,BaseColorLayer1.rgb*sample_BaseColorMap1(uv1).rgb,weight);
    if(EnableNormalMap) {
        vec2 xy=sample_NormalMap(uv).rg*2.0-1.0;
        if(EnableNormalMap1) xy=mix(xy,(sample_NormalMap1(uv1).rg*2.0-1.0)*NormalHeight1,weight);
        NORMAL_MAP=vec3(xy,sqrt(max(1.0-dot(xy,xy),0.0)))*0.5+0.5;
        NORMAL_MAP_DEPTH=NormalHeight;
    }
    ALBEDO=base.rgb;
    ALPHA=base.a*clamp(mix(FresnelAlphaMin,FresnelAlphaMax,fresnel),0.0,1.0);
    METALLIC=EnableMetallicMap?sample_MetallicMap(uv).r:Metallic;
    ROUGHNESS=EnableRoughnessMap?sample_RoughnessMap(uv).r:Roughness;
    AO=EnableAOMap?sample_AOMap(uv).r:1.0;
    EMISSION=EmissionColor.rgb*mix(EmissionIntensity,EmissionIntensityLayer1,weight);
    if(EnableEmissionColorMap) EMISSION*=sample_EmissionColorMap(uv).rgb;
    if(sampler_present[16]) EMISSION+=texture(LocalSpecularProbe,reflect(-VIEW,NORMAL)).rgb*LocalSpecularProbeIntensity*METALLIC;
}

#endif
#ifdef VERTEX_STAGE
#include "Common/Vertex.glsl"
#endif
#ifdef FRAGMENT_STAGE
#include "Common/Fragment.glsl"
#endif
