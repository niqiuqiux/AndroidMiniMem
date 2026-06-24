#include "DX12Renderer.h"
#include "../imgui/backends/imgui_impl_dx12.h"
#include <iostream>

#ifdef _DEBUG
#define DX12_ENABLE_DEBUG_LAYER
#endif

#ifdef DX12_ENABLE_DEBUG_LAYER
#include <dxgidebug.h>
#pragma comment(lib, "dxguid.lib")
#endif

// DescriptorHeapAllocator implementation
void DescriptorHeapAllocator::Create(ID3D12Device* device, ID3D12DescriptorHeap* heap) {
    IM_ASSERT(Heap == nullptr && FreeIndices.empty());
    Heap = heap;
    D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
    HeapType = desc.Type;
    HeapStartCpu = Heap->GetCPUDescriptorHandleForHeapStart();
    HeapStartGpu = Heap->GetGPUDescriptorHandleForHeapStart();
    HeapHandleIncrement = device->GetDescriptorHandleIncrementSize(HeapType);
    FreeIndices.reserve((int)desc.NumDescriptors);
    for (int n = desc.NumDescriptors; n > 0; n--)
        FreeIndices.push_back(n - 1);
}

void DescriptorHeapAllocator::Destroy() {
    Heap = nullptr;
    FreeIndices.clear();
}

void DescriptorHeapAllocator::Alloc(D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu) {
    IM_ASSERT(FreeIndices.Size > 0);
    int idx = FreeIndices.back();
    FreeIndices.pop_back();
    out_cpu->ptr = HeapStartCpu.ptr + (idx * HeapHandleIncrement);
    out_gpu->ptr = HeapStartGpu.ptr + (idx * HeapHandleIncrement);
}

void DescriptorHeapAllocator::Free(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu) {
    int cpu_idx = (int)((cpu.ptr - HeapStartCpu.ptr) / HeapHandleIncrement);
    int gpu_idx = (int)((gpu.ptr - HeapStartGpu.ptr) / HeapHandleIncrement);
    IM_ASSERT(cpu_idx == gpu_idx);
    FreeIndices.push_back(cpu_idx);
}

// DX12Renderer implementation
bool DX12Renderer::init(HWND hwnd) {
    return createDeviceD3D(hwnd);
}

void DX12Renderer::shutdown() {
    waitForPendingOperations();
    cleanupDeviceD3D();
}

bool DX12Renderer::beginFrame() {
    if ((swapChainOccluded && pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)) {
        ::Sleep(10);
        return false;
    }
    swapChainOccluded = false;
    return true;
}

void DX12Renderer::endFrame() {
    ImGui::Render();

    FrameContext* frameCtx = waitForNextFrameContext();
    UINT backBufferIdx = pSwapChain->GetCurrentBackBufferIndex();
    frameCtx->CommandAllocator->Reset();

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = mainRenderTargetResource[backBufferIdx];
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    pd3dCommandList->Reset(frameCtx->CommandAllocator, nullptr);
    pd3dCommandList->ResourceBarrier(1, &barrier);

    const float clear_color_with_alpha[4] = {
        clearColor.x * clearColor.w, clearColor.y * clearColor.w,
        clearColor.z * clearColor.w, clearColor.w
    };
    pd3dCommandList->ClearRenderTargetView(mainRenderTargetDescriptor[backBufferIdx], clear_color_with_alpha, 0, nullptr);
    pd3dCommandList->OMSetRenderTargets(1, &mainRenderTargetDescriptor[backBufferIdx], FALSE, nullptr);
    pd3dCommandList->SetDescriptorHeaps(1, &pd3dSrvDescHeap);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), pd3dCommandList);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    pd3dCommandList->ResourceBarrier(1, &barrier);
    pd3dCommandList->Close();

    pd3dCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList* const*)&pd3dCommandList);

    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }

    pd3dCommandQueue->Signal(fence, ++fenceLastSignaledValue);
    frameCtx->FenceValue = fenceLastSignaledValue;

    HRESULT hr = pSwapChain->Present(1, 0);
    swapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    frameIndex++;
}

