#include "upscaler_ml.h"
#include "../diagnostics.h"

#ifdef HAS_ONNX_RUNTIME
#include <onnxruntime/onnxruntime_cxx_api.h>
#include <onnxruntime/dml_provider_factory.h>
#include <DirectML.h>
#include <wrl/client.h>
#endif

#include "gpu_device.h"
#include <string>
#include <memory>
#include <cstdarg>
#include <cstdio>
#include <chrono>

namespace fallout {

struct UpscalerML::Impl {
#ifdef HAS_ONNX_RUNTIME
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "Fallout2Upscaler"};
    std::unique_ptr<Ort::Session> session;
    Ort::SessionOptions sessionOptions;
    Ort::MemoryInfo memoryInfo{nullptr};
    const OrtDmlApi* dmlApi = nullptr;
#endif
    ID3D12Device* d3dDevice = nullptr;
    bool initialized = false;
    bool usingCpuFallback = false;
    bool directMLFailed = false;
    std::string lastModelPath;
    std::function<void(const char*)> logger;
    
    // Model input/output dimensions
    int64_t modelInputHeight = -1;
    int64_t modelInputWidth = -1;
    int modelScaleFactor = 4; // Default 4x upscaling
    
    std::string inputName = "input";
    std::string outputName = "output";
    
    Impl() 
#ifdef HAS_ONNX_RUNTIME
        : memoryInfo(Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)) 
#endif
    {}
};

UpscalerML::UpscalerML() : mImpl(std::make_unique<Impl>()) {}
UpscalerML::~UpscalerML() = default;

