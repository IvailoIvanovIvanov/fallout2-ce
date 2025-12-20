#ifndef FALLOUT_GPU_DEVICE_H_
#define FALLOUT_GPU_DEVICE_H_

#include <cstdint>
#include <memory>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12CommandAllocator;
struct ID3D12GraphicsCommandList;

namespace fallout {

/**
 * @brief GPU device context for managing Direct3D 12 resources
 * 
 * This module provides access to the underlying D3D12 device from SDL's renderer
 * for advanced GPU operations like AI upscaling with FSR2.
 */

/**
 * @brief Initialize GPU device context from SDL renderer
 * 
 * Must be called after SDL renderer is created. Extracts D3D12 device
 * and command queue from SDL's internal renderer state.
 * 
 * @return true if initialization succeeded, false otherwise
 */
bool gpuDeviceInit();

/**
 * @brief Shutdown GPU device context
 * 
 * Releases all GPU resources and command list allocators.
 */
void gpuDeviceShutdown();

/**
 * @brief Get the D3D12 device
 * 
 * @return Pointer to ID3D12Device, or nullptr if not initialized
 */
ID3D12Device* gpuDeviceGetDevice();

/**
 * @brief Get the D3D12 command queue
 * 
 * @return Pointer to ID3D12CommandQueue, or nullptr if not initialized
 */
ID3D12CommandQueue* gpuDeviceGetCommandQueue();

/**
 * @brief Create a command allocator for GPU command recording
 * 
 * @return Pointer to ID3D12CommandAllocator, or nullptr on error
 */
ID3D12CommandAllocator* gpuDeviceCreateCommandAllocator();

/**
 * @brief Create a graphics command list for GPU rendering operations
 * 
 * @param allocator Command allocator from gpuDeviceCreateCommandAllocator()
 * @return Pointer to ID3D12GraphicsCommandList, or nullptr on error
 */
ID3D12GraphicsCommandList* gpuDeviceCreateCommandList(ID3D12CommandAllocator* allocator);

/**
 * @brief Execute GPU command list
 * 
 * @param commandList Command list to execute
 * @return true if execution succeeded, false otherwise
 */
bool gpuDeviceExecuteCommandList(ID3D12GraphicsCommandList* commandList);

/**
 * @brief Wait for GPU to complete pending operations
 * 
 * Flushes the command queue and waits for GPU idle state.
 */
void gpuDeviceWaitForGpu();

/**
 * @brief Check if GPU device is properly initialized and ready
 * 
 * @return true if GPU device is available, false otherwise
 */
bool gpuDeviceIsReady();

/**
 * @brief Upload constant buffer data to GPU and get virtual address
 * 
 * Creates an upload heap resource, copies data to it, and returns GPU address.
 * The resource lifetime is managed internally and reused across frames.
 * 
 * @param data Pointer to constant buffer data
 * @param size Size of data in bytes (must be 256-byte aligned for CBV)
 * @param outAddress Output GPU virtual address for SetComputeRootConstantBufferView
 * @return true if upload succeeded, false otherwise
 */
bool gpuUploadConstantBuffer(const void* data, int size, uint64_t* outAddress);

}  // namespace fallout

#endif // FALLOUT_GPU_DEVICE_H_
