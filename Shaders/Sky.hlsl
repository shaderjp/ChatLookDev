#include "ChatLookDevShaderABI.hlsli"

struct SkyOutput
{
    float4 position : SV_Position;
    float3 direction : TEXCOORD0;
};

SkyOutput VSMain(uint vertexId : SV_VertexID)
{
    const float2 positions[3] =
    {
        float2(-1.0, -1.0),
        float2(-1.0, 3.0),
        float2(3.0, -1.0),
    };
    SkyOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    const float4 worldNear = mul(float4(positions[vertexId], 1.0, 1.0), gViewProjectionInverse);
    output.direction = normalize(worldNear.xyz / max(worldNear.w, 1.0e-5) - gCameraPositionTime.xyz);
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

float4 PSMain(SkyOutput input) : SV_Target0
{
    float3 color;
    if ((uint)round(gEnvironmentOptions.z) == RB_BACKGROUND_HDRI && gEnvironmentOptions.w > 0.5)
    {
        color = gEnvironmentTexture.SampleLevel(gLinearWrapSampler, DirectionToEquirectUv(input.direction), 0.0).rgb * gEnvironmentOptions.y;
    }
    else if ((uint)round(gEnvironmentOptions.z) == RB_BACKGROUND_CHECKER)
    {
        const float2 uv = input.position.xy * 0.025;
        const float checker = (fmod(floor(uv.x) + floor(uv.y), 2.0) < 0.5) ? 0.18 : 0.42;
        color = checker.xxx;
    }
    else
    {
        const float t = saturate(input.direction.y * 0.5 + 0.5);
        color = lerp(gSkyHorizonColor.rgb, gSkyTopColor.rgb, t);
    }
    return float4(ApplyToneMapping(color), 1.0);
}
