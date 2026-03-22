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

#ifdef HOOK_MALLOC
#ifdef _WIN32

#include <cstdarg>
#include <windows.h>
#include <strsafe.h>

#ifdef USE_DETOURS
// https://github.com/microsoft/Detours
#include <detours.h>
#elif defined(USE_MINHOOK)
// https://github.com/TsudaKageyu/minhook
#include <MinHook.h>
#endif

#else // defined(HOOK_MALLOC) && !defined(_WIN32)

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

// Instance and device dispatch tables and settings.
static std::map<void*, VkuInstanceDispatchTable> instanceDispatchTables;
static std::map<void*, VkuDeviceDispatchTable> deviceDispatchTables;

/**
 * Global lock for access to the maps above.
 */
static std::mutex globalMutex;
typedef std::lock_guard<std::mutex> scoped_lock;
// May not be able to use static; https://stackoverflow.com/questions/12463718/linux-equivalent-of-dllmain
std::chrono::high_resolution_clock::time_point MemStatsLayer_startTime;
FILE* MemStatsLayer_outFile;

#if !defined(HOOK_MALLOC) || !defined(_WIN32)
std::atomic<int> MemStatsLayer_refcount = 0;
#endif

// Global mutex for access to allocator and file output.
#ifdef _WIN32
static std::mutex globalAllocMutex;
#else
static pthread_mutex_t globalAllocMutex = PTHREAD_MUTEX_INITIALIZER;
#endif

#ifdef HOOK_MALLOC

// Whether hooked alloc is in use and we should forward internal allocs to CRT (used on Linux only due to limitations).
static bool hooked_alloc = false;

#ifdef _WIN32
#define ACQUIRE_ALLOC() globalAllocMutex.lock(); hooked_alloc = true;
#define RELEASE_ALLOC() hooked_alloc = false; globalAllocMutex.unlock();
#else
#define ACQUIRE_ALLOC() pthread_mutex_lock(&mutex); hooked_alloc = true;
#define RELEASE_ALLOC() hooked_alloc = false; pthread_mutex_unlock(&mutex);
#endif

#else // !defined(HOOK_MALLOC)

#ifdef _WIN32
#define ACQUIRE_ALLOC() globalAllocMutex.lock();
#define RELEASE_ALLOC() globalAllocMutex.unlock();
#else
#define ACQUIRE_ALLOC() pthread_mutex_lock(&mutex);
#define RELEASE_ALLOC() pthread_mutex_unlock(&mutex);
#endif

#endif

#if !defined(_WIN32) && defined(HOOK_MALLOC)
pid_t MemStatsLayer_pid = 0;
#endif

/// @return Elapsed time since application start in nanoseconds.
uint64_t getTimeStamp() {
    auto now = std::chrono::high_resolution_clock::now();
    auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(now - MemStatsLayer_startTime).count();
    static_assert(sizeof(nanoseconds) == sizeof(uint64_t));
    return uint64_t(nanoseconds);
}

enum class AllocType {
    CPU = 0, GPU = 1
};

#if defined(_WIN32) && defined(HOOK_MALLOC)
/**
 * The MSVC CRT seems to have a heap lock in the debug libraries.
 * This means that a deadlock will occur when calling the "malloc" or "new" in the hooked memory allocation functions.
 * fprintf may do dynamic memory allocations. So replace this with stack-only allocations on Windows.
 * - vsnprintf and StringCbVPrintfExA are not an option, as it does CRT allocations.
 * - wvsprintfA is not an option, as it is heavily limited (missing certain format types.
 * So, we reimplement vsnprintf with support for the following formats:
 * - %s: String
 * - %c: Char
 * - %d: Signed int
 * - %u: Unsigned int
 * - %llu: Unsigned 64-bit int
 * - %p: Pointer (assumes 64-bit pointer size)
 * Further reads on Windows heap allocation handling for those interested:
 * - https://docs.microsoft.com/en-us/windows/win32/memory/heap-functions
 * - https://learn.microsoft.com/en-us/windows/win32/memory/comparing-memory-allocation-methods
 * - For debug, we could use: https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/crtsetallochook?view=msvc-170
 * - One person at least seems to also have successfully hooked new/malloc/delete/free:
 *   https://stackoverflow.com/questions/59579670/why-hooking-heapfree-with-detours-not-working-for-delete-free
 * - In case we ever would need to reimplement itoa as well (luckily seems to not be necessary), we could use
 *   https://gist.github.com/7h3w4lk3r/3f0ac29713b11ad01c8cd2894550d2c9 as a reference.
 */
