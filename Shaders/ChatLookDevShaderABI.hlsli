#ifndef RENDERBUILDER_SHADER_ABI_HLSLI
#define RENDERBUILDER_SHADER_ABI_HLSLI

cbuffer RenderBuilderScene : register(b0)
{
    float4x4 gModelViewProjection;
    float4x4 gModel;
    float4x4 gViewProjectionInverse;
    float4 gCameraPositionTime;
    float4 gLightDirectionIntensity;
    float4x4 gShadowViewProjection;
};

cbuffer RenderBuilderMaterial : register(b1)
{
    float4 gBaseColorFactor;
    uint gMaterialTextureMask;
    float gNormalStrength;
    float gNormalGreenScale;
    float gRoughnessFactor;
    float gMetallicFactor;
    float gOcclusionStrength;
    float gAlphaCutoff;
    float gAlphaMode;
    float4 gEmissiveFactor;
    float gPackedOcclusionRoughnessMetallic;
};

cbuffer RenderBuilderLookDev : register(b2)
{
    float4 gSunColorIntensity;
    float4 gEnvironmentOptions;
    float4 gViewOptions;
    float4 gIblOptions;
    float4 gSkyTopColor;
    float4 gSkyHorizonColor;
    float4 gShadowOptions;
};

static const uint RB_TEXTURE_BASE_COLOR = 1u << 0;
static const uint RB_TEXTURE_NORMAL = 1u << 1;
static const uint RB_TEXTURE_ROUGHNESS = 1u << 2;
static const uint RB_TEXTURE_METALLIC = 1u << 3;
static const uint RB_TEXTURE_OCCLUSION = 1u << 4;
static const uint RB_TEXTURE_EMISSIVE = 1u << 5;

static const uint RB_BACKGROUND_SKY_COLOR = 0u;
static const uint RB_BACKGROUND_HDRI = 1u;
static const uint RB_BACKGROUND_CHECKER = 2u;

static const uint RB_TONEMAP_NONE = 0u;
static const uint RB_TONEMAP_REINHARD = 1u;
static const uint RB_TONEMAP_ACES = 2u;

static const uint RB_DISPLAY_BEAUTY = 0u;
static const uint RB_DISPLAY_BASE_COLOR = 1u;
static const uint RB_DISPLAY_NORMAL = 2u;
static const uint RB_DISPLAY_ROUGHNESS = 3u;
static const uint RB_DISPLAY_METALLIC = 4u;
static const uint RB_DISPLAY_AO = 5u;
static const uint RB_DISPLAY_EMISSIVE = 6u;
static const uint RB_DISPLAY_LIGHTING_ONLY = 7u;
static const uint RB_DISPLAY_SHADOW_MASK = 8u;

Texture2D gBaseColorTexture : register(t0);
Texture2D gNormalTexture : register(t1);
Texture2D gRoughnessTexture : register(t2);
Texture2D gMetallicTexture : register(t3);
Texture2D gOcclusionTexture : register(t4);
Texture2D gEmissiveTexture : register(t5);
Texture2D gEnvironmentTexture : register(t6);
Texture2D gIrradianceTexture : register(t7);
Texture2D gPrefilterTexture : register(t8);
Texture2D gBrdfLutTexture : register(t9);
SamplerState gLinearWrapSampler : register(s0);
SamplerState gLinearClampSampler : register(s1);

struct RBVertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 texcoord : TEXCOORD0;
    float4 tangent : TANGENT;
};

struct RBPixelInput
{
    float4 position : SV_Position;
    float3 worldNormal : NORMAL;
    float2 texcoord : TEXCOORD0;
    float3 worldPosition : TEXCOORD1;
    float3 worldTangent : TANGENT;
    float tangentSign : TEXCOORD2;
};

#endif