void DX12Renderer::onResize(UINT width, UINT height) {
    if (pd3dDevice != nullptr) {
        cleanupRenderTarget();
        DXGI_SWAP_CHAIN_DESC1 desc = {};
        pSwapChain->GetDesc1(&desc);
        pSwapChain->ResizeBuffers(0, width, height, desc.Format, desc.Flags);
        createRenderTarget();
    }
}

void DX12Renderer::waitForPendingOperations() {
    if (!pd3dCommandQueue || !fence || !fenceEvent)
        return;
    pd3dCommandQueue->Signal(fence, ++fenceLastSignaledValue);
    fence->SetEventOnCompletion(fenceLastSignaledValue, fenceEvent);
    ::WaitForSingleObject(fenceEvent, INFINITE);
}

bool DX12Renderer::createDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC1 sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = APP_NUM_BACK_BUFFERS;
    sd.Width = 0;
    sd.Height = 0;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.Stereo = FALSE;

#ifdef DX12_ENABLE_DEBUG_LAYER
    ID3D12Debug* pdx12Debug = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&pdx12Debug))))
        pdx12Debug->EnableDebugLayer();
#endif

    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    if (D3D12CreateDevice(nullptr, featureLevel, IID_PPV_ARGS(&pd3dDevice)) != S_OK)
        return false;

#ifdef DX12_ENABLE_DEBUG_LAYER
    if (pdx12Debug != nullptr) {
        ID3D12InfoQueue* pInfoQueue = nullptr;
        pd3dDevice->QueryInterface(IID_PPV_ARGS(&pInfoQueue));
        pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
        pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
        pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);
        const int D3D12_MESSAGE_ID_FENCE_ZERO_WAIT_ = 1424;
        D3D12_MESSAGE_ID disabledMessages[] = { (D3D12_MESSAGE_ID)D3D12_MESSAGE_ID_FENCE_ZERO_WAIT_ };
        D3D12_INFO_QUEUE_FILTER filter = {};
        filter.DenyList.NumIDs = 1;
        filter.DenyList.pIDList = disabledMessages;
        pInfoQueue->AddStorageFilterEntries(&filter);
        pInfoQueue->Release();
        pdx12Debug->Release();
    }
#endif

    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = APP_NUM_BACK_BUFFERS;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        desc.NodeMask = 1;
        if (pd3dDevice->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&pd3dRtvDescHeap)) != S_OK)
            return false;
        SIZE_T rtvDescriptorSize = pd3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = pd3dRtvDescHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < APP_NUM_BACK_BUFFERS; i++) {
            mainRenderTargetDescriptor[i] = rtvHandle;
            rtvHandle.ptr += rtvDescriptorSize;
        }
    }

    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = APP_SRV_HEAP_SIZE;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (pd3dDevice->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&pd3dSrvDescHeap)) != S_OK)
            return false;
        pd3dSrvDescHeapAlloc.Create(pd3dDevice, pd3dSrvDescHeap);
    }

    {
        D3D12_COMMAND_QUEUE_DESC desc = {};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        desc.NodeMask = 1;
        if (pd3dDevice->CreateCommandQueue(&desc, IID_PPV_ARGS(&pd3dCommandQueue)) != S_OK)
            return false;
    }

    for (UINT i = 0; i < APP_NUM_FRAMES_IN_FLIGHT; i++)
        if (pd3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frameContext[i].CommandAllocator)) != S_OK)
            return false;

    if (pd3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frameContext[0].CommandAllocator, nullptr, IID_PPV_ARGS(&pd3dCommandList)) != S_OK ||
        pd3dCommandList->Close() != S_OK)
        return false;

    if (pd3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)) != S_OK)
        return false;

    fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (fenceEvent == nullptr)
        return false;

    {
        IDXGIFactory5* dxgiFactory = nullptr;
        IDXGISwapChain1* swapChain1 = nullptr;
        if (CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)) != S_OK)
            return false;
        BOOL allow_tearing = FALSE;
        dxgiFactory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing, sizeof(allow_tearing));
        swapChainTearingSupport = (allow_tearing == TRUE);
        if (swapChainTearingSupport)
            sd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        if (dxgiFactory->CreateSwapChainForHwnd(pd3dCommandQueue, hWnd, &sd, nullptr, nullptr, &swapChain1) != S_OK)
            return false;
        if (swapChain1->QueryInterface(IID_PPV_ARGS(&pSwapChain)) != S_OK)
            return false;
        if (swapChainTearingSupport)
            dxgiFactory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER);
        swapChain1->Release();
        dxgiFactory->Release();
        pSwapChain->SetMaximumFrameLatency(APP_NUM_BACK_BUFFERS);
        hSwapChainWaitableObject = pSwapChain->GetFrameLatencyWaitableObject();
    }

    createRenderTarget();
    return true;
}