static void fprintf_save(FILE* stream, const char* format, ...) {
    static_assert(sizeof(void*) == sizeof(uint64_t));

    char buffer[4096];
    char temp[32]; // largest uint64 value has 21 digits (+ some spare space for hex, sign, ...)
    int fmtPos = 0, bufPos = 0;

    va_list vlist;
    va_start(vlist, format);
    while (format[fmtPos] != '\0' && bufPos < sizeof(buffer)) {
        if (format[fmtPos] == '%') {
            fmtPos++;
            if (format[fmtPos] == 's') {
                // String
                const char* str = va_arg(vlist, const char*);
                while (*str != '\0' && bufPos < sizeof(buffer)) {
                    buffer[bufPos++] = *str++;
                }
            } else if (format[fmtPos] == 'c') {
                // Char
                buffer[bufPos++] = static_cast<char>(va_arg(vlist, int));
            } else if (format[fmtPos] == 'd') {
                // Signed integer
                int num = va_arg(vlist, int);
                _itoa_s(num, temp, _countof(temp), 10);
                for (int i = 0; temp[i] != 0 && bufPos < sizeof(buffer); i++) {
                    buffer[bufPos++] = temp[i];
                }
            } else if (format[fmtPos] == 'u') {
                // Unsigned 32-bit integer
                unsigned long num = va_arg(vlist, unsigned long);
                _ultoa_s(num, temp, _countof(temp), 10);
                for (int i = 0; temp[i] != 0 && bufPos < sizeof(buffer); i++) {
                    buffer[bufPos++] = temp[i];
                }
            } else if (format[fmtPos] == 'l' && format[fmtPos + 1] == 'l' && format[fmtPos + 2] == 'u') {
                // Unsigned 64-bit integer
                fmtPos += 2;
                uint64_t num = va_arg(vlist, uint64_t);
                _ui64toa_s(num, temp, _countof(temp), 10);
                for (int i = 0; temp[i] != 0 && bufPos < sizeof(buffer); i++) {
                    buffer[bufPos++] = temp[i];
                }
            } else if (format[fmtPos] == 'p') {
                // Pointer
                void* ptr = va_arg(vlist, void*);
                buffer[bufPos++] = '0';
                if (bufPos < sizeof(buffer)) {
                    buffer[bufPos++] = 'x';
                }
                _ui64toa_s(reinterpret_cast<uint64_t>(ptr), temp, _countof(temp), 16);
                for (int i = 0; temp[i] != 0 && bufPos < sizeof(buffer); i++) {
                    buffer[bufPos++] = temp[i];
                }
            } else {
                // Attempt to pass through unsupported data type formats.
                buffer[bufPos++] = '%';
                if (bufPos < sizeof(buffer)) {
                    buffer[bufPos++] = format[fmtPos];
                }
            }
        } else {
            buffer[bufPos++] = format[fmtPos];
        }
        fmtPos++;
    }
    va_end(vlist);

    // Seems to not allocate heap memory via the CRT, but otherwise we could try the WinAPI function WriteFile.
    fwrite(buffer, 1, bufPos, stream);
}
#else
/**
 * On Linux, there is no global heap lock (unlike Windows).
 * We can use "hooked_alloc" to forward internal allocations directly to the original functions.
 * Still, in the long run, it may also make sense to avoid heap memory allocations and use the Windows function above.
 */
#define fprintf_save fprintf
#endif

static void addAllocation(AllocType allocType, uint64_t memSize, void* ptr, uint32_t memoryTypeIndex) {
    if (allocType == AllocType::CPU) {
        fprintf_save(MemStatsLayer_outFile, "alloc,%" PRIu64 ",%d,%" PRIu64 ",%p\n",
                getTimeStamp(), int(allocType), memSize, ptr);
    } else {
        fprintf_save(MemStatsLayer_outFile, "alloc,%" PRIu64 ",%d,%" PRIu64 ",%p,%u\n",
                getTimeStamp(), int(allocType), memSize, ptr, memoryTypeIndex);
    }
}