bool UpscalerML::init(ID3D12Device* device, const std::string& modelPath, std::function<void(const char*)> logger) {
    mImpl->d3dDevice = device;
    mImpl->logger = logger;
    mImpl->lastModelPath = modelPath;
    
    auto log = [&](const char* fmt, ...) {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        
        // Log to system
        diagnosticsLog(DiagnosticsLevel::Info, "UPSCALER_ML", "%s", buf);
        
        // Log to file if provided
        if (mImpl->logger) mImpl->logger(buf);
    };
    
    log("=== ML INIT START ===");
    log("Model path: %s", modelPath.c_str());
    
#ifdef HAS_ONNX_RUNTIME
    log("✓ ONNX Runtime IS compiled in (HAS_ONNX_RUNTIME defined)");
#else
    log("✗ ONNX Runtime NOT compiled in (HAS_ONNX_RUNTIME NOT defined)");
#endif

#ifdef HAS_ONNX_RUNTIME
    log("ONNX Runtime is compiled in");
    try {
        // 1. Initialize session options
        log("Initializing ONNX Runtime session...");
        
        // Use ALL optimizations for best performance
        mImpl->sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        
        // Let ORT decide the number of threads
        mImpl->sessionOptions.SetIntraOpNumThreads(0);
        mImpl->sessionOptions.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        
        // Enable memory optimizations
        mImpl->sessionOptions.EnableMemPattern();
        mImpl->sessionOptions.EnableCpuMemArena();
        
        // Enable DirectML
        log("Attempting to enable DirectML execution provider...");
        bool dmlEnabled = false;
        
        // Try to use the engine's D3D12 device and command queue for better performance and compatibility
        ID3D12Device* d3dDevice = mImpl->d3dDevice;
        ID3D12CommandQueue* commandQueue = gpuDeviceGetCommandQueue();
        
        if (d3dDevice && commandQueue) {
            log("Using engine's D3D12 device and command queue for DirectML");
            
            Microsoft::WRL::ComPtr<IDMLDevice> dmlDevice;
            // Use DML_CREATE_DEVICE_FLAG_NONE for production
            HRESULT hr = DMLCreateDevice(d3dDevice, DML_CREATE_DEVICE_FLAG_NONE, IID_PPV_ARGS(&dmlDevice));
            if (SUCCEEDED(hr)) {
                const OrtDmlApi* dmlApi = nullptr;
                // Get the DML-specific API from ONNX Runtime
                if (Ort::GetApi().GetExecutionProviderApi("DML", ORT_API_VERSION, (const void**)&dmlApi) == nullptr && dmlApi) {
                    mImpl->dmlApi = dmlApi;
                    try {
                        Ort::ThrowOnError(dmlApi->SessionOptionsAppendExecutionProvider_DML1(mImpl->sessionOptions, dmlDevice.Get(), commandQueue));
                        log("DirectML enabled successfully via DML1 API (shared device)!");
                        dmlEnabled = true;
                    } catch (const std::exception& e) {
                        log("DML1 API failed: %s", e.what());
                    }
                } else {
                    log("Could not get OrtDmlApi, falling back to device ID...");
                }
            } else {
                log("DMLCreateDevice failed: 0x%08X", hr);
            }
        }

        // Fallback to device ID if shared device failed
        if (!dmlEnabled) {
            // Try device 1 (often dGPU on dual-GPU systems)
            try {
                log("Trying DirectML device 1...");
                Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(mImpl->sessionOptions, 1));
                log("DirectML enabled successfully on device 1!");
                dmlEnabled = true;
            } catch (const std::exception& e) {
                log("Device 1 failed (%s), trying device 0...", e.what());
            }
            
            if (!dmlEnabled) {
                try {
                    Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(mImpl->sessionOptions, 0));
                    log("DirectML enabled successfully on device 0!");
                    dmlEnabled = true;
                } catch (const std::exception& e) {
                    log("WARNING: DirectML initialization failed: %s", e.what());
                    log("Cannot proceed without GPU acceleration");
                    return false;
                }
            }
        }

        log("Session options configured");

        // 2. Load the model (convert to wchar_t for Windows)
        log("Loading ONNX model...");
        
        // Simple string to wchar_t conversion for ASCII paths
        std::wstring wModelPath;
        wModelPath.reserve(modelPath.size());
        for (char c : modelPath) {
            wModelPath += static_cast<wchar_t>(static_cast<unsigned char>(c));
        }
        
        mImpl->session = std::make_unique<Ort::Session>(mImpl->env, wModelPath.c_str(), mImpl->sessionOptions);
        log("Model loaded successfully!");
        
        {
            FILE* f = fopen("debug_ml.log", "a");
            if (f) {
                fprintf(f, "DEBUG: Model loaded successfully\n");
                size_t inputCount = mImpl->session->GetInputCount();
                fprintf(f, "DEBUG: Input count: %zu\n", inputCount);
                
                OrtAllocator* allocator;
                Ort::GetApi().GetAllocatorWithDefaultOptions(&allocator);
                char* inputNameC = nullptr;
                Ort::GetApi().SessionGetInputName(*mImpl->session, 0, allocator, &inputNameC);
                if (inputNameC) {
                    fprintf(f, "DEBUG: Input 0 name: %s\n", inputNameC);
                    mImpl->inputName = inputNameC;
                    allocator->Free(allocator, inputNameC);
                }
                
                char* outputNameC = nullptr;
                Ort::GetApi().SessionGetOutputName(*mImpl->session, 0, allocator, &outputNameC);
                if (outputNameC) {
                    fprintf(f, "DEBUG: Output 0 name: %s\n", outputNameC);
                    mImpl->outputName = outputNameC;
                    allocator->Free(allocator, outputNameC);
                }
                fclose(f);
            }
        }
        
        // Log model input/output info
        size_t inputCount = mImpl->session->GetInputCount();
        size_t outputCount = mImpl->session->GetOutputCount();
        log("Model has %zu input(s), %zu output(s)", inputCount, outputCount);
        
        // Get input tensor shape to determine expected dimensions
        Ort::TypeInfo inputTypeInfo = mImpl->session->GetInputTypeInfo(0);
        auto inputTensorInfo = inputTypeInfo.GetTensorTypeAndShapeInfo();
        auto inputShape = inputTensorInfo.GetShape();
        
        if (inputShape.size() == 4) {
            mImpl->modelInputHeight = inputShape[2];
            mImpl->modelInputWidth = inputShape[3];
            log("Model expects input shape: [%lld, %lld, %lld, %lld]", 
                inputShape[0], inputShape[1], inputShape[2], inputShape[3]);
            log("Model input dimensions: %lldx%lld", mImpl->modelInputWidth, mImpl->modelInputHeight);
        }
        
        // Get output shape to determine scale factor
        Ort::TypeInfo outputTypeInfo = mImpl->session->GetOutputTypeInfo(0);
        auto outputTensorInfo = outputTypeInfo.GetTensorTypeAndShapeInfo();
        auto outputShape = outputTensorInfo.GetShape();
        
        if (outputShape.size() == 4 && inputShape.size() == 4) {
            if (inputShape[2] > 0 && outputShape[2] > 0) {
                mImpl->modelScaleFactor = static_cast<int>(outputShape[2] / inputShape[2]);
                log("Model scale factor: %dx", mImpl->modelScaleFactor);
            }
        }
        
        // Verify DirectML is actually being used
        log("DirectML provider should be active (device %d)", dmlEnabled ? 1 : 0);
        
        log("Successfully initialized ML model");
        log("=== ML INIT COMPLETE ===");
        mImpl->initialized = true;
        return true;
    } catch (const Ort::Exception& e) {
        log("!!! ERROR: Failed to init ONNX Runtime !!!");
        log("Exception: %s", e.what());
        return false;
    } catch (const std::exception& e) {
        log("!!! ERROR: Caught std::exception !!!");
        log("What: %s", e.what());
        return false;
    } catch (...) {
        log("!!! ERROR: Caught unknown exception !!!");
        return false;
    }
