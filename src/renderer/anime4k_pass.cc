#include "anime4k_pass.h"
#include "../diagnostics.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>

namespace fallout {
namespace renderer {

const char* ANIME4K_SHADER_SOURCE = R"(
// Anime4K v3.2 Upscale Original x2 (Ported to HLSL)

#define REFINE_STRENGTH 0.5
#define REFINE_BIAS 0.0

// Polynomial coefficients
#define P5 ( 11.68129591)
#define P4 (-42.46906057)
#define P3 ( 60.28286266)
#define P2 (-41.84451327)
#define P1 ( 14.05517353)
#define P0 (-1.081521930)

cbuffer UpscaleParams : register(b0)
{
    uint2 inputSize;    // Source resolution (640, 480)
    uint2 outputSize;   // Target resolution (2560, 1440)
    uint2 effectiveSize; // Scaled resolution (e.g. 1920, 1440)
    uint2 offset;        // Letterbox offset (e.g. 320, 0)
    float2 rcpInput;    // 1.0 / inputSize
    float2 rcpEffectiveOutput; // 1.0 / effectiveSize
    float strength;     // Enhancement strength (0.0-1.0)
    float3 padding;
};

Texture2D<float4> InputTexture : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);
SamplerState LinearSampler : register(s0);

float get_luma(float4 c)
{
    return dot(c.rgb, float3(0.299, 0.587, 0.114));
}

float power_function(float x)
{
    float x2 = x * x;
    float x3 = x2 * x;
    float x4 = x2 * x2;
    float x5 = x2 * x3;
    return P5 * x5 + P4 * x4 + P3 * x3 + P2 * x2 + P1 * x + P0;
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= outputSize.x || DTid.y >= outputSize.y)
        return;

    // Letterboxing check
    if (DTid.x < offset.x || DTid.x >= offset.x + effectiveSize.x ||
        DTid.y < offset.y || DTid.y >= offset.y + effectiveSize.y)
    {
        OutputTexture[DTid.xy] = float4(0, 0, 0, 1); // Black bars
        return;
    }

    // Map to UV space of the effective area
    float2 pixelPos = float2(DTid.xy) - float2(offset);
    float2 uv = (pixelPos + 0.5f) * rcpEffectiveOutput;
    
    float2 d = rcpEffectiveOutput; // Use effective pixel size for sampling offsets

    // Sample center pixel (bilinear interpolation from input)
    float4 cc = InputTexture.SampleLevel(LinearSampler, uv, 0);

    // Calculate Luma of neighbors
    // We sample at offsets corresponding to the OUTPUT pixel size
    float t = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(0, -d.y), 0));
    float b = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(0, d.y), 0));
    float l = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(-d.x, 0), 0));
    float r = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(d.x, 0), 0));
    
    float tl = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(-d.x, -d.y), 0));
    float tr = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(d.x, -d.y), 0));
    float bl = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(-d.x, d.y), 0));
    float br = get_luma(InputTexture.SampleLevel(LinearSampler, uv + float2(d.x, d.y), 0));
    
    // Sobel Gradients
    float gx = (tr + 2.0 * r + br) - (tl + 2.0 * l + bl);
    float gy = (bl + 2.0 * b + br) - (tl + 2.0 * t + tr);

    // Gradient Magnitude (Normalized by 4.0 to keep in 0-1 range for power function)
    float sobel_norm = sqrt(gx * gx + gy * gy) / 4.0;
    
    // Refinement Strength
    float dval = power_function(saturate(sobel_norm));
    dval = saturate(dval * REFINE_STRENGTH + REFINE_BIAS);

    // Determine edge direction
    float xpos = (gx > 0.0) ? 1.0 : -1.0;
    float ypos = (gy > 0.0) ? 1.0 : -1.0;
    
    // Sample pixels along the gradient direction
    float4 xval = InputTexture.SampleLevel(LinearSampler, uv + float2(d.x * xpos, 0), 0);
    float4 yval = InputTexture.SampleLevel(LinearSampler, uv + float2(0, d.y * ypos), 0);
    
    // Interpolate between xval and yval based on gradient ratio
    float abs_gx = abs(gx);
    float abs_gy = abs(gy);
    float sum_g = abs_gx + abs_gy;
    
    float4 grad_sample;
    if (sum_g < 0.001) {
        grad_sample = cc;
    } else {
        grad_sample = (xval * abs_gx + yval * abs_gy) / sum_g;
    }
    
    // Blend original and gradient sample based on strength
    OutputTexture[DTid.xy] = lerp(cc, grad_sample, dval * strength);
}
)";

Anime4kPass::Anime4kPass() {}
Anime4kPass::~Anime4kPass() { Shutdown(); }