static void removeAllocation(AllocType allocType, void* ptr) {
    fprintf_save(MemStatsLayer_outFile, "free,%" PRIu64 ",%d,%p\n", getTimeStamp(), int(allocType), ptr);
}


VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL MemStatsLayer_EnumerateInstanceLayerProperties(
        uint32_t* pPropertyCount, VkLayerProperties* pProperties) {
    if (pPropertyCount) {
        *pPropertyCount = 1;
    }

    if (pProperties) {
#define LAYER_DESC "Memory usage statistics layer - https://github.com/chrismile/VkLayerMemStats"
#ifdef _WIN32
        strcpy_s(pProperties->layerName, VK_LAYER_NAME);
        strcpy_s(pProperties->description, LAYER_DESC);
#else
        strcpy(pProperties->layerName, VK_LAYER_NAME);
        strcpy(pProperties->description, LAYER_DESC);
#endif
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
    ACQUIRE_ALLOC();
#if !defined(_WIN32) && defined(HOOK_MALLOC)
    if (getpid() == MemStatsLayer_pid)
#endif
    {
        fprintf_save(MemStatsLayer_outFile, "devinfo,%" PRIu64 ",%u,%s\n",
                getTimeStamp(), uint32_t(deviceProperties.deviceType), deviceProperties.deviceName);
        for (uint32_t heapIdx = 0; heapIdx < deviceMemoryProperties.memoryHeapCount; heapIdx++) {
            auto& memHeap = deviceMemoryProperties.memoryHeaps[heapIdx];
            fprintf_save(MemStatsLayer_outFile, "memheap,%" PRIu64 ",%u,%" PRIu64 ",%u\n",
                    getTimeStamp(), heapIdx, uint64_t(memHeap.size), uint32_t(memHeap.flags));
        }
        for (uint32_t memoryTypeIdx = 0; memoryTypeIdx < deviceMemoryProperties.memoryTypeCount; memoryTypeIdx++) {
            auto& memType = deviceMemoryProperties.memoryTypes[memoryTypeIdx];
            fprintf_save(MemStatsLayer_outFile, "memtype,%" PRIu64 ",%u,%u,%u\n",
                    getTimeStamp(), memoryTypeIdx, memType.heapIndex, uint32_t(memType.propertyFlags));
        }
    }
    RELEASE_ALLOC();

    {
        scoped_lock l(globalMutex);
        deviceDispatchTables[getDispatchKey(*pDevice)] = dispatchTable;
    }
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_DestroyDevice(
        VkDevice device, const VkAllocationCallbacks* pAllocator) {
    scoped_lock l(globalMutex);
    deviceDispatchTables.erase(getDispatchKey(device));
}


VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL MemStatsLayer_AllocateMemory(
        VkDevice device, const VkMemoryAllocateInfo* pAllocateInfo, const VkAllocationCallbacks* pAllocator,
        VkDeviceMemory* pMemory) {
    scoped_lock l(globalMutex);

    VkResult res = deviceDispatchTables[getDispatchKey(device)].AllocateMemory(device, pAllocateInfo, pAllocator, pMemory);
    if (res != VK_SUCCESS) {
        return res;
    }

    ACQUIRE_ALLOC();
#if !defined(_WIN32) && defined(HOOK_MALLOC)
    if (getpid() == MemStatsLayer_pid)
#endif
    {
        addAllocation(AllocType::GPU, pAllocateInfo->allocationSize, *pMemory, pAllocateInfo->memoryTypeIndex);
    }
    RELEASE_ALLOC();

    return res;
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_FreeMemory(
        VkDevice device, const VkDeviceMemory memory, const VkAllocationCallbacks* pAllocator) {
    scoped_lock l(globalMutex);

    ACQUIRE_ALLOC();
#if !defined(_WIN32) && defined(HOOK_MALLOC)
    if (getpid() == MemStatsLayer_pid)
#endif
    {
        removeAllocation(AllocType::GPU, memory);
    }
    RELEASE_ALLOC();

    deviceDispatchTables[getDispatchKey(device)].FreeMemory(device, memory, pAllocator);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL MemStatsLayer_QueueSubmit(
        VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence) {
    scoped_lock l(globalMutex);

    VkResult res = deviceDispatchTables[getDispatchKey(queue)].QueueSubmit(queue, submitCount, pSubmits, fence);
    if (res != VK_SUCCESS) {
        return res;
    }

    ACQUIRE_ALLOC();
#if !defined(_WIN32) && defined(HOOK_MALLOC)
    if (getpid() == MemStatsLayer_pid)
#endif
    {
        fprintf_save(MemStatsLayer_outFile, "submit,%" PRIu64 "\n", getTimeStamp());
    }
    RELEASE_ALLOC();


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

    ACQUIRE_ALLOC();
#if !defined(_WIN32) && defined(HOOK_MALLOC)
    if (getpid() == MemStatsLayer_pid)
#endif
    {
        fprintf_save(MemStatsLayer_outFile, "acquire_next_image,%" PRIu64 "\n", getTimeStamp());
    }
    RELEASE_ALLOC();

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

extern "C" {

// https://learn.microsoft.com/en-us/windows/win32/api/heapapi/nf-heapapi-heapalloc
LPVOID (WINAPI* Real_HeapAlloc)(HANDLE hHeap, DWORD dwFlags, SIZE_T dwBytes) = HeapAlloc;
// https://learn.microsoft.com/en-us/windows/win32/api/heapapi/nf-heapapi-heapfree
BOOL (WINAPI* Real_HeapFree)(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem) = HeapFree;
// https://learn.microsoft.com/en-us/windows/win32/api/heapapi/nf-heapapi-heaprealloc
LPVOID (WINAPI* Real_HeapReAlloc)(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem, SIZE_T dwBytes) = HeapReAlloc;

LPVOID WINAPI MemStatsLayer_HeapAlloc(HANDLE hHeap, DWORD dwFlags, DWORD_PTR dwBytes) {
    LPVOID ptr = Real_HeapAlloc(hHeap, dwFlags, dwBytes);
    if (!ptr || hooked_alloc) {
        return ptr;
    }

    ACQUIRE_ALLOC();
    addAllocation(AllocType::CPU, uint64_t(dwBytes), (void*)ptr, 0);
    RELEASE_ALLOC();

    return ptr;
}

BOOL WINAPI MemStatsLayer_HeapFree(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem) {
    BOOL retVal = Real_HeapFree(hHeap, dwFlags, lpMem);
    if (!retVal || hooked_alloc) {
        return retVal;
    }

    ACQUIRE_ALLOC();
    removeAllocation(AllocType::CPU, (void*)lpMem);
    RELEASE_ALLOC();

    return retVal;
}

LPVOID WINAPI MemStatsLayer_HeapReAlloc(HANDLE hHeap, DWORD dwFlags, LPVOID lpMem, SIZE_T dwBytes) {
    LPVOID new_ptr = Real_HeapReAlloc(hHeap, dwFlags, lpMem, dwBytes);
    if (!new_ptr || hooked_alloc) {
        return new_ptr;
    }

    ACQUIRE_ALLOC();
    auto new_size = uint64_t(dwBytes);
    removeAllocation(AllocType::CPU, lpMem);
    addAllocation(AllocType::CPU, new_size, new_ptr, 0);
    RELEASE_ALLOC();

    return new_ptr;
}

}

// https://github.com/microsoft/Detours
BOOL WINAPI DllMain(HINSTANCE hModule, DWORD dwReason, LPVOID reserved) {
#ifdef USE_DETOURS
    if (DetourIsHelperProcess()) {
        return TRUE;
    }

    if (dwReason == DLL_PROCESS_ATTACH) {
        if (fopen_s(&MemStatsLayer_outFile, "memstats.csv", "w") != 0) {
            throw std::runtime_error("File memstats.csv could not be opened for writing.");
        }
        MemStatsLayer_startTime = std::chrono::high_resolution_clock::now();

        DetourRestoreAfterWith();
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)Real_HeapAlloc, MemStatsLayer_HeapAlloc);
        DetourAttach(&(PVOID&)Real_HeapFree, MemStatsLayer_HeapFree);
        DetourAttach(&(PVOID&)Real_HeapReAlloc, MemStatsLayer_HeapReAlloc);
        DetourTransactionCommit();
    } else if (dwReason == DLL_PROCESS_DETACH) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)Real_HeapAlloc, MemStatsLayer_HeapAlloc);
        DetourDetach(&(PVOID&)Real_HeapFree, MemStatsLayer_HeapFree);
        DetourDetach(&(PVOID&)Real_HeapReAlloc, MemStatsLayer_HeapReAlloc);
        DetourTransactionCommit();

        fclose(MemStatsLayer_outFile);
    }
#elif defined(USE_MINHOOK)
    if (dwReason == DLL_PROCESS_ATTACH) {
        if (fopen_s(&MemStatsLayer_outFile, "memstats.csv", "w") != 0) {
            throw std::runtime_error("File memstats.csv could not be opened for writing.");
        }

        MemStatsLayer_startTime = std::chrono::high_resolution_clock::now();

        MH_Initialize();
        MH_CreateHook(&Real_HeapAlloc, &MemStatsLayer_HeapAlloc, reinterpret_cast<LPVOID*>(&Real_HeapAlloc));
        MH_CreateHook(&Real_HeapFree, &MemStatsLayer_HeapFree, reinterpret_cast<LPVOID*>(&Real_HeapFree));
        MH_CreateHook(&Real_HeapReAlloc, &MemStatsLayer_HeapReAlloc, reinterpret_cast<LPVOID*>(&Real_HeapReAlloc));
        MH_EnableHook(&Real_HeapFree);
        MH_EnableHook(&Real_HeapAlloc);
        MH_EnableHook(&Real_HeapReAlloc);
    } else if (dwReason == DLL_PROCESS_DETACH) {
        MH_DisableHook(&Real_HeapAlloc);
        MH_DisableHook(&Real_HeapFree);
        MH_DisableHook(&Real_HeapReAlloc);
        MH_Uninitialize();

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
        fprintf_save(stderr, "Error in dlsym: %s\n", dlerror()); \
    }

static void MemStatsLayer_memtrace_init() {
    ACQUIRE_ALLOC();

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

    RELEASE_ALLOC();
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
        ACQUIRE_ALLOC();

        fclose(MemStatsLayer_outFile);
        MemStatsLayer_outFile = nullptr;

        RELEASE_ALLOC();
    }
}

