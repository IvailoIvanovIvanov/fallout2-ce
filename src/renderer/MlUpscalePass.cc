#include "MlUpscalePass.h"
#include "../diagnostics.h"
#include <d3dcompiler.h>

namespace fallout {
namespace renderer {

const char* RGBA_TO_PLANAR_SHADER = R"(
cbuffer Params : register(b0) {
    uint width;
    uint height;
};
Texture2D<float4> Input : register(t0);
RWStructuredBuffer<float> Output : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    if (dtid.x >= width || dtid.y >= height) return;
    float4 color = Input[dtid.xy];
    uint index = dtid.y * width + dtid.x;
    uint planeSize = width * height;
    Output[index] = color.r;
    Output[index + planeSize] = color.g;
    Output[index + 2 * planeSize] = color.b;
}
)";

const char* PLANAR_TO_RGBA_SHADER = R"(
cbuffer Params : register(b0) {
    uint width;
    uint height;
};
StructuredBuffer<float> Input : register(t0);
RWTexture2D<float4> Output : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID) {
    if (dtid.x >= width || dtid.y >= height) return;
    uint index = dtid.y * width + dtid.x;
    uint planeSize = width * height;
    float r = Input[index];
    float g = Input[index + planeSize];
    float b = Input[index + 2 * planeSize];
    Output[dtid.xy] = float4(r, g, b, 1.0);
}
)";

MlUpscalePass::MlUpscalePass() = default;
MlUpscalePass::~MlUpscalePass() { Shutdown(); }

bool MlUpscalePass::Init(D3D12Context& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) {
    mWidth = inputWidth;
    mHeight = inputHeight;
    mOutputWidth = outputWidth;
    mOutputHeight = outputHeight;
    auto device = context.GetDevice();

    // 1. Init UpscalerML
    // Note: UpscalerML expects model path. We assume it's in the same dir or known path.
    if (!mUpscaler.init(device, "realesrgan-x4plus.onnx")) {
        diagnosticsLog(DiagnosticsLevel::Error, "MlUpscalePass", "Failed to init UpscalerML");
        return false;
    }

    // 2. Create Tensors
    // Input: 1x3xHxW float32
    size_t inputSize = inputWidth * inputHeight * 3 * sizeof(float);
    // Output: 1x3x(4H)x(4W) float32
    size_t outputSize = outputWidth * outputHeight * 3 * sizeof(float);

    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = inputSize;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&mInputTensor)))) return false;

    bufDesc.Width = outputSize;
    if (FAILED(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &bufDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&mOutputTensor)))) return false;

    // 3. Create Root Signature (Shared for both shaders)
    {
        D3D12_ROOT_PARAMETER params[2];
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.RegisterSpace = 0;
        params[0].Constants.Num32BitValues = 2;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_DESCRIPTOR_RANGE ranges[2];
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].RegisterSpace = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].RegisterSpace = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 1;

        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 2;
        params[1].DescriptorTable.pDescriptorRanges = ranges;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC desc = {};
        desc.NumParameters = 2;
        desc.pParameters = params;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        
        Microsoft::WRL::ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) return false;
        if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&mRootSignature)))) return false;
    }

    // 4. Compile Shaders & Create PSOs
    Microsoft::WRL::ComPtr<ID3DBlob> csBlob, errBlob;
    
    // RgbaToPlanar
    if (FAILED(D3DCompile(RGBA_TO_PLANAR_SHADER, strlen(RGBA_TO_PLANAR_SHADER), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &csBlob, &errBlob))) {
        if (errBlob) diagnosticsLog(DiagnosticsLevel::Error, "MlUpscalePass", "RgbaToPlanar Compile Error: %s", (char*)errBlob->GetBufferPointer());
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = mRootSignature.Get();
    psoDesc.CS = { csBlob->GetBufferPointer(), csBlob->GetBufferSize() };
    if (FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mPsoRgbaToPlanar)))) return false;

    // PlanarToRgba
    if (FAILED(D3DCompile(PLANAR_TO_RGBA_SHADER, strlen(PLANAR_TO_RGBA_SHADER), nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &csBlob, &errBlob))) {
        if (errBlob) diagnosticsLog(DiagnosticsLevel::Error, "MlUpscalePass", "PlanarToRgba Compile Error: %s", (char*)errBlob->GetBufferPointer());
        return false;
    }
    psoDesc.CS = { csBlob->GetBufferPointer(), csBlob->GetBufferSize() };
    if (FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mPsoPlanarToRgba)))) return false;

    // 5. Create Descriptor Heap
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = 4; // 2 for Pass 1, 2 for Pass 2
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mDescriptorHeap)))) return false;

    return true;
}

