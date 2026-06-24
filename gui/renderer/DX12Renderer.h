#pragma once

#include <d3d12.h>
#include <dxgi1_5.h>
#include <windows.h>
#include "../imgui/imgui.h"

static const int APP_NUM_FRAMES_IN_FLIGHT = 2;
static const int APP_NUM_BACK_BUFFERS = 2;
static const int APP_SRV_HEAP_SIZE = 64;

struct FrameContext {
    ID3D12CommandAllocator* CommandAllocator;
    UINT64 FenceValue;
};

struct DescriptorHeapAllocator {
    ID3D12DescriptorHeap* Heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_TYPE HeapType = D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES;
    D3D12_CPU_DESCRIPTOR_HANDLE HeapStartCpu;
    D3D12_GPU_DESCRIPTOR_HANDLE HeapStartGpu;
    UINT HeapHandleIncrement;
    ImVector<int> FreeIndices;

    void Create(ID3D12Device* device, ID3D12DescriptorHeap* heap);
    void Destroy();
    void Alloc(D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu);
    void Free(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu);
};

class DX12Renderer {
public:
    bool init(HWND hwnd);
    void shutdown();
    bool beginFrame();
    void endFrame();
    void onResize(UINT width, UINT height);

    ID3D12Device* getDevice() const { return pd3dDevice; }
    ID3D12CommandQueue* getCommandQueue() const { return pd3dCommandQueue; }
    ID3D12DescriptorHeap* getSrvHeap() const { return pd3dSrvDescHeap; }
    DescriptorHeapAllocator& getSrvHeapAlloc() { return pd3dSrvDescHeapAlloc; }

    void waitForPendingOperations();

private:
    bool createDeviceD3D(HWND hWnd);
    void cleanupDeviceD3D();
    void createRenderTarget();
    void cleanupRenderTarget();
    FrameContext* waitForNextFrameContext();

    FrameContext frameContext[APP_NUM_FRAMES_IN_FLIGHT] = {};
    UINT frameIndex = 0;

    ID3D12Device* pd3dDevice = nullptr;
    ID3D12DescriptorHeap* pd3dRtvDescHeap = nullptr;
    ID3D12DescriptorHeap* pd3dSrvDescHeap = nullptr;
    DescriptorHeapAllocator pd3dSrvDescHeapAlloc;
    ID3D12CommandQueue* pd3dCommandQueue = nullptr;
    ID3D12GraphicsCommandList* pd3dCommandList = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceLastSignaledValue = 0;
    IDXGISwapChain3* pSwapChain = nullptr;
    bool swapChainTearingSupport = false;
    bool swapChainOccluded = false;
    HANDLE hSwapChainWaitableObject = nullptr;
    ID3D12Resource* mainRenderTargetResource[APP_NUM_BACK_BUFFERS] = {};
    D3D12_CPU_DESCRIPTOR_HANDLE mainRenderTargetDescriptor[APP_NUM_BACK_BUFFERS] = {};

    ImVec4 clearColor = ImVec4(0.95f, 0.95f, 0.95f, 1.00f);
};