bool Anime4kPass::Init(D3D12Context& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) {
    mInputWidth = inputWidth;
    mInputHeight = inputHeight;
    mOutputWidth = outputWidth;
    mOutputHeight = outputHeight;
    
    auto device = context.GetDevice();

    // 1. Create Root Signature
    {
        D3D12_ROOT_PARAMETER params[3];
        
        // Param 0: Constants
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.RegisterSpace = 0;
        params[0].Constants.Num32BitValues = 16; // 4 * uint2 + 2 * float2 + 1 * float + 3 * padding = 8 + 4 + 1 + 3 = 16 floats/uints
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Param 1: Input Texture (SRV)
        D3D12_DESCRIPTOR_RANGE srvRange = {};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 1;
        srvRange.BaseShaderRegister = 0;
        srvRange.RegisterSpace = 0;
        srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &srvRange;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Param 2: Output Texture (UAV)
        D3D12_DESCRIPTOR_RANGE uavRange = {};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 1;
        uavRange.BaseShaderRegister = 0;
        uavRange.RegisterSpace = 0;
        uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges = &uavRange;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Static Sampler
        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MipLODBias = 0;
        sampler.MaxAnisotropy = 0;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        sampler.MinLOD = 0.0f;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.RegisterSpace = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
        rootDesc.NumParameters = 3;
        rootDesc.pParameters = params;
        rootDesc.NumStaticSamplers = 1;
        rootDesc.pStaticSamplers = &sampler;
        rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3DBlob> signature;
        Microsoft::WRL::ComPtr<ID3DBlob> error;
        if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error))) {
            if (error) diagnosticsLog(DiagnosticsLevel::Info, "Anime4kPass", "Root Signature Error: %s", (char*)error->GetBufferPointer());
            return false;
        }
        if (FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&mRootSignature)))) return false;
    }

    // 2. Compile Shader
    Microsoft::WRL::ComPtr<ID3DBlob> computeShader;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
    UINT compileFlags = 0; // D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    
    HRESULT hr = D3DCompile(ANIME4K_SHADER_SOURCE, strlen(ANIME4K_SHADER_SOURCE), "Anime4K", nullptr, nullptr, "main", "cs_5_0", compileFlags, 0, &computeShader, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) diagnosticsLog(DiagnosticsLevel::Info, "Anime4kPass", "Shader Compile Error: %s", (char*)errorBlob->GetBufferPointer());
        return false;
    }

    // 3. Create PSO
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = mRootSignature.Get();
    psoDesc.CS = { computeShader->GetBufferPointer(), computeShader->GetBufferSize() };
    
    if (FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mPipelineState)))) return false;

    // 4. Create Descriptor Heap
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = 2; // 1 SRV + 1 UAV
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mDescriptorHeap)))) return false;

    return true;
}

void Anime4kPass::Execute(D3D12Context& context, ID3D12Resource* input, ID3D12Resource* output) {
    auto cmdList = context.GetCommandList();
    auto device = context.GetDevice();

    // 1. Update Descriptors
    D3D12_CPU_DESCRIPTOR_HANDLE handle = mDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // SRV (Input)
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(input, &srvDesc, handle);

    // UAV (Output)
    handle.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(output, nullptr, &uavDesc, handle);

    // 2. Calculate Constants
    float scaleX = (float)mOutputWidth / mInputWidth;
    float scaleY = (float)mOutputHeight / mInputHeight;
    float scale = std::min(scaleX, scaleY);
    
    int effectiveW = (int)(mInputWidth * scale);
    int effectiveH = (int)(mInputHeight * scale);
    
    int offsetX = (mOutputWidth - effectiveW) / 2;
    int offsetY = (mOutputHeight - effectiveH) / 2;

    struct {
        uint32_t inputSize[2];
        uint32_t outputSize[2];
        uint32_t effectiveSize[2];
        uint32_t offset[2];
        float rcpInput[2];
        float rcpEffectiveOutput[2];
        float strength;
        float padding[3];
    } constants;

    constants.inputSize[0] = mInputWidth;
    constants.inputSize[1] = mInputHeight;
    constants.outputSize[0] = mOutputWidth;
    constants.outputSize[1] = mOutputHeight;
    constants.effectiveSize[0] = effectiveW;
    constants.effectiveSize[1] = effectiveH;
    constants.offset[0] = offsetX;
    constants.offset[1] = offsetY;
    constants.rcpInput[0] = 1.0f / mInputWidth;
    constants.rcpInput[1] = 1.0f / mInputHeight;
    constants.rcpEffectiveOutput[0] = 1.0f / effectiveW;
    constants.rcpEffectiveOutput[1] = 1.0f / effectiveH;
    constants.strength = mStrength;
    constants.padding[0] = 0; constants.padding[1] = 0; constants.padding[2] = 0;

    // 3. Dispatch
    cmdList->SetPipelineState(mPipelineState.Get());
    cmdList->SetComputeRootSignature(mRootSignature.Get());
    
    ID3D12DescriptorHeap* heaps[] = { mDescriptorHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    
    cmdList->SetComputeRoot32BitConstants(0, 16, &constants, 0);
    cmdList->SetComputeRootDescriptorTable(1, mDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    
    D3D12_GPU_DESCRIPTOR_HANDLE uavHandle = mDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
    uavHandle.ptr += increment;
    cmdList->SetComputeRootDescriptorTable(2, uavHandle);

    cmdList->Dispatch((mOutputWidth + 7) / 8, (mOutputHeight + 7) / 8, 1);
}

void Anime4kPass::Shutdown() {
    mRootSignature.Reset();
    mPipelineState.Reset();
    mDescriptorHeap.Reset();
}

} // namespace renderer
} // namespace fallout