void MlUpscalePass::Execute(D3D12Context& context, BufferManager& buffers) {
    auto cmdList = context.GetCommandList();
    auto device = context.GetDevice();
    
    // 1. Update Descriptors
    D3D12_CPU_DESCRIPTOR_HANDLE handle = mDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // Pass 1: InputTexture (SRV) -> InputTensor (UAV)
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(buffers.GetInputBuffer(), &srvDesc, handle);
    
    handle.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN; // Structured Buffer
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = mWidth * mHeight * 3;
    uavDesc.Buffer.StructureByteStride = 4; // float
    uavDesc.Buffer.CounterOffsetInBytes = 0;
    uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    device->CreateUnorderedAccessView(mInputTensor.Get(), nullptr, &uavDesc, handle);

    // Pass 2: OutputTensor (SRV) -> OutputTexture (UAV)
    handle.ptr += increment;
    srvDesc.Format = DXGI_FORMAT_UNKNOWN; // Structured Buffer
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = mOutputWidth * mOutputHeight * 3;
    srvDesc.Buffer.StructureByteStride = 4;
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    device->CreateShaderResourceView(mOutputTensor.Get(), &srvDesc, handle);

    handle.ptr += increment;
    uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(buffers.GetOutputBuffer(), nullptr, &uavDesc, handle);

    // 2. Execute RgbaToPlanar
    cmdList->SetPipelineState(mPsoRgbaToPlanar.Get());
    cmdList->SetComputeRootSignature(mRootSignature.Get());
    ID3D12DescriptorHeap* heaps[] = { mDescriptorHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);
    
    cmdList->SetComputeRootDescriptorTable(1, mDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
    
    uint32_t params[] = { (uint32_t)mWidth, (uint32_t)mHeight };
    cmdList->SetComputeRoot32BitConstants(0, 2, params, 0);
    
    cmdList->Dispatch((mWidth + 7) / 8, (mHeight + 7) / 8, 1);

    // Barrier: InputTensor UAV -> Common (for ML)
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = mInputTensor.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);

    // 3. Execute ML
    // TODO: Implement proper synchronization with UpscalerML if it uses a different queue or submits its own work.
    // For now, we assume UpscalerML needs to be integrated into the command list flow.
    // mUpscaler.dispatchGpuToGpu(mInputTensor.Get(), mOutputTensor.Get(), mWidth, mHeight);

    // Barrier: OutputTensor Common -> SRV
    barrier.Transition.pResource = mOutputTensor.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    cmdList->ResourceBarrier(1, &barrier);

    // 4. Execute PlanarToRgba
    cmdList->SetPipelineState(mPsoPlanarToRgba.Get());
    cmdList->SetComputeRootSignature(mRootSignature.Get());
    cmdList->SetDescriptorHeaps(1, heaps);
    
    // Set Descriptor Table for Pass 2 (Offset 2 descriptors)
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = mDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
    gpuHandle.ptr += 2 * device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    cmdList->SetComputeRootDescriptorTable(1, gpuHandle);
    
    uint32_t outParams[] = { (uint32_t)mOutputWidth, (uint32_t)mOutputHeight };
    cmdList->SetComputeRoot32BitConstants(0, 2, outParams, 0);
    
    cmdList->Dispatch((mOutputWidth + 7) / 8, (mOutputHeight + 7) / 8, 1);
}

void MlUpscalePass::Shutdown() {
    mInputTensor.Reset();
    mOutputTensor.Reset();
    mRootSignature.Reset();
    mPsoRgbaToPlanar.Reset();
    mPsoPlanarToRgba.Reset();
    mDescriptorHeap.Reset();
}

} // namespace renderer
} // namespace fallout
