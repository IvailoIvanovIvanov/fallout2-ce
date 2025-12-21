#include "blur_filter.h"
#include "../diagnostics.h"
#include <d3dcompiler.h>

namespace fallout {
namespace renderer {

const char* BLUR_SHADER_SOURCE = R"(
cbuffer BlurParams : register(b0) {
    uint2 inputResolution;
    uint2 outputResolution;
    float strength;
    float padding;
};

Texture2D<float4> InputTexture : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    if (dispatchThreadId.x >= outputResolution.x || dispatchThreadId.y >= outputResolution.y) return;

    // Nearest Neighbor Scaling
    int2 inputCoord;
    inputCoord.x = (dispatchThreadId.x * inputResolution.x) / outputResolution.x;
    inputCoord.y = (dispatchThreadId.y * inputResolution.y) / outputResolution.y;

    // Clamp to input bounds
    inputCoord = clamp(inputCoord, int2(0, 0), int2(inputResolution.x - 1, inputResolution.y - 1));

    float4 color = InputTexture[inputCoord];
    
    if (strength > 0.01) {
        // Sample neighbors in Input Space
        float4 l = InputTexture[clamp(inputCoord + int2(-1, 0), int2(0,0), int2(inputResolution.x-1, inputResolution.y-1))];
        float4 r = InputTexture[clamp(inputCoord + int2(1, 0), int2(0,0), int2(inputResolution.x-1, inputResolution.y-1))];
        float4 u = InputTexture[clamp(inputCoord + int2(0, -1), int2(0,0), int2(inputResolution.x-1, inputResolution.y-1))];
        float4 d = InputTexture[clamp(inputCoord + int2(0, 1), int2(0,0), int2(inputResolution.x-1, inputResolution.y-1))];
        
        float4 avg = (color + l + r + u + d) * 0.2;
        color = lerp(color, avg, strength);
    }
    
    OutputTexture[dispatchThreadId.xy] = color;
}
)";

BlurFilter::BlurFilter() {}
BlurFilter::~BlurFilter() { Shutdown(); }

bool BlurFilter::Init(D3D12Context& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) {
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
        params[0].Constants.Num32BitValues = 6; // inputRes(2) + outputRes(2) + strength(1) + padding(1)
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
            if (error) diagnosticsLog(DiagnosticsLevel::Info, "BlurFilter", "RootSig Serialize Error: %s", (char*)error->GetBufferPointer());
            return false;
        }
        if (FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&mRootSignature)))) {
            return false;
        }
    }

    // 2. Compile Shader
    Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
    if (FAILED(D3DCompile(BLUR_SHADER_SOURCE, strlen(BLUR_SHADER_SOURCE), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &shaderBlob, &errorBlob))) {
        if (errorBlob) diagnosticsLog(DiagnosticsLevel::Info, "BlurFilter", "Shader Compile Error: %s", (char*)errorBlob->GetBufferPointer());
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

void BlurFilter::Execute(D3D12Context& context, BufferManager& buffers) {
    auto cmdList = context.GetCommandList();
    auto device = context.GetDevice();

    // 1. Create Descriptors (every frame, as buffers might change due to double buffering)
    auto inputBuffer = buffers.GetInputBuffer();
    auto outputBuffer = buffers.GetOutputBuffer();
    
    D3D12_CPU_DESCRIPTOR_HANDLE handle = mDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    UINT handleSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // SRV for Input
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(inputBuffer, &srvDesc, handle);

    // UAV for Output
    handle.ptr += handleSize;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(outputBuffer, nullptr, &uavDesc, handle);

    // 2. Resource Barriers
    D3D12_RESOURCE_BARRIER barriers[2] = {};
    
    // Input: Ensure it's in SRV state (BufferManager leaves it in PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE)
    // No transition needed if it's already there. But let's be safe or assume BufferManager contract.
    // BufferManager::UploadInput leaves it in PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE.
    
    // Output: Transition to UAV
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = outputBuffer;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON; // RenderPipeline leaves it in COMMON
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    
    cmdList->ResourceBarrier(1, barriers);

    // 3. Dispatch
    cmdList->SetPipelineState(mPipelineState.Get());
    cmdList->SetComputeRootSignature(mRootSignature.Get());
    
    ID3D12DescriptorHeap* heaps[] = { mDescriptorHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);

    // Set Constants
    mParams.resolution[0] = mInputWidth;
    mParams.resolution[1] = mInputHeight;
    // We need to pass output resolution too, but struct only has resolution[2].
    // Let's update the struct in header first, or just cast here if we changed the shader.
    // The shader expects 6 floats/uints.
    
    struct ShaderConstants {
        uint32_t inputRes[2];
        uint32_t outputRes[2];
        float strength;
        float padding;
    } constants;
    
    constants.inputRes[0] = mInputWidth;
    constants.inputRes[1] = mInputHeight;
    constants.outputRes[0] = mOutputWidth;
    constants.outputRes[1] = mOutputHeight;
    constants.strength = mParams.strength;
    constants.padding = 0.0f;

    cmdList->SetComputeRoot32BitConstants(0, 6, &constants, 0);

    // Set Descriptor Tables
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = mDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
    cmdList->SetComputeRootDescriptorTable(1, gpuHandle); // SRV
    
    gpuHandle.ptr += handleSize;
    cmdList->SetComputeRootDescriptorTable(2, gpuHandle); // UAV

    cmdList->Dispatch((mOutputWidth + 7) / 8, (mOutputHeight + 7) / 8, 1);

    // 4. Restore Output State (Optional, but RenderPipeline expects UNORDERED_ACCESS? No, it transitions FROM UNORDERED_ACCESS)
    // So we leave it in UNORDERED_ACCESS.
}

void BlurFilter::Shutdown() {
    mRootSignature.Reset();
    mPipelineState.Reset();
}

void BlurFilter::SetStrength(float strength) {
    mParams.strength = strength;
}

} // namespace renderer
} // namespace fallout
