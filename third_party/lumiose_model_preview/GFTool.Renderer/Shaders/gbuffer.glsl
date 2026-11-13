#version 420 core

#ifdef VERTEX_STAGE
layout (location = 0) in vec3 inPosition;
layout (location = 1) in vec2 inTexCoord;

layout (location = 0) out vec2 outTexCoord;

void main() 
{
    outTexCoord = inTexCoord;
    gl_Position = vec4(inPosition, 1.0);
}
#endif

#ifdef FRAGMENT_STAGE
layout (location = 0) in vec2 inTexCoord;
layout (location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D albedoTexture;
layout(binding = 1) uniform sampler2D normalTexture;
layout(binding = 2) uniform sampler2D specularTexture;
layout(binding = 3) uniform sampler2D aoTexture;
layout(binding = 4) uniform sampler2D ssaoTexture;

uniform vec3 LightDirection;
uniform vec3 LightColor;
uniform vec3 AmbientColor;

uniform bool useAlbedo;
uniform bool useNormal;
uniform bool useSpecular;
uniform bool useAO;
uniform bool useSSAO;
uniform bool useToon;
uniform bool useLegacy;

void main()
{
    vec4 albedoSample = texture(albedoTexture, inTexCoord);
    vec3 albedo = albedoSample.rgb;
    outColor = vec4(0.0, 0.0, 0.0, albedoSample.a);
    vec3 normal = texture(normalTexture, inTexCoord).rgb;
    vec3 specular = texture(specularTexture, inTexCoord).rgb;
    float ao = texture(aoTexture, inTexCoord).r;
    float ssao = useSSAO ? texture(ssaoTexture, inTexCoord).r : 1.0;

    bool useAll = useAlbedo && useNormal && useSpecular && useAO;

    if(useLegacy)
    {
        float aoSoft = mix(1.0, ao, 0.7);
        outColor.rgb = albedo * aoSoft;
        outColor.rgb += specular * 0.35;
    }
    else if(useToon)
    {
        vec3 n = normalize(normal * 2.0 - 1.0);
        float nDotL = max(dot(n, normalize(-LightDirection)), 0.0);
        float steps = 3.0;
        float toon = floor(nDotL * steps) / (steps - 1.0);
        vec3 lit = AmbientColor + LightColor * toon;
        float aoSoft = mix(1.0, ao, 0.75);
        float ssaoSoft = mix(1.0, ssao, 0.8);
        outColor.rgb = albedo * lit * aoSoft * ssaoSoft;
    }
    else if(useAll)
    {

        float aoSoft = mix(1.0, ao, 0.65);
        float ssaoSoft = mix(1.0, ssao, 0.7);
        outColor.rgb = albedo * aoSoft * ssaoSoft;
        outColor.rgb += specular * 0.25;
    }
    else
    {
        if(useAlbedo)
            outColor.rgb = albedo;
        if(useNormal)
            outColor.rgb = normal;
        if(useSpecular)
            outColor.rgb = specular;
        if(useAO)
            outColor.rgb *= ao;
    }
    outColor.rgb *= outColor.a; // final target uses premultiplied coverage

}
#endif
