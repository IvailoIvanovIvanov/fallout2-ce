#include "hdr_filter.h"
#include "../diagnostics.h"
#include <d3dcompiler.h>

namespace fallout {
namespace renderer {

const char* HDR_SHADER_SOURCE = R"(
cbuffer HdrParams : register(b0) {
    uint2 inputResolution;
    uint2 outputResolution;
    float saturation;
    float contrast;
    float blackCrushThreshold;
    float blackCrushStrength;
};

Texture2D<float4> InputTexture : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);

float3 rgb_to_hsv(float3 rgb) {
    float maxc = max(max(rgb.r, rgb.g), rgb.b);
    float minc = min(min(rgb.r, rgb.g), rgb.b);
    float v = maxc;
    if (minc == maxc) return float3(0, 0, v);
    float s = (maxc - minc) / maxc;
    float delta = maxc - minc;
    float h;
    if (maxc == rgb.r) h = fmod((rgb.g - rgb.b) / delta, 6.0);
    else if (maxc == rgb.g) h = (rgb.b - rgb.r) / delta + 2.0;
    else h = (rgb.r - rgb.g) / delta + 4.0;
    h /= 6.0;
    if (h < 0.0) h += 1.0;
    return float3(h, s, v);
}

float3 hsv_to_rgb(float3 hsv) {
    if (hsv.y == 0.0) return float3(hsv.z, hsv.z, hsv.z);
    float h = fmod(hsv.x * 6.0, 6.0);
    float c = hsv.z * hsv.y;
    float x = c * (1.0 - abs(fmod(h, 2.0) - 1.0));
    float m = hsv.z - c;
    float3 rgb;
    if (h < 1.0) rgb = float3(c, x, 0);
    else if (h < 2.0) rgb = float3(x, c, 0);
    else if (h < 3.0) rgb = float3(0, c, x);
    else if (h < 4.0) rgb = float3(0, x, c);
    else if (h < 5.0) rgb = float3(x, 0, c);
    else rgb = float3(c, 0, x);
    return rgb + m;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    if (dispatchThreadId.x >= outputResolution.x || dispatchThreadId.y >= outputResolution.y) return;

    // Nearest Neighbor Scaling
    int2 inputCoord;
    inputCoord.x = (dispatchThreadId.x * inputResolution.x) / outputResolution.x;
    inputCoord.y = (dispatchThreadId.y * inputResolution.y) / outputResolution.y;
    
    // Clamp
    inputCoord = clamp(inputCoord, int2(0, 0), int2(inputResolution.x - 1, inputResolution.y - 1));

    float4 color = InputTexture[inputCoord];
    
    // 1. Black Crush (Pre-processing)
    float luminance = dot(color.rgb, float3(0.299, 0.587, 0.114));
    if (luminance < blackCrushThreshold) {
        float factor = 1.0 - (blackCrushThreshold - luminance) / blackCrushThreshold * blackCrushStrength;
        color.rgb *= max(0.0, factor);
    }

    // 2. Saturation
    float3 hsv = rgb_to_hsv(color.rgb);
    hsv.y *= saturation;
    float3 rgb = hsv_to_rgb(hsv);
    
    // 3. Contrast (Pivot around 0.5)
    rgb = (rgb - 0.5) * contrast + 0.5;
    
    OutputTexture[dispatchThreadId.xy] = float4(rgb, color.a);
}
)";

HdrFilter::HdrFilter() {}
HdrFilter::~HdrFilter() { Shutdown(); }

bool HdrFilter::Init(D3D12Context& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) {
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
        params[0].Constants.Num32BitValues = 8; // inputRes(2) + outputRes(2) + saturation(1) + contrast(1) + padding(2)
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

        D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
        rootDesc.NumParameters = 3;
        rootDesc.pParameters = params;
        rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3DBlob> signature;
        Microsoft::WRL::ComPtr<ID3DBlob> error;
        if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error))) {
            if (error) diagnosticsLog(DiagnosticsLevel::Info, "HdrFilter", "RootSig Serialize Error: %s", (char*)error->GetBufferPointer());
            return false;
        }
        if (FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&mRootSignature)))) {
            return false;
        }
    }

    // 2. Compile Shader
    Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
    if (FAILED(D3DCompile(HDR_SHADER_SOURCE, strlen(HDR_SHADER_SOURCE), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &shaderBlob, &errorBlob))) {
        if (errorBlob) diagnosticsLog(DiagnosticsLevel::Info, "HdrFilter", "Shader Compile Error: %s", (char*)errorBlob->GetBufferPointer());
        return false;
    }

    // 3. Create PSO
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = mRootSignature.Get();
    psoDesc.CS = { shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize() };
    
    if (FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mPipelineState)))) {
        return false;
    }

    // 4. Create Descriptor Heap
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = 2; // 1 SRV + 1 UAV
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mDescriptorHeap)))) {
        return false;
    }

    return true;
}

void HdrFilter::Execute(D3D12Context& context, ID3D12Resource* input, ID3D12Resource* output) {
    auto cmdList = context.GetCommandList();
    auto device = context.GetDevice();

    // 1. Create Descriptors
    
    D3D12_CPU_DESCRIPTOR_HANDLE handle = mDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    UINT handleSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // SRV for Input
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(input, &srvDesc, handle);

    // UAV for Output
    handle.ptr += handleSize;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(output, nullptr, &uavDesc, handle);

    // 3. Dispatch
    cmdList->SetPipelineState(mPipelineState.Get());
    cmdList->SetComputeRootSignature(mRootSignature.Get());
    
    ID3D12DescriptorHeap* heaps[] = { mDescriptorHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);

    // Set Constants
    struct ShaderConstants {
        uint32_t inputRes[2];
        uint32_t outputRes[2];
        float saturation;
        float contrast;
        float padding[2];
    } constants;
    
    constants.inputRes[0] = mInputWidth;
    constants.inputRes[1] = mInputHeight;
    constants.outputRes[0] = mOutputWidth;
    constants.outputRes[1] = mOutputHeight;
    constants.saturation = mParams.saturation;
    constants.contrast = mParams.contrast;
    constants.padding[0] = 0.0f;
    constants.padding[1] = 0.0f;

    cmdList->SetComputeRoot32BitConstants(0, 8, &constants, 0);

    // Set Descriptor Tables
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = mDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
    cmdList->SetComputeRootDescriptorTable(1, gpuHandle); // SRV
    
    gpuHandle.ptr += handleSize;
    cmdList->SetComputeRootDescriptorTable(2, gpuHandle); // UAV

    cmdList->Dispatch((mOutputWidth + 7) / 8, (mOutputHeight + 7) / 8, 1);
}

void HdrFilter::Shutdown() {
    mRootSignature.Reset();
    mPipelineState.Reset();
}

void HdrFilter::SetParams(float saturation, float contrast, float blackCrushThreshold, float blackCrushStrength) {
    mParams.saturation = saturation;
    mParams.contrast = contrast;
    mParams.blackCrushThreshold = blackCrushThreshold;
    mParams.blackCrushStrength = blackCrushStrength;
}

} // namespace renderer
} // namespace fallout
