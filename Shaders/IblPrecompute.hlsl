cbuffer IblConstants : register(b0)
{
    uint gMode;
    uint gWidth;
    uint gHeight;
    uint gMipLevel;
    float gRoughness;
    float gSourceMipCount;
    float2 gPadding;
};

Texture2D<float4> gSourceEnvironment : register(t0);
RWTexture2D<float4> gOutputTexture : register(u0);
SamplerState gLinearWrapSampler : register(s0);

static const float PI = 3.14159265359;
static const uint SAMPLE_COUNT = 64;

float RadicalInverseVdc(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;
}

float2 Hammersley(uint i, uint n)
{
    return float2(float(i) / float(n), RadicalInverseVdc(i));
}

float3 EquirectUvToDirection(float2 uv)
{
    const float phi = (uv.x - 0.5) * 2.0 * PI;
    const float theta = uv.y * PI;
    return float3(sin(phi) * sin(theta), cos(theta), cos(phi) * sin(theta));
}

float2 DirectionToEquirectUv(float3 direction)
{
    direction = normalize(direction);
    return float2(atan2(direction.x, direction.z) * 0.159154943 + 0.5, acos(clamp(direction.y, -1.0, 1.0)) * 0.318309886);
}

void BuildBasis(float3 normal, out float3 tangent, out float3 bitangent)
{
    const float3 up = abs(normal.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    tangent = normalize(cross(up, normal));
    bitangent = cross(normal, tangent);
}

float3 CosineSampleHemisphere(float2 xi)
{
    const float phi = 2.0 * PI * xi.x;
    const float cosTheta = sqrt(saturate(1.0 - xi.y));
    const float sinTheta = sqrt(saturate(xi.y));
    return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

float3 ImportanceSampleGGX(float2 xi, float roughness, float3 normal)
{
    const float a = roughness * roughness;
    const float phi = 2.0 * PI * xi.x;
    const float cosTheta = sqrt(saturate((1.0 - xi.y) / max(1.0 + (a * a - 1.0) * xi.y, 1.0e-5)));
    const float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));
    const float3 h = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    float3 tangent;
    float3 bitangent;
    BuildBasis(normal, tangent, bitangent);
    return normalize(tangent * h.x + bitangent * h.y + normal * h.z);
}

float GeometrySchlickGGX(float nDotV, float roughness)
{
    const float a = roughness;
    const float k = (a * a) * 0.5;
    return nDotV / max(nDotV * (1.0 - k) + k, 1.0e-5);
}

float GeometrySmith(float nDotV, float nDotL, float roughness)
{
    return GeometrySchlickGGX(nDotV, roughness) * GeometrySchlickGGX(nDotL, roughness);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= gWidth || dispatchThreadId.y >= gHeight)
    {
        return;
    }

    const float2 uv = (float2(dispatchThreadId.xy) + 0.5) / float2(gWidth, gHeight);
    if (gMode == 2)
    {
        const float nDotV = saturate(uv.x);
        const float roughness = saturate(uv.y);
        const float3 view = float3(sqrt(1.0 - nDotV * nDotV), 0.0, nDotV);
        float a = 0.0;
        float b = 0.0;

        [loop]
        for (uint i = 0; i < SAMPLE_COUNT; ++i)
        {
            const float2 xi = Hammersley(i, SAMPLE_COUNT);
            const float3 h = ImportanceSampleGGX(xi, roughness, float3(0.0, 0.0, 1.0));
            const float3 l = normalize(2.0 * dot(view, h) * h - view);
            const float nDotL = saturate(l.z);
            const float nDotH = saturate(h.z);
            const float vDotH = saturate(dot(view, h));
            if (nDotL > 0.0)
            {
                const float g = GeometrySmith(nDotV, nDotL, roughness);
                const float gVis = (g * vDotH) / max(nDotH * nDotV, 1.0e-5);
                const float fc = pow(1.0 - vDotH, 5.0);
                a += (1.0 - fc) * gVis;
                b += fc * gVis;
            }
        }
        gOutputTexture[dispatchThreadId.xy] = float4(a / SAMPLE_COUNT, b / SAMPLE_COUNT, 0.0, 1.0);
        return;
    }

    const float3 normal = EquirectUvToDirection(uv);
    float3 tangent;
    float3 bitangent;
    BuildBasis(normal, tangent, bitangent);

    float3 color = 0.0;
    float totalWeight = 0.0;
    [loop]
    for (uint i = 0; i < SAMPLE_COUNT; ++i)
    {
        const float2 xi = Hammersley(i, SAMPLE_COUNT);
        float3 sampleDirection;
        float weight;
        float sourceMip = 0.0;
        if (gMode == 0)
        {
            const float3 hemi = CosineSampleHemisphere(xi);
            sampleDirection = normalize(tangent * hemi.x + bitangent * hemi.y + normal * hemi.z);
            weight = saturate(dot(normal, sampleDirection));
            sourceMip = max(gSourceMipCount - 3.0, 0.0);
        }
        else
        {
            const float3 halfVector = ImportanceSampleGGX(xi, gRoughness, normal);
            sampleDirection = normalize(2.0 * dot(normal, halfVector) * halfVector - normal);
            weight = saturate(dot(normal, sampleDirection));
            sourceMip = gRoughness * gRoughness * max(gSourceMipCount - 1.0, 0.0);
        }
        color += gSourceEnvironment.SampleLevel(gLinearWrapSampler, DirectionToEquirectUv(sampleDirection), sourceMip).rgb * weight;
        totalWeight += weight;
    }
    gOutputTexture[dispatchThreadId.xy] = float4(color / max(totalWeight, 1.0e-5), 1.0);
}