#else
    log("!!! ERROR: ONNX Runtime not compiled in (HAS_ONNX_RUNTIME not defined) !!!");
    return false;
#endif
}

bool UpscalerML::dispatch(const uint32_t* inputBuffer, uint32_t* outputBuffer, int width, int height) {
    // Use logger callback if available, otherwise fall back to diagnosticsLog
    auto log = [this](const char* fmt, ...) {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        
        if (mImpl->logger) {
            mImpl->logger(buf);
        }
        diagnosticsLog(DiagnosticsLevel::Info, "UPSCALER_ML", "%s", buf);
    };
    
    log("=== ML DISPATCH START === initialized=%d, mImpl=%p", 
        mImpl ? mImpl->initialized : -1, mImpl.get());
    log("Input dimensions: %dx%d", width, height);
    
    if (!mImpl->initialized) {
        log("ERROR: ML engine not initialized!");
        return false;
    }
    
    if (!inputBuffer || !outputBuffer) {
        log("ERROR: Null buffers - input=%p, output=%p", inputBuffer, outputBuffer);
        return false;
    }

#ifdef HAS_ONNX_RUNTIME
    try {
        auto totalStart = std::chrono::high_resolution_clock::now();
        
        // Check if model requires tiling
        bool needsTiling = (mImpl->modelInputHeight > 0 && mImpl->modelInputWidth > 0) &&
                          (width != mImpl->modelInputWidth || height != mImpl->modelInputHeight);
        
        if (needsTiling) {
            log("[DISPATCH] Model expects %lldx%lld but got %dx%d - using TILED inference",
                mImpl->modelInputWidth, mImpl->modelInputHeight, width, height);
            
            int tileSize = static_cast<int>(mImpl->modelInputWidth);
            int overlap = tileSize / 8; // 12.5% overlap to avoid seams
            int stride = tileSize - overlap;
            int scaleFactor = mImpl->modelScaleFactor;
            
            int outputWidth = width * scaleFactor;
            int outputHeight = height * scaleFactor;
            
            log("[DISPATCH] Tile size: %d, overlap: %d, stride: %d, output: %dx%d",
                tileSize, overlap, stride, outputWidth, outputHeight);
            
            // Allocate output buffer (will be accumulated from tiles)
            std::vector<float> accumulatedOutput(outputWidth * outputHeight * 3, 0.0f);
            std::vector<float> weightMap(outputWidth * outputHeight, 0.0f);
            
            int tilesProcessed = 0;
            int totalTiles = ((width + stride - 1) / stride) * ((height + stride - 1) / stride);
            
            // Process tiles
            for (int ty = 0; ty < height; ty += stride) {
                for (int tx = 0; tx < width; tx += stride) {
                    int tileW = std::min(tileSize, width - tx);
                    int tileH = std::min(tileSize, height - ty);
                    
                    // Skip if tile is too small (less than half tile size)
                    if (tileW < tileSize / 2 || tileH < tileSize / 2) continue;
                    
                    // Pad tile to expected size if needed
                    int paddedW = tileSize;
                    int paddedH = tileSize;
                    
                    // Prepare tile input tensor
                    size_t tilePixelCount = paddedW * paddedH;
                    size_t tileTensorSize = tilePixelCount * 3;
                    std::vector<float> tileTensorValues(tileTensorSize, 0.0f);
                    
                    // Copy tile data with padding
                    for (int y = 0; y < tileH; ++y) {
                        for (int x = 0; x < tileW; ++x) {
                            int srcIdx = (ty + y) * width + (tx + x);
                            int dstIdx = y * paddedW + x;
                            
                            uint32_t pixel = inputBuffer[srcIdx];
                            uint8_t r = (pixel) & 0xFF;
                            uint8_t g = (pixel >> 8) & 0xFF;
                            uint8_t b = (pixel >> 16) & 0xFF;
                            
                            tileTensorValues[dstIdx] = r / 255.0f;
                            tileTensorValues[dstIdx + tilePixelCount] = g / 255.0f;
                            tileTensorValues[dstIdx + tilePixelCount * 2] = b / 255.0f;
                        }
                    }
                    
                    // Run inference on tile
                    std::vector<int64_t> inputShape = {1, 3, paddedH, paddedW};
                    Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
                        mImpl->memoryInfo, tileTensorValues.data(), tileTensorSize, inputShape.data(), inputShape.size());
                    
                    const char* inputNames[] = {mImpl->inputName.c_str()};
                    const char* outputNames[] = {mImpl->outputName.c_str()};
                    
                    auto outputTensors = mImpl->session->Run(Ort::RunOptions{nullptr}, inputNames, &inputTensor, 1, outputNames, 1);
                    
                    // Extract tile output
                    float* floatOutput = outputTensors[0].GetTensorMutableData<float>();
                    int outTileW = paddedW * scaleFactor;
                    int outTileH = paddedH * scaleFactor;
                    size_t outTilePixelCount = outTileW * outTileH;
                    
                    // Accumulate into output with blending weights
                    for (int y = 0; y < std::min(outTileH, outputHeight - ty * scaleFactor); ++y) {
                        for (int x = 0; x < std::min(outTileW, outputWidth - tx * scaleFactor); ++x) {
                            int srcIdx = y * outTileW + x;
                            int dstX = tx * scaleFactor + x;
                            int dstY = ty * scaleFactor + y;
                            
                            if (dstX >= outputWidth || dstY >= outputHeight) continue;
                            
                            int dstIdx = dstY * outputWidth + dstX;
                            
                            // Calculate blend weight (fade at edges)
                            float weight = 1.0f;
                            if (overlap > 0) {
                                float xDist = std::min(x, outTileW - 1 - x) / (float)(overlap * scaleFactor);
                                float yDist = std::min(y, outTileH - 1 - y) / (float)(overlap * scaleFactor);
                                weight = std::min(1.0f, std::min(xDist, yDist));
                            }
                            
                            accumulatedOutput[dstIdx] += floatOutput[srcIdx] * weight;
                            accumulatedOutput[dstIdx + outputWidth * outputHeight] += floatOutput[srcIdx + outTilePixelCount] * weight;
                            accumulatedOutput[dstIdx + outputWidth * outputHeight * 2] += floatOutput[srcIdx + outTilePixelCount * 2] * weight;
                            weightMap[dstIdx] += weight;
                        }
                    }
                    
                    tilesProcessed++;
                }
            }
            
            log("[DISPATCH] Processed %d/%d tiles", tilesProcessed, totalTiles);
            
            // Normalize by weight and convert to output
            for (int i = 0; i < outputWidth * outputHeight; ++i) {
                float weight = weightMap[i] > 0 ? weightMap[i] : 1.0f;
                float r = accumulatedOutput[i] / weight;
                float g = accumulatedOutput[i + outputWidth * outputHeight] / weight;
                float b = accumulatedOutput[i + outputWidth * outputHeight * 2] / weight;
                
                uint8_t r8 = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, r)) * 255.0f);
                uint8_t g8 = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, g)) * 255.0f);
                uint8_t b8 = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, b)) * 255.0f);
                
                outputBuffer[i] = (0xFF << 24) | (b8 << 16) | (g8 << 8) | r8;
            }
            
            auto totalEnd = std::chrono::high_resolution_clock::now();
            auto totalMs = std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();
            log("[DISPATCH] === TILED COMPLETE: %.2f ms total (%.1f FPS) ===", totalMs, 1000.0 / totalMs);
            
            return true;
        }
        
        // Original non-tiled path for models that accept arbitrary sizes
        log("[DISPATCH] Starting preprocessing...");
        auto preprocessStart = std::chrono::high_resolution_clock::now();
        
        // 1. Pre-process: Convert RGBA8 to Float32 Planar (NCHW)
        // Input shape: [1, 3, height, width]
        // We ignore Alpha channel for inference usually
        size_t inputPixelCount = width * height;
        size_t inputTensorSize = inputPixelCount * 3; // RGB
        std::vector<float> inputTensorValues(inputTensorSize);
        
        for (size_t i = 0; i < inputPixelCount; ++i) {
            uint32_t pixel = inputBuffer[i];
            // Extract RGB (assuming ABGR or ARGB, need to check endianness/format)
            // SDL surfaces are usually ABGR on little-endian
            uint8_t r = (pixel) & 0xFF;
            uint8_t g = (pixel >> 8) & 0xFF;
            uint8_t b = (pixel >> 16) & 0xFF;
            
            // Normalize to [0, 1]
            inputTensorValues[i] = r / 255.0f;                     // R plane
            inputTensorValues[i + inputPixelCount] = g / 255.0f;   // G plane
            inputTensorValues[i + inputPixelCount * 2] = b / 255.0f; // B plane
        }
        
        auto preprocessEnd = std::chrono::high_resolution_clock::now();
        auto preprocessMs = std::chrono::duration<double, std::milli>(preprocessEnd - preprocessStart).count();
        
        log("[DISPATCH] Preprocessing took %.2f ms", preprocessMs);
        log("[DISPATCH] Creating input tensor...");

        // 2. Create Input Tensor
        std::vector<int64_t> inputShape = {1, 3, height, width};
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            mImpl->memoryInfo, inputTensorValues.data(), inputTensorSize, inputShape.data(), inputShape.size());

        // 3. Run Inference
        log("[DISPATCH] Running GPU inference via DirectML...");
        auto inferenceStart = std::chrono::high_resolution_clock::now();
        
        const char* inputNames[] = {mImpl->inputName.c_str()};
        const char* outputNames[] = {mImpl->outputName.c_str()};
        
        auto outputTensors = mImpl->session->Run(Ort::RunOptions{nullptr}, inputNames, &inputTensor, 1, outputNames, 1);
        
        auto inferenceEnd = std::chrono::high_resolution_clock::now();
        auto inferenceMs = std::chrono::duration<double, std::milli>(inferenceEnd - inferenceStart).count();
        
        log("[DISPATCH] *** GPU INFERENCE took %.2f ms (%.1f FPS) ***", inferenceMs, 1000.0 / inferenceMs);
        log("[DISPATCH] Starting postprocessing...");
        auto postprocessStart = std::chrono::high_resolution_clock::now();
        
        // 4. Post-process: Convert Float32 Planar to RGBA8
        // Output shape: [1, 3, outHeight, outWidth]
        float* floatOutput = outputTensors[0].GetTensorMutableData<float>();
        auto typeInfo = outputTensors[0].GetTensorTypeAndShapeInfo();
        auto outputShape = typeInfo.GetShape();
        
        int outHeight = static_cast<int>(outputShape[2]);
        int outWidth = static_cast<int>(outputShape[3]);
        size_t outputPixelCount = outWidth * outHeight;
        
        for (size_t i = 0; i < outputPixelCount; ++i) {
            float r = floatOutput[i];
            float g = floatOutput[i + outputPixelCount];
            float b = floatOutput[i + outputPixelCount * 2];
            
            // Clamp and convert
            uint8_t r8 = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, r)) * 255.0f);
            uint8_t g8 = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, g)) * 255.0f);
            uint8_t b8 = static_cast<uint8_t>(std::max(0.0f, std::min(1.0f, b)) * 255.0f);
            
            // Pack to RGBA (Alpha = 255)
            outputBuffer[i] = (0xFF << 24) | (b8 << 16) | (g8 << 8) | r8;
        }
        
        auto postprocessEnd = std::chrono::high_resolution_clock::now();
        auto postprocessMs = std::chrono::duration<double, std::milli>(postprocessEnd - postprocessStart).count();
        
        log("[DISPATCH] Postprocessing took %.2f ms", postprocessMs);
        
        double totalMs = preprocessMs + inferenceMs + postprocessMs;
        log("[DISPATCH] === COMPLETE: %.2f ms total (%.1f FPS) ===", totalMs, 1000.0 / totalMs);
        log("[DISPATCH] Breakdown: Preprocess=%.2f ms, GPU Inference=%.2f ms, Postprocess=%.2f ms", 
            preprocessMs, inferenceMs, postprocessMs);
        log("[DISPATCH] Output dimensions: %dx%d", outWidth, outHeight);
        
        return true;
    } catch (const Ort::Exception& e) {
        std::string errorMsg(e.what());
        log("!!! ERROR: ONNX Runtime Exception - %s", errorMsg.c_str());
        log("Hint: This might be a DirectML compatibility issue with the model.");
        log("Try using a different ONNX model or check ONNX Runtime/DirectML version.");
        return false;
    } catch (const std::exception& e) {
        log("!!! ERROR: std::exception - %s", e.what());
        return false;
    } catch (...) {
        log("!!! ERROR: Unknown exception in dispatch");
        return false;
    }
