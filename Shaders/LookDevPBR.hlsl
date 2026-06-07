#include "ChatLookDevShaderABI.hlsli"

static const float PI = 3.14159265359;

RBPixelInput VSMain(RBVertexInput input)
{
    RBPixelInput output;
    const float4 worldPosition = mul(float4(input.position, 1.0), gModel);
    output.position = mul(float4(input.position, 1.0), gModelViewProjection);
    output.worldNormal = normalize(mul(float4(input.normal, 0.0), gModel).xyz);
    output.worldTangent = normalize(mul(float4(input.tangent.xyz, 0.0), gModel).xyz);
    output.tangentSign = input.tangent.w;
    output.texcoord = input.texcoord;
    output.worldPosition = worldPosition.xyz;
    return output;
}

float3 ApplyEnvironmentRotation(float3 direction)
{
    const float c = cos(gEnvironmentOptions.x);
    const float s = sin(gEnvironmentOptions.x);
    return float3(c * direction.x - s * direction.z, direction.y, s * direction.x + c * direction.z);
}

float2 DirectionToEquirectUv(float3 direction)
{
    direction = ApplyEnvironmentRotation(normalize(direction));
    return float2(atan2(direction.x, direction.z) * 0.159154943 + 0.5, acos(clamp(direction.y, -1.0, 1.0)) * 0.318309886);
}

float3 SampleDiffuseIbl(float3 normal)
{
    if (gEnvironmentOptions.w <= 0.5)
    {
        const float t = saturate(normal.y * 0.5 + 0.5);
        return lerp(gSkyHorizonColor.rgb, gSkyTopColor.rgb, t) * gEnvironmentOptions.y;
    }
    return gIrradianceTexture.SampleLevel(gLinearWrapSampler, DirectionToEquirectUv(normal), 0.0).rgb * gEnvironmentOptions.y;
}

float3 SampleSpecularIbl(float3 reflectionDirection, float roughness)
{
    if (gEnvironmentOptions.w <= 0.5)
    {
        const float t = saturate(reflectionDirection.y * 0.5 + 0.5);
        return lerp(gSkyHorizonColor.rgb, gSkyTopColor.rgb, t) * gEnvironmentOptions.y;
    }
    return gPrefilterTexture.SampleLevel(gLinearWrapSampler, DirectionToEquirectUv(reflectionDirection), roughness * gIblOptions.x).rgb * gEnvironmentOptions.y;
}

float3 ApplyToneMapping(float3 color)
{
    color *= exp2(gViewOptions.x);
    const uint toneMapper = (uint)round(gViewOptions.z);
    if (toneMapper == RB_TONEMAP_REINHARD)
    {
        color = color / (1.0 + color);
    }
    else if (toneMapper == RB_TONEMAP_ACES)
    {
        color = saturate((color * (2.51 * color + 0.03)) / (color * (2.43 * color + 0.59) + 0.14));
    }
    return pow(max(color, 0.0), 1.0 / max(gViewOptions.y, 0.01));
}

float DistributionGGX(float nDotH, float roughness)
{
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float denom = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * denom * denom, 1.0e-5);
}

float GeometrySchlickGGX(float nDotV, float roughness)
{
    const float r = roughness + 1.0;
    const float k = (r * r) * 0.125;
    return nDotV / max(nDotV * (1.0 - k) + k, 1.0e-5);
}

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(saturate(1.0 - cosTheta), 5.0);
}