void DX12Renderer::cleanupDeviceD3D() {
    cleanupRenderTarget();
    if (pSwapChain) { pSwapChain->SetFullscreenState(false, nullptr); pSwapChain->Release(); pSwapChain = nullptr; }
    if (hSwapChainWaitableObject != nullptr) { CloseHandle(hSwapChainWaitableObject); }
    for (UINT i = 0; i < APP_NUM_FRAMES_IN_FLIGHT; i++)
        if (frameContext[i].CommandAllocator) { frameContext[i].CommandAllocator->Release(); frameContext[i].CommandAllocator = nullptr; }
    if (pd3dCommandQueue) { pd3dCommandQueue->Release(); pd3dCommandQueue = nullptr; }
    if (pd3dCommandList) { pd3dCommandList->Release(); pd3dCommandList = nullptr; }
    if (pd3dRtvDescHeap) { pd3dRtvDescHeap->Release(); pd3dRtvDescHeap = nullptr; }
    if (pd3dSrvDescHeap) { pd3dSrvDescHeap->Release(); pd3dSrvDescHeap = nullptr; }
    if (fence) { fence->Release(); fence = nullptr; }
    if (fenceEvent) { CloseHandle(fenceEvent); fenceEvent = nullptr; }
    if (pd3dDevice) { pd3dDevice->Release(); pd3dDevice = nullptr; }

#ifdef DX12_ENABLE_DEBUG_LAYER
    IDXGIDebug1* pDebug = nullptr;
    if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&pDebug)))) {
        pDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_SUMMARY);
        pDebug->Release();
    }
#endif
}

void DX12Renderer::createRenderTarget() {
    for (UINT i = 0; i < APP_NUM_BACK_BUFFERS; i++) {
        ID3D12Resource* pBackBuffer = nullptr;
        pSwapChain->GetBuffer(i, IID_PPV_ARGS(&pBackBuffer));
        pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, mainRenderTargetDescriptor[i]);
        mainRenderTargetResource[i] = pBackBuffer;
    }
}

void DX12Renderer::cleanupRenderTarget() {
    waitForPendingOperations();
    for (UINT i = 0; i < APP_NUM_BACK_BUFFERS; i++)
        if (mainRenderTargetResource[i]) { mainRenderTargetResource[i]->Release(); mainRenderTargetResource[i] = nullptr; }
}

FrameContext* DX12Renderer::waitForNextFrameContext() {
    FrameContext* frame_context = &frameContext[frameIndex % APP_NUM_FRAMES_IN_FLIGHT];
    if (fence->GetCompletedValue() < frame_context->FenceValue) {
        fence->SetEventOnCompletion(frame_context->FenceValue, fenceEvent);
        HANDLE waitableObjects[] = { hSwapChainWaitableObject, fenceEvent };
        ::WaitForMultipleObjects(2, waitableObjects, TRUE, INFINITE);
    } else {
        ::WaitForSingleObject(hSwapChainWaitableObject, INFINITE);
    }
    return frame_context;
}
