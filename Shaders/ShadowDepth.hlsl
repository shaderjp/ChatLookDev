#include "ChatLookDevShaderABI.hlsli"

struct ShadowPixelInput
{
    float4 position : SV_Position;
    float2 texcoord : TEXCOORD0;
};

ShadowPixelInput VSMain(RBVertexInput input)
{
    ShadowPixelInput output;
    const float4 worldPosition = mul(float4(input.position, 1.0), gModel);
    output.position = mul(worldPosition, gShadowViewProjection);
    output.texcoord = input.texcoord;
    return output;
}

void PSMain(ShadowPixelInput input)
{
    // The pixel shader exists only so alpha-mask materials can discard before
    // fixed-function depth writes. Opaque materials simply fall through.
    if ((uint)round(gAlphaMode) == 1u)
    {
        float alpha = gBaseColorFactor.a;
        if ((gMaterialTextureMask & RB_TEXTURE_BASE_COLOR) != 0)
        {
            alpha *= gBaseColorTexture.Sample(gLinearWrapSampler, input.texcoord).a;
        }
        if (alpha < gAlphaCutoff)
        {
            discard;
        }
    }
}
