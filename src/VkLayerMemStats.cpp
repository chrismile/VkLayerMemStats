/*
 * BSD 3-Clause License
 *
 * Copyright (c) 2026, Christoph Neuhauser
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <iostream>
#include <map>
#include <unordered_map>
#include <chrono>
#include <fstream>
#include <atomic>
#include <memory>
#include <algorithm>
#include <mutex>
#include <cstring>
#include <cctype>
#include <cinttypes>
#include <vulkan/vk_layer.h>
#include <vulkan/utility/vk_dispatch_table.h>
#include <vulkan/layer/vk_layer_settings.h>
#include <vulkan/layer/vk_layer_settings.hpp>

#ifdef HOOK_MALLOC
extern "C" {
#include <hashtable/hashtable.h>
}

#ifdef _WIN32
#include <windows.h>
#ifdef USE_DETOURS
// https://github.com/microsoft/Detours
#include <detours.h>
#elif defined(USE_MINHOOK)
// https://github.com/TsudaKageyu/minhook
#include <MinHook.h>
#endif
#else // !defined(_WIN32)
#include <cstdio>
#include <dlfcn.h>
#include <unistd.h>
#endif
#endif

#ifdef _WIN32
#define VK_LAYER_EXPORT extern "C" __declspec(dllexport)
#else
#define VK_LAYER_EXPORT extern "C"
#endif

static const char* const VK_LAYER_NAME = "VK_LAYER_CHRISMILE_memstats";

/// Retrieves the dispatch table pointer to be used as the key for the instance and device maps.
template <typename DispatchT>
void* getDispatchKey(DispatchT inst) {
    return *reinterpret_cast<void**>(inst);
}

/// Stores active memory allocations for a Vulkan device.
struct DeviceMemInfo {
    std::unordered_map<VkDeviceMemory, VkDeviceSize> allocatedMemoryBlocks;
};

// Instance and device dispatch tables and settings.
static std::map<void*, VkuInstanceDispatchTable> instanceDispatchTables;
static std::map<void*, VkuDeviceDispatchTable> deviceDispatchTables;
static std::map<void*, std::shared_ptr<DeviceMemInfo>> deviceMemInfos;

#ifdef HOOK_MALLOC
static HashTable* cpuAllocations = nullptr;
#endif

/**
 * Global lock for access to the maps above.
 * Theoretically, we could switch to vku::concurrent::unordered_map, but this is likely not a performance bottleneck.
 */
static std::mutex globalMutex;
static std::mutex globalFileMutex;
typedef std::lock_guard<std::mutex> scoped_lock;
// Cannot use static; https://stackoverflow.com/questions/12463718/linux-equivalent-of-dllmain
std::chrono::high_resolution_clock::time_point MemStatsLayer_startTime;
FILE* MemStatsLayer_outFile;

#if !defined(HOOK_MALLOC) || !defined(_WIN32)
std::atomic<int> MemStatsLayer_refcount = 0;
#endif

#ifdef HOOK_MALLOC
static bool hooked_alloc = false;
#endif

#if !defined(_WIN32) && defined(HOOK_MALLOC)
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pid_t MemStatsLayer_pid = 0;
#endif

uint64_t getTimeStamp() {
    auto now = std::chrono::high_resolution_clock::now();
    auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(now - MemStatsLayer_startTime).count();
    static_assert(sizeof(nanoseconds) == sizeof(uint64_t));
    return uint64_t(nanoseconds);
}

enum class AllocType {
    CPU = 0, GPU = 1
};

static void addAllocation(AllocType allocType, uint64_t memSize, void* ptr, uint32_t memoryTypeIndex) {
    scoped_lock l(globalFileMutex);
    //if (allocType == AllocType::CPU) {
    //    fprintf(MemStatsLayer_outFile, "alloc,%" PRIu64 ",%d,%" PRIu64 ",%p\n",
    //            getTimeStamp(), int(allocType), memSize, ptr);
    //} else {
    //    fprintf(MemStatsLayer_outFile, "alloc,%" PRIu64 ",%d,%" PRIu64 ",%p,%u\n",
    //            getTimeStamp(), int(allocType), memSize, ptr, memoryTypeIndex);
    //}
    //fflush(MemStatsLayer_outFile);
}

