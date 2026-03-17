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
#include <fstream>
#include <atomic>
#include <memory>
#include <algorithm>
#include <mutex>
#include <cstring>
#include <cctype>
#include <vulkan/vk_layer.h>
#include <vulkan/utility/vk_dispatch_table.h>
#include <vulkan/layer/vk_layer_settings.h>
#include <vulkan/layer/vk_layer_settings.hpp>

#ifdef _WIN32
#include <windows.h>
#ifdef USE_DETOURS
// https://github.com/microsoft/Detours
#include <detours.h>
#elif defined(USE_MINHOOK)
// https://github.com/TsudaKageyu/minhook
#include <MinHook.h>
#pragma comment(lib, "libMinHook.x64.lib")
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
static std::unordered_map<void*, uint64_t> cpuAllocations;

/**
 * Global lock for access to the maps above.
 * Theoretically, we could switch to vku::concurrent::unordered_map, but this is likely not a performance bottleneck.
 */
static std::mutex globalMutex;
typedef std::lock_guard<std::mutex> scoped_lock;
// Cannot use static; https://stackoverflow.com/questions/12463718/linux-equivalent-of-dllmain
std::ofstream* MemStatsLayer_outFile;

enum class AllocType {
    CPU = 0, GPU = 1
};

static void addAllocation(AllocType allocType, uint64_t memSize, void* ptr, uint32_t memoryTypeIndex) {
    *MemStatsLayer_outFile << "alloc,";
    *MemStatsLayer_outFile << std::to_string(int(allocType));
    *MemStatsLayer_outFile << ",";
    *MemStatsLayer_outFile << std::to_string(memSize);
    *MemStatsLayer_outFile << ",";
    *MemStatsLayer_outFile << ptr;
    if (allocType == AllocType::GPU) {
        *MemStatsLayer_outFile << std::to_string(memoryTypeIndex);
    }
    *MemStatsLayer_outFile << "\n";
}

static void removeAllocation(AllocType allocType, uint64_t memSize, void* ptr) {
    *MemStatsLayer_outFile << "free,";
    *MemStatsLayer_outFile << std::to_string(int(allocType));
    *MemStatsLayer_outFile << ",";
    *MemStatsLayer_outFile << std::to_string(memSize);
    *MemStatsLayer_outFile << ",";
    *MemStatsLayer_outFile << ptr;
    *MemStatsLayer_outFile << "\n";
}


VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL MemStatsLayer_EnumerateInstanceLayerProperties(uint32_t* pPropertyCount,
    VkLayerProperties* pProperties) {
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
    }
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_DestroyInstance(
        VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    scoped_lock l(globalMutex);
    instanceDispatchTables.erase(getDispatchKey(instance));
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

    addAllocation(AllocType::GPU, pAllocateInfo->allocationSize, *pMemory, pAllocateInfo->memoryTypeIndex);
    auto& allocatedMemoryBlocks = deviceMemInfos[getDispatchKey(device)]->allocatedMemoryBlocks;
    allocatedMemoryBlocks.insert(std::make_pair(*pMemory, pAllocateInfo->allocationSize));

    return res;
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_FreeMemory(
        VkDevice device, const VkDeviceMemory memory, const VkAllocationCallbacks* pAllocator) {
    scoped_lock l(globalMutex);

    auto& allocatedMemoryBlocks = deviceMemInfos[getDispatchKey(device)]->allocatedMemoryBlocks;
    auto it = allocatedMemoryBlocks.find(memory);
    if (it == allocatedMemoryBlocks.end()) {
        return;
    }
    removeAllocation(AllocType::GPU, it->second, memory);
    allocatedMemoryBlocks.erase(it);
    deviceDispatchTables[getDispatchKey(device)].FreeMemory(device, memory, pAllocator);
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

    {
        scoped_lock l(globalMutex);
        return instanceDispatchTables[getDispatchKey(instance)].GetInstanceProcAddr(instance, pName);
    }
}


#ifdef _WIN32

extern "C" {

LPVOID (WINAPI* Real_HeapAlloc)(HANDLE hHeap, DWORD dwFlags, DWORD_PTR dwBytes) = HeapAlloc;
LPVOID (WINAPI* Real_HeapFree)(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem) = HeapFree;

LPVOID WINAPI Mine_HeapAlloc(HANDLE hHeap, DWORD dwFlags, DWORD_PTR dwBytes) {
    LPVOID ptr = Real_HeapAlloc(hHeap, dwFlags, dwBytes);
    if (!ptr) {
        return;
    }

    scoped_lock l(globalMutex);
    cpuAllocations.insert(std::make_pair(ptr, uint64_t(dwBytes)));
    addAllocation(AllocType::CPU, uint64_t(dwBytes), (void*)ptr, 0);

    return ptr;
}

LPVOID WINAPI Mine_HeapFree(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem) {
    BOOL retVal = Real_HeapFree(hHeap, dwFlags, lpMem);
    if (!retVal) {
        return retVal;
    }

    scoped_lock l(globalMutex);
    auto it = cpuAllocations.find(lpMem);
    removeAllocation(AllocType::CPU, it->second, (void*)lpMem);
    cpuAllocations.erase(it);

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
    }
    return TRUE;
#elif defined(USE_MINHOOK)
    if (DetourIsHelperProcess()) {
        return TRUE;
    }

    if (dwReason == DLL_PROCESS_ATTACH) {
        MH_Initialize();
        MH_CreateHook(&HeapAlloc, &MemStatsLayer_HeapAlloc, reinterpret_cast<LPVOID*>(&Real_HeapAlloc));
        MH_CreateHook(&HeapFree, &MemStatsLayer_HeapFree, reinterpret_cast<LPVOID*>(&Real_HeapFree));
        MH_EnableHook(&HeapFree);
        MH_EnableHook(&HeapAlloc);
    } else if (dwReason == DLL_PROCESS_DETACH) {
        MH_DisableHook(&HeapAlloc);
        MH_DisableHook(&HeapFree);
        MH_Uninitialize();
    }
    return TRUE;
#endif
}

#else

#include <stdio.h>
#include <dlfcn.h>
#include <unistd.h>

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
static bool hooked_alloc = false;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

#define CHECK_DLSYM(f) \
    if (!real_##f) { \
        fprintf(stderr, "Error in dlsym: %s\n", dlerror()); \
    }

static void MemStatsLayer_memtrace_init() {
    pthread_mutex_lock(&mutex);

    real_malloc = (malloc_type)dlsym(RTLD_NEXT, "malloc");
    CHECK_DLSYM(malloc);
    real_free = (free_type)dlsym(RTLD_NEXT, "free");
    CHECK_DLSYM(free);
    real_calloc = (calloc_type)dlsym(RTLD_NEXT, "calloc");
    CHECK_DLSYM(calloc);
    real_realloc = (realloc_type)dlsym(RTLD_NEXT, "realloc");
    CHECK_DLSYM(realloc);

    pthread_mutex_unlock(&mutex);
}

__attribute__((constructor)) void MemStatsLayer_OnLoad() {
    //MemStatsLayer_memtrace_init(); // Didn't work...
    MemStatsLayer_outFile = new std::ofstream("memstats.csv", std::ofstream::out);
}

__attribute__((destructor)) void MemStatsLayer_OnFree() {
    MemStatsLayer_outFile->close();
    delete MemStatsLayer_outFile;
}

extern "C" {

void* malloc(size_t size) {
    if (!real_malloc) {
        MemStatsLayer_memtrace_init();
    }
    //write(STDOUT_FILENO, "malloc()", strlen("malloc()"));
    void* ptr = real_malloc(size);
    if (hooked_alloc) {
        return ptr;
    }

    pthread_mutex_lock(&mutex);
    hooked_alloc = true;
    printf("1 malloc(%zu) = %p\n", size, ptr);
    {
        printf("2 malloc(%zu) = %p\n", size, ptr);
        scoped_lock l(globalMutex);
        printf("3 malloc(%zu) = %p\n", size, ptr);
        cpuAllocations.insert(std::make_pair(ptr, uint64_t(size)));
        printf("4 malloc(%zu) = %p\n", size, ptr);
        addAllocation(AllocType::CPU, uint64_t(size), ptr, 0);
        printf("5 malloc(%zu) = %p\n", size, ptr);
    }
    printf("6 malloc(%zu) = %p\n", size, ptr);
    hooked_alloc = false;
    pthread_mutex_unlock(&mutex);

    return ptr;
}

void free(void* ptr) {
    real_free(ptr);
    if (hooked_alloc) {
        return;
    }

    /*pthread_mutex_lock(&mutex);
    hooked_alloc = true;
    {
        scoped_lock l(globalMutex);
        hooked_alloc = true;
        auto it = cpuAllocations.find(ptr);
        removeAllocation(AllocType::CPU, it->second, ptr);
        cpuAllocations.erase(it);
    }
    hooked_alloc = false;
    pthread_mutex_unlock(&mutex);*/
}

void* calloc(size_t num, size_t size) {
    void* ptr = real_calloc(num, size);
    if (hooked_alloc) {
        return ptr;
    }

    /*pthread_mutex_lock(&mutex);
    hooked_alloc = true;
    {
        scoped_lock l(globalMutex);
        hooked_alloc = true;
        cpuAllocations.insert(std::make_pair(ptr, uint64_t(num * size)));
        addAllocation(AllocType::CPU, uint64_t(num * size), ptr, 0);
    }
    hooked_alloc = false;
    pthread_mutex_unlock(&mutex);*/

    return ptr;
}

void* realloc(void* ptr, size_t new_size) {
    void* new_ptr = real_realloc(ptr, new_size);
    if (hooked_alloc) {
        return new_ptr;
    }

    /*pthread_mutex_lock(&mutex);
    hooked_alloc = true;
    {
        scoped_lock l(globalMutex);
        hooked_alloc = true;
        auto it = cpuAllocations.find(ptr);
        removeAllocation(AllocType::CPU, it->second, ptr);
        cpuAllocations.erase(it);
        cpuAllocations.insert(std::make_pair(new_ptr, uint64_t(new_size)));
        addAllocation(AllocType::CPU, uint64_t(new_size), new_ptr, 0);
    }
    hooked_alloc = false;
    pthread_mutex_unlock(&mutex);*/

    return new_ptr;
}

}

#endif