extern "C" {

void* malloc(size_t size) {
    if (!real_malloc) {
        MemStatsLayer_memtrace_init();
    }
    void* ptr = real_malloc(size);
    if (hooked_alloc || MemStatsLayer_pid != getpid()) {
        return ptr;
    }

    ACQUIRE_ALLOC();
    addAllocation(AllocType::CPU, uint64_t(size), ptr, 0);
    RELEASE_ALLOC();

    return ptr;
}

void free(void* ptr) {
    if (!real_free) {
        MemStatsLayer_memtrace_init();
    }
    real_free(ptr);
    if (hooked_alloc || MemStatsLayer_pid != getpid()) {
        return;
    }

    ACQUIRE_ALLOC();
    removeAllocation(AllocType::CPU, ptr);
    RELEASE_ALLOC();
}

void* calloc(size_t num, size_t size) {
    if (!real_calloc) {
        MemStatsLayer_memtrace_init();
    }
    void* ptr = real_calloc(num, size);
    if (hooked_alloc || MemStatsLayer_pid != getpid()) {
        return ptr;
    }

    ACQUIRE_ALLOC();
    addAllocation(AllocType::CPU, uint64_t(num * size), ptr, 0);
    RELEASE_ALLOC();

    return ptr;
}

void* realloc(void* ptr, size_t new_size) {
    if (!real_realloc) {
        MemStatsLayer_memtrace_init();
    }
    void* new_ptr = real_realloc(ptr, new_size);
    if (hooked_alloc || MemStatsLayer_pid != getpid()) {
        return new_ptr;
    }

    ACQUIRE_ALLOC();
    removeAllocation(AllocType::CPU, ptr);
    addAllocation(AllocType::CPU, uint64_t(new_size), new_ptr, 0);
    RELEASE_ALLOC();

    return new_ptr;
}

}

#endif