static void removeAllocation(AllocType allocType, uint64_t memSize, void* ptr) {
    scoped_lock l(globalFileMutex);
    //fprintf(MemStatsLayer_outFile, "free,%" PRIu64 ",%d,%" PRIu64 ",%p\n",
    //        getTimeStamp(), int(allocType), memSize, ptr);
    //fflush(MemStatsLayer_outFile);
}


VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL MemStatsLayer_EnumerateInstanceLayerProperties(
        uint32_t* pPropertyCount, VkLayerProperties* pProperties) {
    if (pPropertyCount) {
        *pPropertyCount = 1;
    }

    if (pProperties) {
        strcpy(pProperties->layerName, VK_LAYER_NAME);
        strcpy(pProperties->description, "Memory usage statistics layer - https://github.com/chrismile/VkLayerMemStats");
        pProperties->implementationVersion = 1;
        pProperties->specVersion = VK_MAKE_API_VERSION(0, 1, 3, 234);
    }

    return VK_SUCCESS;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL MemStatsLayer_EnumerateInstanceExtensionProperties(
        const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties) {
    if (!pLayerName || strcmp(pLayerName, VK_LAYER_NAME) != 0) {
        return VK_ERROR_LAYER_NOT_PRESENT;
    }

    if (pPropertyCount) {
        *pPropertyCount = 0;
    }
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL MemStatsLayer_EnumerateDeviceLayerProperties(
        VkPhysicalDevice physicalDevice, uint32_t* pPropertyCount, VkLayerProperties* pProperties) {
    return MemStatsLayer_EnumerateInstanceLayerProperties(pPropertyCount, pProperties);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL MemStatsLayer_EnumerateDeviceExtensionProperties(
        VkPhysicalDevice physicalDevice, const char* pLayerName,
        uint32_t* pPropertyCount, VkExtensionProperties* pProperties) {
    if (!pLayerName || strcmp(pLayerName, VK_LAYER_NAME) != 0) {
        if (physicalDevice == VK_NULL_HANDLE) {
            return VK_SUCCESS;
        }

        scoped_lock l(globalMutex);
        return instanceDispatchTables[getDispatchKey(physicalDevice)].EnumerateDeviceExtensionProperties(
                physicalDevice, pLayerName, pPropertyCount, pProperties);
    }

    if (pPropertyCount) {
        *pPropertyCount = 0;
    }
    return VK_SUCCESS;
}


VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL MemStatsLayer_CreateInstance(
        const VkInstanceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkInstance* pInstance) {
    auto* createInfoChain = (VkLayerInstanceCreateInfo*)pCreateInfo->pNext;
    while (createInfoChain && (createInfoChain->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO
            || createInfoChain->function != VK_LAYER_LINK_INFO)) {
        createInfoChain = (VkLayerInstanceCreateInfo*)createInfoChain->pNext;
    }
    if (!createInfoChain || !createInfoChain->u.pLayerInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    auto pGetInstanceProcAddr = createInfoChain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    auto pCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(pGetInstanceProcAddr(nullptr, "vkCreateInstance"));
    if (!pCreateInstance) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Advance the chain for passing to the next layer and create the instance.
    createInfoChain->u.pLayerInfo = createInfoChain->u.pLayerInfo->pNext;
    VkResult result = pCreateInstance(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS) {
        return result;
    }

    // Populate the dispatch table using the functions of the next layer.
    VkuInstanceDispatchTable dispatchTable;
    dispatchTable.GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)pGetInstanceProcAddr(*pInstance, "vkGetInstanceProcAddr");
    dispatchTable.DestroyInstance = (PFN_vkDestroyInstance)pGetInstanceProcAddr(*pInstance, "vkDestroyInstance");
    dispatchTable.EnumerateDeviceExtensionProperties = (PFN_vkEnumerateDeviceExtensionProperties)pGetInstanceProcAddr(
            *pInstance, "vkEnumerateDeviceExtensionProperties");

    {
        scoped_lock l(globalMutex);
        instanceDispatchTables[getDispatchKey(*pInstance)] = dispatchTable;
#ifndef HOOK_MALLOC
        if (MemStatsLayer_refcount == 0) {
            MemStatsLayer_outFile = fopen("memstats.csv", "w");
            MemStatsLayer_startTime = std::chrono::high_resolution_clock::now();
        }
        ++MemStatsLayer_refcount;
#endif
    }
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_DestroyInstance(
        VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    scoped_lock l(globalMutex);
    instanceDispatchTables.erase(getDispatchKey(instance));
#ifndef HOOK_MALLOC
    --MemStatsLayer_refcount;
    if (MemStatsLayer_refcount == 0) {
        fclose(MemStatsLayer_outFile);
        MemStatsLayer_outFile = nullptr;
    }
#endif
}


VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL MemStatsLayer_CreateDevice(
        VkPhysicalDevice physicalDevice,
        const VkDeviceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDevice* pDevice) {
    auto* createInfoChain = (VkLayerDeviceCreateInfo*)pCreateInfo->pNext;
    while (createInfoChain && (createInfoChain->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO
            || createInfoChain->function != VK_LAYER_LINK_INFO)) {
        createInfoChain = (VkLayerDeviceCreateInfo*)createInfoChain->pNext;
    }

    if (!createInfoChain || !createInfoChain->u.pLayerInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr pGetInstanceProcAddr = createInfoChain->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr pGetDeviceProcAddr = createInfoChain->u.pLayerInfo->pfnNextGetDeviceProcAddr;

    // Advance the chain for passing to the next layer and create the device.
    createInfoChain->u.pLayerInfo = createInfoChain->u.pLayerInfo->pNext;
    auto pCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(pGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateDevice"));
    VkResult result = pCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (result != VK_SUCCESS) {
        return result;
    }

    // Populate the dispatch table using the functions of the next layer.
    VkuDeviceDispatchTable dispatchTable;
    dispatchTable.GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)pGetDeviceProcAddr(*pDevice, "vkGetDeviceProcAddr");
    dispatchTable.DestroyDevice = (PFN_vkDestroyDevice)pGetDeviceProcAddr(*pDevice, "vkDestroyDevice");
    dispatchTable.AllocateMemory = (PFN_vkAllocateMemory)pGetDeviceProcAddr(*pDevice, "vkAllocateMemory");
    dispatchTable.FreeMemory = (PFN_vkFreeMemory)pGetDeviceProcAddr(*pDevice, "vkFreeMemory");
    dispatchTable.QueueSubmit = (PFN_vkQueueSubmit)pGetDeviceProcAddr(*pDevice, "vkQueueSubmit");
    dispatchTable.AcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)pGetDeviceProcAddr(*pDevice, "vkAcquireNextImageKHR");

    // Write memory statistics.
    VkPhysicalDeviceProperties deviceProperties{};
    VkPhysicalDeviceMemoryProperties deviceMemoryProperties{};
    auto pGetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)pGetInstanceProcAddr(
            VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
    auto pGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)pGetInstanceProcAddr(
            VK_NULL_HANDLE, "vkGetPhysicalDeviceMemoryProperties");
    pGetPhysicalDeviceProperties(physicalDevice, &deviceProperties);
    pGetPhysicalDeviceMemoryProperties(physicalDevice, &deviceMemoryProperties);
#if !defined(_WIN32) && defined(HOOK_MALLOC)
    pthread_mutex_lock(&mutex);
    hooked_alloc = true;
#endif
#if !defined(_WIN32) && defined(HOOK_MALLOC)
    if (getpid() == MemStatsLayer_pid)
#endif
    {
        scoped_lock l(globalFileMutex);
        fprintf(MemStatsLayer_outFile, "devinfo,%" PRIu64 ",%u,%s\n",
                getTimeStamp(), uint32_t(deviceProperties.deviceType), deviceProperties.deviceName);
        for (uint32_t heapIdx = 0; heapIdx < deviceMemoryProperties.memoryHeapCount; heapIdx++) {
            auto& memHeap = deviceMemoryProperties.memoryHeaps[heapIdx];
            fprintf(MemStatsLayer_outFile, "memheap,%" PRIu64 ",%u,%" PRIu64 ",%u\n",
                    getTimeStamp(), heapIdx, uint64_t(memHeap.size), uint32_t(memHeap.flags));
        }
        for (uint32_t memoryTypeIdx = 0; memoryTypeIdx < deviceMemoryProperties.memoryTypeCount; memoryTypeIdx++) {
            auto& memType = deviceMemoryProperties.memoryTypes[memoryTypeIdx];
            fprintf(MemStatsLayer_outFile, "memtype,%" PRIu64 ",%u,%u,%u\n",
                    getTimeStamp(), memoryTypeIdx, memType.heapIndex, uint32_t(memType.propertyFlags));
        }
    }
#if !defined(_WIN32) && defined(HOOK_MALLOC)
    hooked_alloc = false;
    pthread_mutex_unlock(&mutex);
#endif

    {
        scoped_lock l(globalMutex);
        deviceDispatchTables[getDispatchKey(*pDevice)] = dispatchTable;
        deviceMemInfos[getDispatchKey(*pDevice)] = std::make_shared<DeviceMemInfo>();
    }
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_DestroyDevice(
        VkDevice device, const VkAllocationCallbacks* pAllocator) {
    scoped_lock l(globalMutex);
    deviceDispatchTables.erase(getDispatchKey(device));
    deviceMemInfos.erase(getDispatchKey(device));
}


VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL MemStatsLayer_AllocateMemory(
        VkDevice device, const VkMemoryAllocateInfo* pAllocateInfo, const VkAllocationCallbacks* pAllocator,
        VkDeviceMemory* pMemory) {
    scoped_lock l(globalMutex);

    VkResult res = deviceDispatchTables[getDispatchKey(device)].AllocateMemory(device, pAllocateInfo, pAllocator, pMemory);
    if (res != VK_SUCCESS) {
        return res;
    }

#if !defined(_WIN32) && defined(HOOK_MALLOC)
    pthread_mutex_lock(&mutex);
    hooked_alloc = true;
#endif
#if !defined(_WIN32) && defined(HOOK_MALLOC)
    if (getpid() == MemStatsLayer_pid)
#endif
    {
        addAllocation(AllocType::GPU, pAllocateInfo->allocationSize, *pMemory, pAllocateInfo->memoryTypeIndex);
    }
    auto& allocatedMemoryBlocks = deviceMemInfos[getDispatchKey(device)]->allocatedMemoryBlocks;
    allocatedMemoryBlocks.insert(std::make_pair(*pMemory, pAllocateInfo->allocationSize));
#if !defined(_WIN32) && defined(HOOK_MALLOC)
    hooked_alloc = false;
    pthread_mutex_unlock(&mutex);
#endif

    return res;
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_FreeMemory(
        VkDevice device, const VkDeviceMemory memory, const VkAllocationCallbacks* pAllocator) {
    scoped_lock l(globalMutex);

#if !defined(_WIN32) && defined(HOOK_MALLOC)
    pthread_mutex_lock(&mutex);
    hooked_alloc = true;
#endif
    auto& allocatedMemoryBlocks = deviceMemInfos[getDispatchKey(device)]->allocatedMemoryBlocks;
    auto it = allocatedMemoryBlocks.find(memory);
    if (it == allocatedMemoryBlocks.end()) {
        return;
    }
#if !defined(_WIN32) && defined(HOOK_MALLOC)
    if (getpid() == MemStatsLayer_pid)
#endif
    {
        removeAllocation(AllocType::GPU, it->second, memory);
    }
    allocatedMemoryBlocks.erase(it);
    it = {};
#if !defined(_WIN32) && defined(HOOK_MALLOC)
    hooked_alloc = false;
    pthread_mutex_unlock(&mutex);
#endif

    deviceDispatchTables[getDispatchKey(device)].FreeMemory(device, memory, pAllocator);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL MemStatsLayer_QueueSubmit(
        VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence) {
    scoped_lock l(globalMutex);

    VkResult res = deviceDispatchTables[getDispatchKey(queue)].QueueSubmit(queue, submitCount, pSubmits, fence);
    if (res != VK_SUCCESS) {
        return res;
    }

#if !defined(_WIN32) && defined(HOOK_MALLOC)
    pthread_mutex_lock(&mutex);
    hooked_alloc = true;
#endif
#if !defined(_WIN32) && defined(HOOK_MALLOC)
    if (getpid() == MemStatsLayer_pid)
#endif
    {
        scoped_lock l(globalFileMutex);
        fprintf(MemStatsLayer_outFile, "submit,%" PRIu64 "\n", getTimeStamp());
    }
#if !defined(_WIN32) && defined(HOOK_MALLOC)
    hooked_alloc = false;
    pthread_mutex_unlock(&mutex);
#endif

    return res;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL MemStatsLayer_AcquireNextImageKHR(
        VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence,
        uint32_t* pImageIndex) {
    scoped_lock l(globalMutex);

    VkResult res = deviceDispatchTables[getDispatchKey(device)].AcquireNextImageKHR(
            device, swapchain, timeout, semaphore, fence, pImageIndex);
    if (res != VK_SUCCESS) {
        return res;
    }

#if !defined(_WIN32) && defined(HOOK_MALLOC)
    pthread_mutex_lock(&mutex);
    hooked_alloc = true;
#endif
#if !defined(_WIN32) && defined(HOOK_MALLOC)
    if (getpid() == MemStatsLayer_pid)
#endif
    {
        scoped_lock l(globalFileMutex);
        fprintf(MemStatsLayer_outFile, "acquire_next_image,%" PRIu64 "\n", getTimeStamp());
    }
#if !defined(_WIN32) && defined(HOOK_MALLOC)
    hooked_alloc = false;
    pthread_mutex_unlock(&mutex);
#endif

    return res;
}


#define GETPROCADDR_VK(func) if (strcmp(pName, "vk" #func) == 0) { return (PFN_vkVoidFunction)&MemStatsLayer_##func; }

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL MemStatsLayer_GetDeviceProcAddr(VkDevice device, const char* pName) {
    GETPROCADDR_VK(GetDeviceProcAddr);
    GETPROCADDR_VK(EnumerateDeviceLayerProperties);
    GETPROCADDR_VK(EnumerateDeviceExtensionProperties);
    GETPROCADDR_VK(CreateDevice);
    GETPROCADDR_VK(DestroyDevice);
    GETPROCADDR_VK(AllocateMemory);
    GETPROCADDR_VK(FreeMemory);
    GETPROCADDR_VK(QueueSubmit);
    GETPROCADDR_VK(AcquireNextImageKHR);

    {
        scoped_lock l(globalMutex);
        return deviceDispatchTables[getDispatchKey(device)].GetDeviceProcAddr(device, pName);
    }
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL MemStatsLayer_GetInstanceProcAddr(VkInstance instance, const char* pName) {
    GETPROCADDR_VK(GetInstanceProcAddr);
    GETPROCADDR_VK(EnumerateInstanceLayerProperties);
    GETPROCADDR_VK(EnumerateInstanceExtensionProperties);
    GETPROCADDR_VK(CreateInstance);
    GETPROCADDR_VK(DestroyInstance);

    GETPROCADDR_VK(GetDeviceProcAddr);
    GETPROCADDR_VK(EnumerateDeviceLayerProperties);
    GETPROCADDR_VK(EnumerateDeviceExtensionProperties);
    GETPROCADDR_VK(CreateDevice);
    GETPROCADDR_VK(DestroyDevice);
    GETPROCADDR_VK(AllocateMemory);
    GETPROCADDR_VK(FreeMemory);
    GETPROCADDR_VK(QueueSubmit);
    GETPROCADDR_VK(AcquireNextImageKHR);

    {
        scoped_lock l(globalMutex);
        return instanceDispatchTables[getDispatchKey(instance)].GetInstanceProcAddr(instance, pName);
    }
}


#if defined(_WIN32) && defined(HOOK_MALLOC)

static std::mutex globalWinAllocMutex;

extern "C" {

LPVOID (WINAPI* Real_HeapAlloc)(HANDLE hHeap, DWORD dwFlags, DWORD_PTR dwBytes) = HeapAlloc;
BOOL (WINAPI* Real_HeapFree)(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem) = HeapFree;

LPVOID WINAPI MemStatsLayer_HeapAlloc(HANDLE hHeap, DWORD dwFlags, DWORD_PTR dwBytes) {
    LPVOID ptr = Real_HeapAlloc(hHeap, dwFlags, dwBytes);
    if (!ptr || hooked_alloc) {
        return ptr;
    }

    scoped_lock l(globalWinAllocMutex);
    hooked_alloc = true;
    ht_insert(cpuAllocations, &ptr, &dwBytes);
    addAllocation(AllocType::CPU, uint64_t(dwBytes), (void*)ptr, 0);
    hooked_alloc = false;

    return ptr;
}

BOOL WINAPI MemStatsLayer_HeapFree(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem) {
    BOOL retVal = Real_HeapFree(hHeap, dwFlags, lpMem);
    if (!retVal || hooked_alloc) {
        return retVal;
    }

    scoped_lock l(globalWinAllocMutex);
    hooked_alloc = true;
    if (ht_contains(cpuAllocations, &lpMem)) {
        size_t size = HT_LOOKUP_AS(size_t, cpuAllocations, &lpMem);
        removeAllocation(AllocType::CPU, size, (void*)lpMem);
        ht_erase(cpuAllocations, &lpMem);
    }
    hooked_alloc = false;

    return retVal;
}

}

// https://github.com/microsoft/Detours
BOOL WINAPI DllMain(HINSTANCE hModule, DWORD dwReason, LPVOID reserved) {
#ifdef USE_DETOURS
    if (DetourIsHelperProcess()) {
        return TRUE;
    }

    if (dwReason == DLL_PROCESS_ATTACH) {
        cpuAllocations = reinterpret_cast<HashTable*>(malloc(sizeof(HashTable)));
        ht_setup(cpuAllocations, sizeof(size_t), sizeof(void*), 4096);
        MemStatsLayer_outFile = fopen("memstats.csv", "w");
        MemStatsLayer_startTime = std::chrono::high_resolution_clock::now();

        DetourRestoreAfterWith();
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)HeapAlloc, MemStatsLayer_HeapAlloc);
        DetourAttach(&(PVOID&)HeapFree, MemStatsLayer_HeapFree);
        DetourTransactionCommit();
    } else if (dwReason == DLL_PROCESS_DETACH) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)HeapAlloc, MemStatsLayer_HeapAlloc);
        DetourDetach(&(PVOID&)HeapFree, MemStatsLayer_HeapFree);
        DetourTransactionCommit();

        ht_clear(cpuAllocations);
        ht_destroy(cpuAllocations);
        fclose(MemStatsLayer_outFile);
    }
#elif defined(USE_MINHOOK)
    if (dwReason == DLL_PROCESS_ATTACH) {
        cpuAllocations = reinterpret_cast<HashTable*>(malloc(sizeof(HashTable)));
        ht_setup(cpuAllocations, sizeof(size_t), sizeof(void*), 4096);
        MemStatsLayer_outFile = fopen("memstats.csv", "w");
        MemStatsLayer_startTime = std::chrono::high_resolution_clock::now();

        MH_Initialize();
        MH_CreateHook(&HeapAlloc, &MemStatsLayer_HeapAlloc, reinterpret_cast<LPVOID*>(&Real_HeapAlloc));
        MH_CreateHook(&HeapFree, &MemStatsLayer_HeapFree, reinterpret_cast<LPVOID*>(&Real_HeapFree));
        MH_EnableHook(&HeapFree);
        MH_EnableHook(&HeapAlloc);
    } else if (dwReason == DLL_PROCESS_DETACH) {
        MH_DisableHook(&HeapAlloc);
        MH_DisableHook(&HeapFree);
        MH_Uninitialize();

        ht_clear(cpuAllocations);
        ht_destroy(cpuAllocations);
        fclose(MemStatsLayer_outFile);
    }
#endif
    return TRUE;
}

#endif


#if !defined(_WIN32) && defined(HOOK_MALLOC)

/*
 * https://sourceware.org/glibc/manual/2.43/html_mono/libc.html#Replacing-malloc
 * Minimum set of functions: malloc, free, calloc, realloc
 */

typedef void* (*malloc_type)(size_t);
typedef void (*free_type)(void* ptr);
typedef void* (*calloc_type)(size_t num, size_t size);
typedef void* (*realloc_type)(void* ptr, size_t new_size);
static malloc_type real_malloc = nullptr;
static free_type real_free = nullptr;
static calloc_type real_calloc = nullptr;
static realloc_type real_realloc = nullptr;

#define CHECK_DLSYM(f) \
    if (!real_##f) { \
        fprintf(stderr, "Error in dlsym: %s\n", dlerror()); \
    }

static void MemStatsLayer_memtrace_init() {
    pthread_mutex_lock(&mutex);
    hooked_alloc = true;

    real_malloc = (malloc_type)dlsym(RTLD_NEXT, "malloc");
    CHECK_DLSYM(malloc);
    real_free = (free_type)dlsym(RTLD_NEXT, "free");
    CHECK_DLSYM(free);
    real_calloc = (calloc_type)dlsym(RTLD_NEXT, "calloc");
    CHECK_DLSYM(calloc);
    real_realloc = (realloc_type)dlsym(RTLD_NEXT, "realloc");
    CHECK_DLSYM(realloc);

    if (MemStatsLayer_pid == 0) {
        MemStatsLayer_pid = getpid();
    }

    MemStatsLayer_outFile = fopen("memstats.csv", "w");
    MemStatsLayer_startTime = std::chrono::high_resolution_clock::now();
    cpuAllocations = reinterpret_cast<HashTable*>(malloc(sizeof(HashTable)));
    ht_setup(cpuAllocations, sizeof(size_t), sizeof(void*), 4096);

    hooked_alloc = false;
    pthread_mutex_unlock(&mutex);
}

__attribute__((constructor)) void MemStatsLayer_OnLoad() {
    //MemStatsLayer_memtrace_init(); // Didn't work...
    /**
     * Environment variables such as LD_PRELOAD are inherited by child processes.
     * As we are not attaching the pid to the file name, we must avoid child processes writing to the same file and
     * causing data races.
     */
    unsetenv("LD_PRELOAD");
    ++MemStatsLayer_refcount;
}

__attribute__((destructor)) void MemStatsLayer_OnFree() {
    --MemStatsLayer_refcount;
    if (MemStatsLayer_refcount > 0) {
        return;
    }
    if (MemStatsLayer_outFile) {
        pthread_mutex_lock(&mutex);
        hooked_alloc = true;

        ht_clear(cpuAllocations);
        ht_destroy(cpuAllocations);
        free(cpuAllocations);
        cpuAllocations = nullptr;
        fclose(MemStatsLayer_outFile);
        MemStatsLayer_outFile = nullptr;

        hooked_alloc = false;
        pthread_mutex_unlock(&mutex);
    }
}

extern "C" {

void* malloc(size_t size) {
    if (!real_malloc) {
        MemStatsLayer_memtrace_init();
    }
    void* ptr = real_malloc(size);
    if (hooked_alloc || !cpuAllocations || MemStatsLayer_pid != getpid()) {
        return ptr;
    }

    pthread_mutex_lock(&mutex);
    hooked_alloc = true;
    ht_insert(cpuAllocations, &ptr, &size);
    addAllocation(AllocType::CPU, uint64_t(size), ptr, 0);
    hooked_alloc = false;
    pthread_mutex_unlock(&mutex);

    return ptr;
}

void free(void* ptr) {
    if (!real_free) {
        MemStatsLayer_memtrace_init();
    }
    real_free(ptr);
    if (hooked_alloc || !cpuAllocations || MemStatsLayer_pid != getpid()) {
        return;
    }

    pthread_mutex_lock(&mutex);
    hooked_alloc = true;
    if (ht_contains(cpuAllocations, &ptr)) {
        size_t size = HT_LOOKUP_AS(size_t, cpuAllocations, &ptr);
        removeAllocation(AllocType::CPU, size, ptr);
        ht_erase(cpuAllocations, &ptr);
    }
    hooked_alloc = false;
    pthread_mutex_unlock(&mutex);
}

void* calloc(size_t num, size_t size) {
    if (!real_calloc) {
        MemStatsLayer_memtrace_init();
    }
    void* ptr = real_calloc(num, size);
    if (hooked_alloc || !cpuAllocations || MemStatsLayer_pid != getpid()) {
        return ptr;
    }

    pthread_mutex_lock(&mutex);
    hooked_alloc = true;
    size_t total_size = num * size;
    ht_insert(cpuAllocations, &ptr, &total_size);
    addAllocation(AllocType::CPU, uint64_t(num * size), ptr, 0);
    hooked_alloc = false;
    pthread_mutex_unlock(&mutex);

    return ptr;
}

void* realloc(void* ptr, size_t new_size) {
    if (!real_realloc) {
        MemStatsLayer_memtrace_init();
    }
    void* new_ptr = real_realloc(ptr, new_size);
    if (hooked_alloc || !cpuAllocations || MemStatsLayer_pid != getpid()) {
        return new_ptr;
    }

    pthread_mutex_lock(&mutex);
    hooked_alloc = true;
    if (ht_contains(cpuAllocations, &ptr)) {
        size_t size = HT_LOOKUP_AS(size_t, cpuAllocations, &ptr);
        removeAllocation(AllocType::CPU, size, ptr);
        ht_erase(cpuAllocations, &ptr);
    }
    ht_insert(cpuAllocations, &new_ptr, &new_size);
    addAllocation(AllocType::CPU, uint64_t(new_size), new_ptr, 0);
    hooked_alloc = false;
    pthread_mutex_unlock(&mutex);

    return new_ptr;
}

}

#endif
