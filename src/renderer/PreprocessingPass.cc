#include "PreprocessingPass.h"
#include "../diagnostics.h"
#include <d3dcompiler.h>

namespace fallout {
namespace renderer {

const char* PREPROCESS_SHADER_SOURCE = R"(
cbuffer PreprocessParams : register(b0) {
    uint2 resolution;
    float blurStrength;
    float hdrSaturation;
    float hdrContrast;
    float padding[2];
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
    if (dispatchThreadId.x >= resolution.x || dispatchThreadId.y >= resolution.y) return;

    float4 color = InputTexture[dispatchThreadId.xy];
    
    float3 hsv = rgb_to_hsv(color.rgb);
    hsv.y *= hdrSaturation;
    float3 rgb = hsv_to_rgb(hsv);
    
    rgb = (rgb - 0.5) * hdrContrast + 0.5;
    
    OutputTexture[dispatchThreadId.xy] = float4(rgb, color.a);
}
)";

PreprocessingPass::PreprocessingPass() {
    // Default params
    mParams.blurStrength = 0.0f;
    mParams.hdrSaturation = 1.0f;
    mParams.hdrContrast = 1.0f;
}

PreprocessingPass::~PreprocessingPass() {
    Shutdown();
}

bool PreprocessingPass::Init(D3D12Context& context, int inputWidth, int inputHeight, int outputWidth, int outputHeight) {
    auto device = context.GetDevice();

    // 1. Create Root Signature
    {
        D3D12_ROOT_PARAMETER rootParameters[2];
        
        // Param 0: Root Constants (b0)
        rootParameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParameters[0].Constants.ShaderRegister = 0;
        rootParameters[0].Constants.RegisterSpace = 0;
        rootParameters[0].Constants.Num32BitValues = 7; // 2+1+1+1+2
        rootParameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Param 1: Descriptor Table (t0, u0)
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

        rootParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParameters[1].DescriptorTable.NumDescriptorRanges = 2;
        rootParameters[1].DescriptorTable.pDescriptorRanges = ranges;
        rootParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
        rootSigDesc.NumParameters = 2;
        rootSigDesc.pParameters = rootParameters;
        rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

        Microsoft::WRL::ComPtr<ID3DBlob> signature;
        Microsoft::WRL::ComPtr<ID3DBlob> error;
        if (FAILED(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error))) {
            return false;
        }
        if (FAILED(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&mRootSignature)))) {
            return false;
        }
    }

    // 2. Compile Shader
    Microsoft::WRL::ComPtr<ID3DBlob> computeShader;
    Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3DCompile(PREPROCESS_SHADER_SOURCE, strlen(PREPROCESS_SHADER_SOURCE), 
        nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &computeShader, &errorBlob);
    
    if (FAILED(hr)) {
        if (errorBlob) {
            diagnosticsLog(DiagnosticsLevel::Error, "PreprocessingPass", "Shader Compile Error: %s", (char*)errorBlob->GetBufferPointer());
        }
        return false;
    }

    // 3. Create PSO
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = mRootSignature.Get();
        psoDesc.CS = { computeShader->GetBufferPointer(), computeShader->GetBufferSize() };
        
        if (FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mPipelineState)))) {
            return false;
        }
    }

    // 4. Create Descriptor Heap
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = 2; // 1 SRV + 1 UAV
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        
        if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mDescriptorHeap)))) {
            return false;
        }
    }

    return true;
}

void PreprocessingPass::Shutdown() {
    mDescriptorHeap.Reset();
    mPipelineState.Reset();
    mRootSignature.Reset();
}

void PreprocessingPass::SetParams(float blur, float saturation, float contrast) {
    mParams.blurStrength = blur;
    mParams.hdrSaturation = saturation;
    mParams.hdrContrast = contrast;
}

void PreprocessingPass::Execute(D3D12Context& context, BufferManager& buffers) {
    auto cmdList = context.GetCommandList();
    auto device = context.GetDevice();

    // 1. Update Descriptors
    auto inputBuffer = buffers.GetInputBuffer();
    auto outputBuffer = buffers.GetOutputBuffer();
    
    D3D12_CPU_DESCRIPTOR_HANDLE handle = mDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
    UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // SRV for Input
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(inputBuffer, &srvDesc, handle);

    // UAV for Output
    handle.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(outputBuffer, nullptr, &uavDesc, handle);

    // 2. Set Pipeline State
    cmdList->SetPipelineState(mPipelineState.Get());
    cmdList->SetComputeRootSignature(mRootSignature.Get());

    ID3D12DescriptorHeap* heaps[] = { mDescriptorHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);

    cmdList->SetComputeRootDescriptorTable(1, mDescriptorHeap->GetGPUDescriptorHandleForHeapStart());

    // 3. Set Constants
    mParams.resolution[0] = buffers.GetWidth();
    mParams.resolution[1] = buffers.GetHeight();
    cmdList->SetComputeRoot32BitConstants(0, 7, &mParams, 0);

    // 4. Dispatch
    // 8x8 threads per group
    int groupsX = (buffers.GetWidth() + 7) / 8;
    int groupsY = (buffers.GetHeight() + 7) / 8;
    cmdList->Dispatch(groupsX, groupsY, 1);
}

} // namespace renderer
} // namespace fallout