#else
    return false;
#endif
}

bool UpscalerML::dispatchGPU(ID3D12Resource* inputResource, uint32_t* outputBuffer, int width, int height) {
    auto log = [this](const char* fmt, ...) {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        if (mImpl->logger) mImpl->logger(buf);
        diagnosticsLog(DiagnosticsLevel::Info, "UPSCALER_ML", "%s", buf);
    };

#ifdef HAS_ONNX_RUNTIME
    if (!mImpl->initialized || !mImpl->dmlApi) {
        log("ERROR: ML engine or DML API not initialized!");
        return false;
    }

    try {
        auto totalStart = std::chrono::high_resolution_clock::now();

        // 1. Create OrtValue from D3D12 Resource
        void* dmlInputResource = nullptr;
        Ort::ThrowOnError(mImpl->dmlApi->CreateGPUAllocationFromD3DResource(inputResource, &dmlInputResource));

        // Create memory info for DML
        Ort::MemoryInfo dmlMemoryInfo("DML", OrtDeviceAllocator, 0, OrtMemTypeDefault);
        
        std::vector<int64_t> inputShape = {1, 3, height, width};
        size_t inputElementCount = 3 * width * height;
        
        // Use the C API to create the tensor from the DML allocation
        OrtValue* inputOrtValue = nullptr;
        Ort::ThrowOnError(Ort::GetApi().CreateTensorWithDataAsOrtValue(
            dmlMemoryInfo, dmlInputResource, inputElementCount * sizeof(float),
            inputShape.data(), inputShape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputOrtValue));
        
        Ort::Value inputTensor(inputOrtValue);

        // 2. Run Inference
        const char* inputNames[] = {mImpl->inputName.c_str()};
        const char* outputNames[] = {mImpl->outputName.c_str()};
        
        auto outputTensors = mImpl->session->Run(Ort::RunOptions{nullptr}, inputNames, &inputTensor, 1, outputNames, 1);

        // 3. Extract Output (to CPU buffer)
        float* floatOutput = outputTensors[0].GetTensorMutableData<float>();
        int outWidth = width * mImpl->modelScaleFactor;
        int outHeight = height * mImpl->modelScaleFactor;
        size_t outPixelCount = (size_t)outWidth * outHeight;

        // Convert planar float RGB to interleaved RGBA8
        for (int y = 0; y < outHeight; ++y) {
            for (int x = 0; x < outWidth; ++x) {
                size_t idx = (size_t)y * outWidth + x;
                float r = std::clamp(floatOutput[idx], 0.0f, 1.0f);
                float g = std::clamp(floatOutput[idx + outPixelCount], 0.0f, 1.0f);
                float b = std::clamp(floatOutput[idx + outPixelCount * 2], 0.0f, 1.0f);
                
                outputBuffer[idx] = 0xFF000000 | 
                                   ((uint8_t)(b * 255.0f) << 16) | 
                                   ((uint8_t)(g * 255.0f) << 8) | 
                                   ((uint8_t)(r * 255.0f));
            }
        }

        // Cleanup DML allocation
        mImpl->dmlApi->FreeGPUAllocation(dmlInputResource);

        auto totalEnd = std::chrono::high_resolution_clock::now();
        double totalMs = std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();
        log("[DISPATCH_GPU] Total GPU-to-CPU pipeline took %.2f ms (%.1f FPS)", totalMs, 1000.0 / totalMs);

        return true;
    } catch (const Ort::Exception& e) {
        log("!!! ERROR: ONNX Runtime Exception in dispatchGPU - %s", e.what());
        return false;
    } catch (const std::exception& e) {
        log("!!! ERROR: std::exception in dispatchGPU - %s", e.what());
        return false;
    }
#else
    return false;
#endif
}

