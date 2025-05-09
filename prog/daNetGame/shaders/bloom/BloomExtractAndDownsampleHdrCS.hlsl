//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
// Developed by Minigraph
//
// Author:  James Stanard 
//
// The CS for extracting bright pixels and downsampling them to an unblurred bloom buffer.

#include "ShaderUtility.hlsli"
//#include "PostEffectsRS.hlsli"

RWTexture2D<float3> BloomResult : register( u0 );
RWTexture2D<float> LumaResult : register( u1 );

//cbuffer cb0
//{
//  float2 g_inverseOutputSize;
//  float g_bloomThreshold;
//}
float RGBToAverage(float3 x) {return dot(x, 1./3.);}

//[RootSignature(PostEffects_RootSig)]
[numthreads( 8, 8, 1 )]
void main( uint3 DTid : SV_DispatchThreadID )
{
    // We need the scale factor and the size of one pixel so that our four samples are right in the middle
    // of the quadrant they are covering.
    float2 uv = (DTid.xy + 0.5) * g_inverseOutputSize;
    float2 offset = g_inverseOutputSize * 0.25;

    // Use 4 bilinear samples to guarantee we don't undersample when downsizing by more than 2x
    float3 color1 = -min(0, -SourceTex.SampleLevel( BiLinearClamp, uv + float2(-offset.x, -offset.y), 0 ).xyz);
    float3 color2 = -min(0, -SourceTex.SampleLevel( BiLinearClamp, uv + float2( offset.x, -offset.y), 0 ).xyz);
    float3 color3 = -min(0, -SourceTex.SampleLevel( BiLinearClamp, uv + float2(-offset.x,  offset.y), 0 ).xyz);
    float3 color4 = -min(0, -SourceTex.SampleLevel( BiLinearClamp, uv + float2( offset.x,  offset.y), 0 ).xyz);

    float luma1 = RGBToLuminance(color1);
    float luma2 = RGBToLuminance(color2);
    float luma3 = RGBToLuminance(color3);
    float luma4 = RGBToLuminance(color4);

    const float kSmallEpsilon = 0.0001;

    float ScaledThreshold = g_bloomThreshold * Exposure[1];   // BloomThreshold / Exposure
    //float ScaledThreshold = g_bloomThreshold;

    // We perform a brightness filter pass, where lone bright pixels will contribute less.
    //color1 *= max(kSmallEpsilon, luma1 - ScaledThreshold) / (luma1 + kSmallEpsilon);
    //color2 *= max(kSmallEpsilon, luma2 - ScaledThreshold) / (luma2 + kSmallEpsilon);
    //color3 *= max(kSmallEpsilon, luma3 - ScaledThreshold) / (luma3 + kSmallEpsilon);
    //color4 *= max(kSmallEpsilon, luma4 - ScaledThreshold) / (luma4 + kSmallEpsilon);

    color1 = max(0, color1 - ScaledThreshold);
    color2 = max(0, color2 - ScaledThreshold);
    color3 = max(0, color3 - ScaledThreshold);
    color4 = max(0, color4 - ScaledThreshold);
    //color1 *= exp2(Exposure[3]+bloomEC-3); //moving frostbite to pbr - additional bloom at all exposures

    // The shimmer filter helps remove stray bright pixels from the bloom buffer by inversely weighting
    // them by their luminance.  The overall effect is to shrink bright pixel regions around the border.
    // Lone pixels are likely to dissolve completely.  This effect can be tuned by adjusting the shimmer
    // filter inverse strength.  The bigger it is, the less a pixel's luminance will matter.
    const float kShimmerFilterInverseStrength = 10.0f;
    float weight1 = 1.0f / (luma1 + kShimmerFilterInverseStrength);
    float weight2 = 1.0f / (luma2 + kShimmerFilterInverseStrength);
    float weight3 = 1.0f / (luma3 + kShimmerFilterInverseStrength);
    float weight4 = 1.0f / (luma4 + kShimmerFilterInverseStrength);
    float weightSum = weight1 + weight2 + weight3 + weight4;

    BloomResult[DTid.xy] = (color1 * weight1 + color2 * weight2 + color3 * weight3 + color4 * weight4) / weightSum;

    #if WRITE_LOG_LUM
    float luma = (luma1 + luma2 + luma3 + luma4) * 0.25;

    // Prevent log(0) and put only pure black pixels in Histogram[0]
    float result; // store result in here, compiler may crash on else branch on write to LumaResult
    if (luma == 0.0)
    {
        //LumaResult[DTid.xy] = 0;
        result = 0;
    }
    else
    {
        const float MinLog = Exposure[4];
        const float RcpLogRange = Exposure[7];
        //const float MinLog = -12;
        //const float RcpLogRange = 1./(4-MinLog);
        float logLuma = saturate((log2(luma) - MinLog) * RcpLogRange);  // Rescale to [0.0, 1.0]
        //LumaResult[DTid.xy] = logLuma * 254.0/255.0 + 1./255.0;                    // Rescale to [1, 255]
        //LumaResult[DTid.xy] = logLuma * 254.0/255.0 + 1.5/255.0;                    // Rescale to [1, 255]
        //LumaResult[DTid.xy] = luma * 254.0/255.0 + 1.5/255.0;
        result = logLuma * 254.0/255.0 + 1./255.0;
    }
    if ((DTid.x&3) == 1 && (DTid.y&3) == 1)
      LumaResult[DTid.xy>>2] = result;
    #endif
}