float4 PSMain(RBPixelInput input) : SV_Target0
{
    const float3 vertexNormal = normalize(input.worldNormal);
    const float3 tangentCandidate = input.worldTangent - vertexNormal * dot(vertexNormal, input.worldTangent);
    const float3 tangent = tangentCandidate * rsqrt(max(dot(tangentCandidate, tangentCandidate), 1.0e-6));
    const float3 bitangentCandidate = cross(vertexNormal, tangent) * input.tangentSign;
    const float3 bitangent = bitangentCandidate * rsqrt(max(dot(bitangentCandidate, bitangentCandidate), 1.0e-6));

    float3 sampledNormal = gNormalTexture.Sample(gLinearWrapSampler, input.texcoord).xyz * 2.0 - 1.0;
    sampledNormal.xy *= float2(gNormalStrength, gNormalStrength * gNormalGreenScale);
    sampledNormal = normalize(sampledNormal);
    const bool hasNormal = (gMaterialTextureMask & RB_TEXTURE_NORMAL) != 0;
    const float3 normal = hasNormal ? normalize(sampledNormal.x * tangent + sampledNormal.y * bitangent + sampledNormal.z * vertexNormal) : vertexNormal;
    const float3 viewDirection = normalize(gCameraPositionTime.xyz - input.worldPosition);
    const float3 lightDirection = normalize(-gLightDirectionIntensity.xyz);
    const float3 halfVector = normalize(viewDirection + lightDirection);

    const float4 baseTexture = ((gMaterialTextureMask & RB_TEXTURE_BASE_COLOR) != 0)
        ? gBaseColorTexture.Sample(gLinearWrapSampler, input.texcoord)
        : float4(0.75, 0.72, 0.68, 1.0);
    const float3 baseColor = saturate(baseTexture.rgb * gBaseColorFactor.rgb);
    const float alpha = baseTexture.a * gBaseColorFactor.a;
    const bool hasPackedOrm = gPackedOcclusionRoughnessMetallic > 0.5 && ((gMaterialTextureMask & RB_TEXTURE_ROUGHNESS) != 0);
    const float3 packedOrmSample = hasPackedOrm ? gRoughnessTexture.Sample(gLinearWrapSampler, input.texcoord).rgb : float3(1.0, 1.0, 1.0);
    const float roughnessSample = hasPackedOrm
        ? packedOrmSample.g
        : (((gMaterialTextureMask & RB_TEXTURE_ROUGHNESS) != 0) ? gRoughnessTexture.Sample(gLinearWrapSampler, input.texcoord).r : 1.0);
    const float metallicSample = hasPackedOrm
        ? packedOrmSample.b
        : (((gMaterialTextureMask & RB_TEXTURE_METALLIC) != 0) ? gMetallicTexture.Sample(gLinearWrapSampler, input.texcoord).r : 1.0);
    const float aoSample = hasPackedOrm
        ? packedOrmSample.r
        : (((gMaterialTextureMask & RB_TEXTURE_OCCLUSION) != 0) ? gOcclusionTexture.Sample(gLinearWrapSampler, input.texcoord).r : 1.0);
    const float3 emissiveSample = ((gMaterialTextureMask & RB_TEXTURE_EMISSIVE) != 0) ? gEmissiveTexture.Sample(gLinearWrapSampler, input.texcoord).rgb : float3(1.0, 1.0, 1.0);
    const float roughness = saturate(max(roughnessSample * gRoughnessFactor, 0.045));
    const float metallic = saturate(metallicSample * gMetallicFactor);
    const float occlusion = lerp(1.0, aoSample, saturate(gOcclusionStrength));
    const float3 emissive = emissiveSample * gEmissiveFactor.rgb * gEmissiveFactor.a;

    if ((uint)round(gAlphaMode) == 1u && alpha < gAlphaCutoff)
    {
        discard;
    }

    const float nDotL = saturate(dot(normal, lightDirection));
    const float nDotV = saturate(dot(normal, viewDirection));
    const float nDotH = saturate(dot(normal, halfVector));
    const float hDotV = saturate(dot(halfVector, viewDirection));
    const float3 f0 = lerp(float3(0.04, 0.04, 0.04), baseColor, metallic);
    const float3 fresnel = FresnelSchlick(hDotV, f0);
    const float distribution = DistributionGGX(nDotH, roughness);
    const float geometry = GeometrySchlickGGX(nDotV, roughness) * GeometrySchlickGGX(nDotL, roughness);
    const float3 specularBrdf = distribution * geometry * fresnel / max(4.0 * nDotV * nDotL, 1.0e-5);
    const float3 diffuseBrdf = (1.0 - fresnel) * (1.0 - metallic) * baseColor / PI;
    const float3 sunRadiance = gSunColorIntensity.rgb * gSunColorIntensity.a;
    const float3 sunLighting = (diffuseBrdf + specularBrdf) * sunRadiance * nDotL;

    const float3 reflectionDirection = reflect(-viewDirection, normal);
    const float3 diffuseIbl = SampleDiffuseIbl(normal) * baseColor * (1.0 - metallic);
    const float3 brdf = gBrdfLutTexture.SampleLevel(gLinearClampSampler, float2(nDotV, roughness), 0.0).rgb;
    const float3 specularIbl = SampleSpecularIbl(reflectionDirection, roughness) * (FresnelSchlick(nDotV, f0) * brdf.x + brdf.y);
    const float3 ambient = (diffuseIbl + specularIbl) * occlusion * gIblOptions.w;
    const float3 beauty = ambient + sunLighting + emissive;

    const uint displayMode = (uint)round(gViewOptions.w);
    if (displayMode == RB_DISPLAY_BASE_COLOR) { return float4(ApplyToneMapping(baseColor), alpha); }
    if (displayMode == RB_DISPLAY_NORMAL) { return float4(normal * 0.5 + 0.5, 1.0); }
    if (displayMode == RB_DISPLAY_ROUGHNESS) { return float4(roughness.xxx, 1.0); }
    if (displayMode == RB_DISPLAY_METALLIC) { return float4(metallic.xxx, 1.0); }
    if (displayMode == RB_DISPLAY_AO) { return float4(occlusion.xxx, 1.0); }
    if (displayMode == RB_DISPLAY_EMISSIVE) { return float4(ApplyToneMapping(emissive), 1.0); }
    if (displayMode == RB_DISPLAY_LIGHTING_ONLY) { return float4(ApplyToneMapping(ambient + sunLighting), 1.0); }

    return float4(ApplyToneMapping(beauty), alpha);
}
