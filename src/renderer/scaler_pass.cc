#include "scaler_pass.h"
#include "../diagnostics.h"
#include <d3dcompiler.h>
#include <algorithm>

namespace fallout {
namespace renderer {

const char* SCALER_SHADER_SOURCE = R"(
cbuffer ScalerParams : register(b0) {
    uint2 inputResolution;
    uint2 outputResolution;
    float2 offset;
    float2 scale;
};

Texture2D<float4> InputTexture : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID) {
    if (dispatchThreadId.x >= outputResolution.x || dispatchThreadId.y >= outputResolution.y) return;

    // Calculate normalized coordinates in output space
    float2 uv = (float2(dispatchThreadId.xy) + 0.5) / float2(outputResolution);
    
    // Apply inverse transform to get input UV
    // Output = Input * Scale + Offset
    // Input = (Output - Offset) / Scale
    
    // We want to map [0,1] output to [0,1] input, but with letterboxing.
    // Actually, simpler:
    // Screen Coord -> Input Coord
    
    float2 screenCoord = float2(dispatchThreadId.xy);
    float2 inputCoord = (screenCoord - offset) / scale;
    
    float4 color = float4(0, 0, 0, 1); // Black bars
    
    if (inputCoord.x >= 0 && inputCoord.x < inputResolution.x &&
        inputCoord.y >= 0 && inputCoord.y < inputResolution.y) {
        
        // Nearest Neighbor Sampling
        color = InputTexture[int2(inputCoord)];
    }
    
    OutputTexture[dispatchThreadId.xy] = color;
}
)";

ScalerPass::ScalerPass() {}
ScalerPass::~ScalerPass() { Shutdown(); }

bool ScalerPass::Init(D3D12Context& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) {
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
        params[0].Constants.Num32BitValues = 8; // inputRes(2) + outputRes(2) + offset(2) + scale(2)
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
            if (error) diagnosticsLog(DiagnosticsLevel::Info, "ScalerPass", "RootSig Serialize Error: %s", (char*)error->GetBufferPointer());
            return false;
        }
        if (FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&mRootSignature)))) {
            return false;
        }
    }

    // 2. Compile Shader
    Microsoft::WRL::ComPtr<ID3DBlob> shaderBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
    if (FAILED(D3DCompile(SCALER_SHADER_SOURCE, strlen(SCALER_SHADER_SOURCE), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &shaderBlob, &errorBlob))) {
        if (errorBlob) diagnosticsLog(DiagnosticsLevel::Info, "ScalerPass", "Shader Compile Error: %s", (char*)errorBlob->GetBufferPointer());
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

void ScalerPass::Execute(D3D12Context& context, ID3D12Resource* input, ID3D12Resource* output) {
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

    // 2. Calculate Scale and Offset (Letterboxing)
    // We need to know the actual dimensions of the input resource, not just mInputWidth.
    // But Init gave us mInputWidth. Let's assume input resource matches mInputWidth/Height.
    // Wait, if we upscale, input might be larger than mInputWidth (if ML pass ran).
    // We should probably get dimensions from resource desc, but for now let's trust Init or update mInputWidth dynamically?
    // Actually, ScalerPass is initialized with the *expected* input size (e.g. 2560x1920 or 640x480).
    
    float scaleX = (float)mOutputWidth / mInputWidth;
    float scaleY = (float)mOutputHeight / mInputHeight;
    float scale = std::min(scaleX, scaleY);
    
    float finalWidth = mInputWidth * scale;
    float finalHeight = mInputHeight * scale;
    
    float offsetX = (mOutputWidth - finalWidth) * 0.5f;
    float offsetY = (mOutputHeight - finalHeight) * 0.5f;

    // 3. Dispatch
    cmdList->SetPipelineState(mPipelineState.Get());
    cmdList->SetComputeRootSignature(mRootSignature.Get());
    
    ID3D12DescriptorHeap* heaps[] = { mDescriptorHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);

    struct Constants {
        uint32_t inputRes[2];
        uint32_t outputRes[2];
        float offset[2];
        float scale[2];
    } constants;
    
    constants.inputRes[0] = mInputWidth;
    constants.inputRes[1] = mInputHeight;
    constants.outputRes[0] = mOutputWidth;
    constants.outputRes[1] = mOutputHeight;
    constants.offset[0] = offsetX;
    constants.offset[1] = offsetY;
    constants.scale[0] = scale;
    constants.scale[1] = scale;

    cmdList->SetComputeRoot32BitConstants(0, 8, &constants, 0);

    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = mDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
    cmdList->SetComputeRootDescriptorTable(1, gpuHandle); // SRV
    
    gpuHandle.ptr += handleSize;
    cmdList->SetComputeRootDescriptorTable(2, gpuHandle); // UAV

    cmdList->Dispatch((mOutputWidth + 7) / 8, (mOutputHeight + 7) / 8, 1);
}

void ScalerPass::Shutdown() {
    mRootSignature.Reset();
    mPipelineState.Reset();
    mDescriptorHeap.Reset();
}

} // namespace renderer
} // namespace fallout