bool UpscalerML::dispatchGpuToGpu(ID3D12Resource* inputResource, ID3D12Resource* outputResource, int width, int height) {
    auto log = [this](const char* fmt, ...) {
        char buf[1024];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        if (mImpl->logger) mImpl->logger(buf);
        diagnosticsLog(DiagnosticsLevel::Info, "UPSCALER_ML", "%s", buf);
    };

#ifdef HAS_ONNX_RUNTIME
    if (!mImpl->initialized || !mImpl->dmlApi) {
        log("ERROR: ML engine or DML API not initialized!");
        return false;
    }

    try {
        auto totalStart = std::chrono::high_resolution_clock::now();

        // 1. Create OrtValue from D3D12 Resources
        void* dmlInputResource = nullptr;
        void* dmlOutputResource = nullptr;
        Ort::ThrowOnError(mImpl->dmlApi->CreateGPUAllocationFromD3DResource(inputResource, &dmlInputResource));
        Ort::ThrowOnError(mImpl->dmlApi->CreateGPUAllocationFromD3DResource(outputResource, &dmlOutputResource));

        // Create memory info for DML
        Ort::MemoryInfo dmlMemoryInfo("DML", OrtDeviceAllocator, 0, OrtMemTypeDefault);
        
        // Input shape: [1, 3, H, W]
        std::vector<int64_t> inputShape = {1, 3, height, width};
        size_t inputElementCount = 3 * width * height;
        
        // Output shape: [1, 3, H*scale, W*scale]
        int outWidth = width * mImpl->modelScaleFactor;
        int outHeight = height * mImpl->modelScaleFactor;
        std::vector<int64_t> outputShape = {1, 3, outHeight, outWidth};
        size_t outputElementCount = 3 * outWidth * outHeight;
        
        // Create tensors from DML allocations
        OrtValue* inputOrtValue = nullptr;
        Ort::ThrowOnError(Ort::GetApi().CreateTensorWithDataAsOrtValue(
            dmlMemoryInfo, dmlInputResource, inputElementCount * sizeof(float),
            inputShape.data(), inputShape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inputOrtValue));
        Ort::Value inputTensor(inputOrtValue);

        OrtValue* outputOrtValue = nullptr;
        Ort::ThrowOnError(Ort::GetApi().CreateTensorWithDataAsOrtValue(
            dmlMemoryInfo, dmlOutputResource, outputElementCount * sizeof(float),
            outputShape.data(), outputShape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &outputOrtValue));
        Ort::Value outputTensor(outputOrtValue);

        // 2. Run Inference using IoBinding (Zero-Copy)
        Ort::IoBinding ioBinding(*mImpl->session);
        ioBinding.BindInput(mImpl->inputName.c_str(), inputTensor);
        ioBinding.BindOutput(mImpl->outputName.c_str(), outputTensor);
        
        mImpl->session->Run(Ort::RunOptions{nullptr}, ioBinding);

        // Cleanup DML allocations
        mImpl->dmlApi->FreeGPUAllocation(dmlInputResource);
        mImpl->dmlApi->FreeGPUAllocation(dmlOutputResource);

        auto totalEnd = std::chrono::high_resolution_clock::now();
        double totalMs = std::chrono::duration<double, std::milli>(totalEnd - totalStart).count();
        log("[DISPATCH_G2G] Total GPU-to-GPU pipeline took %.2f ms (%.1f FPS)", totalMs, 1000.0 / totalMs);

        return true;
    } catch (const Ort::Exception& e) {
        log("!!! ERROR: ONNX Runtime Exception in dispatchGpuToGpu - %s", e.what());
        return false;
    } catch (const std::exception& e) {
        log("!!! ERROR: std::exception in dispatchGpuToGpu - %s", e.what());
        return false;
    }
#else
    return false;
#endif
}

bool UpscalerML::isSupported() const {
#ifdef HAS_ONNX_RUNTIME
    return true;
#else
    return false;
#endif
}

void UpscalerML::updateBindings(int inputW, int inputH) {
    // Resize tensors if resolution changes
}

} // namespace fallout
