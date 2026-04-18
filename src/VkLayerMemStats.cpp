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
#include <list>
#include <map>
#include <unordered_set>
#include <chrono>
#include <fstream>
#include <atomic>
#include <memory>
#include <algorithm>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cctype>
#include <cinttypes>
#include <vulkan/vk_layer.h>
#include <vulkan/utility/vk_dispatch_table.h>
#include <vulkan/layer/vk_layer_settings.h>
#include <vulkan/layer/vk_layer_settings.hpp>

#define FILE_FORMAT_VERSION_NUMBER 2

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifdef HOOK_MALLOC
#ifdef _WIN32
#include <strsafe.h>

#ifdef USE_DETOURS
// https://github.com/microsoft/Detours
#include <detours.h>
#elif defined(USE_MINHOOK)
// https://github.com/TsudaKageyu/minhook
#include <MinHook.h>
#endif

#else // defined(HOOK_MALLOC) && !defined(_WIN32)

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

/// Layer settings.
struct MemStatsLayerSettingsInstance {
    bool useProfiler = false;
};
struct MemStatsLayerSettingsDevice {
    bool useProfiler = false;
    bool useMeshShaderEXT = false;
    bool useRayTracingPipelineKHR = false;
};

// Instance and device dispatch tables and settings.
static std::map<void*, VkuInstanceDispatchTable> instanceDispatchTables;
static std::map<void*, VkInstance> instances;
static std::map<void*, MemStatsLayerSettingsInstance> instanceLayerSettings;
static std::map<void*, VkuDeviceDispatchTable> deviceDispatchTables;
static std::map<void*, MemStatsLayerSettingsDevice> deviceLayerSettings;

/**
 * Global lock for access to the maps above.
 */
static std::mutex globalMutex;
typedef std::lock_guard<std::mutex> scoped_lock;
// May not be able to use static; https://stackoverflow.com/questions/12463718/linux-equivalent-of-dllmain
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
#define ACQUIRE_ALLOC() pthread_mutex_lock(&globalAllocMutex); hooked_alloc = true;
#define RELEASE_ALLOC() hooked_alloc = false; pthread_mutex_unlock(&globalAllocMutex);
#endif

#else // !defined(HOOK_MALLOC)

#ifdef _WIN32
#define ACQUIRE_ALLOC() globalAllocMutex.lock();
#define RELEASE_ALLOC() globalAllocMutex.unlock();
#else
#define ACQUIRE_ALLOC() pthread_mutex_lock(&globalAllocMutex);
#define RELEASE_ALLOC() pthread_mutex_unlock(&globalAllocMutex);
#endif

#endif

#if !defined(_WIN32) && defined(HOOK_MALLOC)
pid_t MemStatsLayer_pid = 0;
#endif

// Don't use std::chrono::high_resolution_clock to be compatible with VkTimeDomainKHR.
//std::chrono::high_resolution_clock::time_point MemStatsLayer_startTime;
int64_t MemStatsLayer_startTime;
#ifdef _WIN32
int64_t MemStatsLayer_timerFreq;
#endif

static int64_t getAbsoluteTimeStamp() {
#ifdef _WIN32
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
#else
    struct timespec tv{};
    clock_gettime(CLOCK_MONOTONIC, &tv);
    return tv.tv_nsec + tv.tv_sec * 1000000000ll;
#endif
}

static void initializeTimer() {
#ifdef _WIN32
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    MemStatsLayer_timerFreq = frequency.QuadPart;
#endif
    MemStatsLayer_startTime = getAbsoluteTimeStamp();
}

/// @return Elapsed time since application start in nanoseconds.
static uint64_t getTimeStamp() {
    //auto now = std::chrono::high_resolution_clock::now();
    //auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(now - MemStatsLayer_startTime).count();
    //static_assert(sizeof(nanoseconds) == sizeof(uint64_t));
    //return uint64_t(nanoseconds);
#ifdef _WIN32
    auto now = getAbsoluteTimeStamp();
    int64_t nanoseconds;
    if (1000000000LL % MemStatsLayer_timerFreq == 0) {
        nanoseconds = (now - MemStatsLayer_startTime) * (1000000000LL / MemStatsLayer_timerFreq);
    } else {
        nanoseconds = (now - MemStatsLayer_startTime) * 1000000000LL / MemStatsLayer_timerFreq;
    }
    return static_cast<uint64_t>(nanoseconds);
#else
    auto now = getAbsoluteTimeStamp();
    auto nanoseconds = now - MemStatsLayer_startTime;
    return static_cast<uint64_t>(nanoseconds);
#endif
}

static uint64_t deviceTimeStampToHostTimeStamp(
        uint64_t timeStampDevice, float timestampPeriodDevice,
        uint64_t calibTimeStampDevice, uint64_t calibTimeStampHost) {
    auto timeOffsetDeviceNs = static_cast<int64_t>(
            static_cast<double>(static_cast<int64_t>(timeStampDevice) - static_cast<int64_t>(calibTimeStampDevice))
            * static_cast<double>(timestampPeriodDevice));
#ifdef _WIN32
    auto timeStampHost = static_cast<int64_t>(calibTimeStampHost) - MemStatsLayer_startTime;
    if (1000000000LL % MemStatsLayer_timerFreq == 0) {
        timeStampHost = timeStampHost * (1000000000LL / MemStatsLayer_timerFreq);
    } else {
        timeStampHost = timeStampHost * 1000000000LL / MemStatsLayer_timerFreq;
    }
    timeStampHost += timeOffsetDeviceNs;
#else
    auto timeStampHost = static_cast<int64_t>(calibTimeStampHost) + timeOffsetDeviceNs - MemStatsLayer_startTime;
#endif
    if (timeStampHost < 0) {
        std::cerr << "[VkLayer_memstats] Negative device time stamp detected." << std::endl;
        timeStampHost = 0;
    }
    return static_cast<uint64_t>(timeStampHost);
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
static void vfprintf_save(FILE* file, const char* format, va_list vlist) {
    static_assert(sizeof(void*) == sizeof(uint64_t));

    char buffer[4096];
    char temp[32]; // largest uint64 value has 21 digits (+ some spare space for hex, sign, ...)
    int fmtPos = 0, bufPos = 0;

    while (format[fmtPos] != '\0' && bufPos < static_cast<int>(sizeof(buffer))) {
        if (format[fmtPos] == '%') {
            fmtPos++;
            if (format[fmtPos] == 's') {
                // String
                const char* str = va_arg(vlist, const char*);
                while (*str != '\0' && bufPos < static_cast<int>(sizeof(buffer))) {
                    buffer[bufPos++] = *str++;
                }
            } else if (format[fmtPos] == 'c') {
                // Char
                buffer[bufPos++] = static_cast<char>(va_arg(vlist, int));
            } else if (format[fmtPos] == 'd') {
                // Signed integer
                int num = va_arg(vlist, int);
                _itoa_s(num, temp, _countof(temp), 10);
                for (int i = 0; temp[i] != 0 && bufPos < static_cast<int>(sizeof(buffer)); i++) {
                    buffer[bufPos++] = temp[i];
                }
            } else if (format[fmtPos] == 'u') {
                // Unsigned 32-bit integer
                unsigned long num = va_arg(vlist, unsigned long);
                _ultoa_s(num, temp, _countof(temp), 10);
                for (int i = 0; temp[i] != 0 && bufPos < static_cast<int>(sizeof(buffer)); i++) {
                    buffer[bufPos++] = temp[i];
                }
            } else if (format[fmtPos] == 'l' && format[fmtPos + 1] == 'l' && format[fmtPos + 2] == 'u') {
                // Unsigned 64-bit integer
                fmtPos += 2;
                uint64_t num = va_arg(vlist, uint64_t);
                _ui64toa_s(num, temp, _countof(temp), 10);
                for (int i = 0; temp[i] != 0 && bufPos < static_cast<int>(sizeof(buffer)); i++) {
                    buffer[bufPos++] = temp[i];
                }
            } else if (format[fmtPos] == 'p') {
                // Pointer
                void* ptr = va_arg(vlist, void*);
                _ui64toa_s(reinterpret_cast<uint64_t>(ptr), temp, _countof(temp), 16);
                int pointerStringLength;
                for (pointerStringLength = 0; temp[pointerStringLength] != 0; pointerStringLength++) {
                }
                for (int i = pointerStringLength; i < 16 && bufPos < static_cast<int>(sizeof(buffer)); i++) {
                    buffer[bufPos++] = '0';
                }
                for (int i = 0; temp[i] != 0 && bufPos < static_cast<int>(sizeof(buffer)); i++) {
                    buffer[bufPos++] = temp[i];
                }
            } else {
                // Attempt to pass through unsupported data type formats.
                buffer[bufPos++] = '%';
                if (bufPos < static_cast<int>(sizeof(buffer))) {
                    buffer[bufPos++] = format[fmtPos];
                }
            }
        } else {
            buffer[bufPos++] = format[fmtPos];
        }
        fmtPos++;
    }

    // Seems to not allocate heap memory via the CRT, but otherwise we could try the WinAPI function WriteFile.
    fwrite(buffer, 1, bufPos, file);
}
inline void fprintf_save(FILE* file, const char* format, ...) {
    va_list vlist;
    va_start(vlist, format);
    vfprintf_save(file, format, vlist);
    va_end(vlist);
}
#else
/**
 * On Linux, there is no global heap lock (unlike Windows).
 * We can use "hooked_alloc" to forward internal allocations directly to the original functions.
 * Still, in the long run, it may also make sense to avoid heap memory allocations and use the Windows function above.
 */
#define fprintf_save fprintf
#define vfprintf_save vfprintf
#endif

#if !defined(_WIN32) && defined(HOOK_MALLOC)
#define fprintf_wrapper(...) \
    ACQUIRE_ALLOC(); \
    if (getpid() == MemStatsLayer_pid) \
    { \
        fprintf_save(__VA_ARGS__); \
    } \
    RELEASE_ALLOC();
#else
#define fprintf_wrapper(...) \
    ACQUIRE_ALLOC(); \
    fprintf_save(__VA_ARGS__); \
    RELEASE_ALLOC();
#endif

VK_LAYER_EXPORT void memstats_printf(const char* format, ...) {
    ACQUIRE_ALLOC();
    va_list vlist;
    va_start(vlist, format);
    vfprintf_save(MemStatsLayer_outFile, format, vlist);
    va_end(vlist);
    RELEASE_ALLOC();
}

VK_LAYER_EXPORT uint64_t memstats_gettimestamp() {
    return getTimeStamp();
}



#include "Profiler.hpp"

static void addAllocation(AllocType allocType, uint64_t memSize, void* ptr, uint32_t memoryTypeIndex) {
    if (allocType == AllocType::CPU) {
        fprintf_save(MemStatsLayer_outFile, "alloc,%" PRIu64 ",%d,%" PRIu64 ",0x%p\n",
                getTimeStamp(), int(allocType), memSize, ptr);
    } else {
        fprintf_save(MemStatsLayer_outFile, "alloc,%" PRIu64 ",%d,%" PRIu64 ",0x%p,%u\n",
                getTimeStamp(), int(allocType), memSize, ptr, memoryTypeIndex);
    }
}

static void removeAllocation(AllocType allocType, void* ptr) {
    fprintf_save(MemStatsLayer_outFile, "free,%" PRIu64 ",%d,0x%p\n", getTimeStamp(), int(allocType), ptr);
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

    VkuLayerSettingSet layerSettingSet = VK_NULL_HANDLE;
    result = vkuCreateLayerSettingSet(
            VK_LAYER_NAME, vkuFindLayerSettingsCreateInfo(pCreateInfo), pAllocator, nullptr, &layerSettingSet);
    if (result != VK_SUCCESS) {
        return result;
    }
    MemStatsLayerSettingsInstance settings{};
    const char *settingKeyProfiler = "profiler";
    if (vkuHasLayerSetting(layerSettingSet, settingKeyProfiler)) {
        std::string profilerString;
        vkuGetLayerSettingValue(layerSettingSet, settingKeyProfiler, profilerString);
        std::transform(
                profilerString.begin(), profilerString.end(), profilerString.begin(),
                [](unsigned char c){ return std::tolower(c); });
        if (profilerString == "on" || profilerString == "1") {
            settings.useProfiler = true;
        }
    }
    vkuDestroyLayerSettingSet(layerSettingSet, pAllocator);

    {
        scoped_lock l(globalMutex);
        instanceDispatchTables[getDispatchKey(*pInstance)] = dispatchTable;
        instanceLayerSettings[getDispatchKey(*pInstance)] = settings;
        instances[getDispatchKey(*pInstance)] = *pInstance;
#ifndef HOOK_MALLOC
        if (MemStatsLayer_refcount == 0) {
            MemStatsLayer_outFile = fopen("memstats.csv", "w");
            fprintf_save(MemStatsLayer_outFile, "version,%d,%d\n", 0, FILE_FORMAT_VERSION_NUMBER);
            initializeTimer();
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
    instanceLayerSettings.erase(getDispatchKey(instance));
    instances.erase(getDispatchKey(instance));
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
	// The code below should fail due to the instance being a null handle according to
	// https://docs.vulkan.org/refpages/latest/refpages/source/vkGetInstanceProcAddr.html, but for whatever reason,
	// this works with my own Vulkan apps and only fails with Blender.
    //auto pGetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)pGetInstanceProcAddr(
    //        VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties");
    //auto pGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)pGetInstanceProcAddr(
    //        VK_NULL_HANDLE, "vkGetPhysicalDeviceMemoryProperties");
    VkInstance instance{};
    bool useProfiler = false;
    uint32_t deviceExtensionCount = 0;
    std::vector<VkExtensionProperties> extensionProperties;
    {
        scoped_lock l(globalMutex);
        instance = instances[getDispatchKey(physicalDevice)];
        VkResult res = instanceDispatchTables[getDispatchKey(physicalDevice)].EnumerateDeviceExtensionProperties(
                physicalDevice, nullptr, &deviceExtensionCount, nullptr);
        if (res == VK_SUCCESS) {
            extensionProperties.resize(deviceExtensionCount);
            instanceDispatchTables[getDispatchKey(physicalDevice)].EnumerateDeviceExtensionProperties(
                    physicalDevice, nullptr, &deviceExtensionCount, extensionProperties.data());
        }
        useProfiler = instanceLayerSettings[getDispatchKey(physicalDevice)].useProfiler;
    }
    auto pGetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)pGetInstanceProcAddr(
            instance, "vkGetPhysicalDeviceProperties");
    auto pGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)pGetInstanceProcAddr(
            instance, "vkGetPhysicalDeviceMemoryProperties");
    auto pCreateQueryPool = (PFN_vkCreateQueryPool)pGetInstanceProcAddr(instance, "vkCreateQueryPool");
    auto pDestroyQueryPool = (PFN_vkDestroyQueryPool)pGetInstanceProcAddr(instance, "vkDestroyQueryPool");
    auto pResetQueryPool = (PFN_vkResetQueryPool)pGetInstanceProcAddr(instance, "vkResetQueryPool");
    auto pCmdResetQueryPool = (PFN_vkCmdResetQueryPool)pGetInstanceProcAddr(instance, "vkCmdResetQueryPool");
    auto pGetQueryPoolResults = (PFN_vkGetQueryPoolResults)pGetInstanceProcAddr(instance, "vkGetQueryPoolResults");
    auto pCmdWriteTimestamp = (PFN_vkCmdWriteTimestamp)pGetInstanceProcAddr(instance, "vkCmdWriteTimestamp");

    VkDeviceCreateInfo deviceCreateInfoCopy = *pCreateInfo;
    bool hasMeshShaderEXT = false;
    bool hasRequestedMeshShaderEXT = false;
    bool hasRayTracingPipelineKHR = false;
    bool hasRequestedRayTracingPipelineKHR = false;
    bool hasCalibratedTimestampsKHR = false;
    bool hasRequestedCalibratedTimestampsKHR = false;
    std::vector<const char*> extensionsList;
    extensionsList.reserve(deviceCreateInfoCopy.enabledExtensionCount + 1);
    for (const auto& extensionProperty : extensionProperties) {
        if (strcmp(extensionProperty.extensionName, VK_EXT_MESH_SHADER_EXTENSION_NAME) == 0) {
            hasMeshShaderEXT = true;
        }
        if (strcmp(extensionProperty.extensionName, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) == 0) {
            hasRayTracingPipelineKHR = true;
        }
        if (strcmp(extensionProperty.extensionName, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) == 0) {
            hasCalibratedTimestampsKHR = true;
        }
    }
    for (uint32_t extIdx = 0; extIdx < deviceCreateInfoCopy.enabledExtensionCount; extIdx++) {
        extensionsList.push_back(deviceCreateInfoCopy.ppEnabledExtensionNames[extIdx]);
        if (strcmp(deviceCreateInfoCopy.ppEnabledExtensionNames[extIdx], VK_EXT_MESH_SHADER_EXTENSION_NAME) == 0) {
            hasRequestedMeshShaderEXT = true;
        }
        if (strcmp(deviceCreateInfoCopy.ppEnabledExtensionNames[extIdx], VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) == 0) {
            hasRequestedRayTracingPipelineKHR = true;
        }
        if (strcmp(deviceCreateInfoCopy.ppEnabledExtensionNames[extIdx], VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) == 0) {
            hasRequestedCalibratedTimestampsKHR = true;
        }
    }
    if (useProfiler && hasCalibratedTimestampsKHR && !hasRequestedCalibratedTimestampsKHR) {
        extensionsList.push_back(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    }
    if (useProfiler && !hasCalibratedTimestampsKHR) {
        std::cerr << "[VkLayer_memstats] Profiler requested, but device does not support Vulkan extension '"
                  << VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME << "'." << std::endl;
        useProfiler = false;
    }
    deviceCreateInfoCopy.enabledExtensionCount = static_cast<uint32_t>(extensionsList.size());
    deviceCreateInfoCopy.ppEnabledExtensionNames = extensionsList.data();

    VkResult result = pCreateDevice(physicalDevice, &deviceCreateInfoCopy, pAllocator, pDevice);
    if (result != VK_SUCCESS) {
        return result;
    }

    // Populate the dispatch table using the functions of the next layer.
    VkuDeviceDispatchTable dispatchTable;
    dispatchTable.GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)pGetDeviceProcAddr(*pDevice, "vkGetDeviceProcAddr");
    dispatchTable.DestroyDevice = (PFN_vkDestroyDevice)pGetDeviceProcAddr(*pDevice, "vkDestroyDevice");
    dispatchTable.AllocateMemory = (PFN_vkAllocateMemory)pGetDeviceProcAddr(*pDevice, "vkAllocateMemory");
    dispatchTable.FreeMemory = (PFN_vkFreeMemory)pGetDeviceProcAddr(*pDevice, "vkFreeMemory");
    dispatchTable.BindBufferMemory = (PFN_vkBindBufferMemory)pGetDeviceProcAddr(*pDevice, "vkBindBufferMemory");
    dispatchTable.BindImageMemory = (PFN_vkBindImageMemory)pGetDeviceProcAddr(*pDevice, "vkBindImageMemory");
    dispatchTable.DestroyBuffer = (PFN_vkDestroyBuffer)pGetDeviceProcAddr(*pDevice, "vkDestroyBuffer");
    dispatchTable.DestroyImage = (PFN_vkDestroyImage)pGetDeviceProcAddr(*pDevice, "vkDestroyImage");
    dispatchTable.BeginCommandBuffer = (PFN_vkBeginCommandBuffer)pGetDeviceProcAddr(*pDevice, "vkBeginCommandBuffer");
    dispatchTable.EndCommandBuffer = (PFN_vkEndCommandBuffer)pGetDeviceProcAddr(*pDevice, "vkEndCommandBuffer");
    dispatchTable.CmdUpdateBuffer = (PFN_vkCmdUpdateBuffer)pGetDeviceProcAddr(*pDevice, "vkCmdUpdateBuffer");
    dispatchTable.CmdCopyBuffer = (PFN_vkCmdCopyBuffer)pGetDeviceProcAddr(*pDevice, "vkCmdCopyBuffer");
    dispatchTable.CmdCopyImage = (PFN_vkCmdCopyImage)pGetDeviceProcAddr(*pDevice, "vkCmdCopyImage");
    dispatchTable.CmdCopyBufferToImage = (PFN_vkCmdCopyBufferToImage)pGetDeviceProcAddr(*pDevice, "vkCmdCopyBufferToImage");
    dispatchTable.CmdCopyImageToBuffer = (PFN_vkCmdCopyImageToBuffer)pGetDeviceProcAddr(*pDevice, "vkCmdCopyImageToBuffer");
    dispatchTable.CmdDispatch = (PFN_vkCmdDispatch)pGetDeviceProcAddr(*pDevice, "vkCmdDispatch");
    dispatchTable.CmdDispatchIndirect = (PFN_vkCmdDispatchIndirect)pGetDeviceProcAddr(*pDevice, "vkCmdDispatchIndirect");
    dispatchTable.CmdDraw = (PFN_vkCmdDraw)pGetDeviceProcAddr(*pDevice, "vkCmdDraw");
    dispatchTable.CmdDrawIndirect = (PFN_vkCmdDrawIndirect)pGetDeviceProcAddr(*pDevice, "vkCmdDrawIndirect");
    dispatchTable.CmdDrawIndirectCount = (PFN_vkCmdDrawIndirectCount)pGetDeviceProcAddr(*pDevice, "vkCmdDrawIndirectCount");
    dispatchTable.CmdDrawIndexed = (PFN_vkCmdDrawIndexed)pGetDeviceProcAddr(*pDevice, "vkCmdDrawIndexed");
    dispatchTable.CmdDrawIndexedIndirect = (PFN_vkCmdDrawIndexedIndirect)pGetDeviceProcAddr(*pDevice, "vkCmdDrawIndexedIndirect");
    dispatchTable.CmdDrawIndexedIndirectCount = (PFN_vkCmdDrawIndexedIndirectCount)pGetDeviceProcAddr(*pDevice, "vkCmdDrawIndexedIndirectCount");
    if (hasMeshShaderEXT && hasRequestedMeshShaderEXT) {
        dispatchTable.CmdDrawMeshTasksEXT = (PFN_vkCmdDrawMeshTasksEXT)pGetDeviceProcAddr(*pDevice, "vkCmdDrawMeshTasksEXT");
        dispatchTable.CmdDrawMeshTasksIndirectEXT = (PFN_vkCmdDrawMeshTasksIndirectEXT)pGetDeviceProcAddr(*pDevice, "vkCmdDrawMeshTasksIndirectEXT");
        dispatchTable.CmdDrawMeshTasksIndirectCountEXT = (PFN_vkCmdDrawMeshTasksIndirectCountEXT)pGetDeviceProcAddr(*pDevice, "vkCmdDrawMeshTasksIndirectCountEXT");
    }
    if (hasRayTracingPipelineKHR && hasRequestedRayTracingPipelineKHR) {
        dispatchTable.CmdTraceRaysKHR = (PFN_vkCmdTraceRaysKHR)pGetDeviceProcAddr(*pDevice, "vkCmdTraceRaysKHR");
        dispatchTable.CmdTraceRaysIndirectKHR = (PFN_vkCmdTraceRaysIndirectKHR)pGetDeviceProcAddr(*pDevice, "vkCmdTraceRaysIndirectKHR");
    }
    dispatchTable.QueueSubmit = (PFN_vkQueueSubmit)pGetDeviceProcAddr(*pDevice, "vkQueueSubmit");
    dispatchTable.AcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)pGetDeviceProcAddr(*pDevice, "vkAcquireNextImageKHR");
    dispatchTable.QueuePresentKHR = (PFN_vkQueuePresentKHR)pGetDeviceProcAddr(*pDevice, "vkQueuePresentKHR");
    dispatchTable.WaitForFences = (PFN_vkWaitForFences)pGetDeviceProcAddr(*pDevice, "vkWaitForFences");
    dispatchTable.WaitSemaphores = (PFN_vkWaitSemaphores)pGetDeviceProcAddr(*pDevice, "vkWaitSemaphores");
    dispatchTable.DeviceWaitIdle = (PFN_vkDeviceWaitIdle)pGetDeviceProcAddr(*pDevice, "vkDeviceWaitIdle");
    dispatchTable.QueueWaitIdle = (PFN_vkQueueWaitIdle)pGetDeviceProcAddr(*pDevice, "vkQueueWaitIdle");

    // Write memory statistics.
    VkPhysicalDeviceProperties deviceProperties{};
    VkPhysicalDeviceMemoryProperties deviceMemoryProperties{};
    pGetPhysicalDeviceProperties(physicalDevice, &deviceProperties);
    pGetPhysicalDeviceMemoryProperties(physicalDevice, &deviceMemoryProperties);
    useProfiler = useProfiler && deviceProperties.limits.timestampComputeAndGraphics && hasCalibratedTimestampsKHR;
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

    PFN_vkGetCalibratedTimestampsKHR pGetCalibratedTimestampsKHR = nullptr;
    PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR pGetPhysicalDeviceCalibrateableTimeDomainsKHR = nullptr;
    if (useProfiler && hasCalibratedTimestampsKHR) {
        pGetCalibratedTimestampsKHR = (PFN_vkGetCalibratedTimestampsKHR)pGetInstanceProcAddr(
                instance, "vkGetCalibratedTimestampsKHR");
        pGetPhysicalDeviceCalibrateableTimeDomainsKHR = (PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR)pGetInstanceProcAddr(
                instance, "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR");
        uint32_t numTimeDomains = 0;
        std::vector<VkTimeDomainKHR> timeDomains{};
        auto res = pGetPhysicalDeviceCalibrateableTimeDomainsKHR(physicalDevice, &numTimeDomains, nullptr);
        if (res != VK_SUCCESS) {
            std::cerr << "[VkLayer_memstats] vkGetPhysicalDeviceCalibrateableTimeDomainsKHR failed." << std::endl;
        }
        timeDomains.resize(numTimeDomains);
        res = pGetPhysicalDeviceCalibrateableTimeDomainsKHR(physicalDevice, &numTimeDomains, timeDomains.data());
        if (res != VK_SUCCESS) {
            std::cerr << "[VkLayer_memstats] vkGetPhysicalDeviceCalibrateableTimeDomainsKHR failed." << std::endl;
        }
        bool hasDeviceTimeDomain = false;
        bool hasHostTimeDomain = false;
        for (VkTimeDomainKHR timeDomain : timeDomains) {
            if (timeDomain == VK_TIME_DOMAIN_DEVICE_KHR) {
                hasDeviceTimeDomain = true;
            }
#ifdef _WIN32
            if (timeDomain == VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR) {
                hasHostTimeDomain = true;
            }
#else
            if (timeDomain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR) {
                hasHostTimeDomain = true;
            }
#endif
        }
        if (!hasDeviceTimeDomain) {
            std::cerr << "[VkLayer_memstats] Profiler requested, but device does not support device time domain." << std::endl;
            useProfiler = false;
        }
        if (!hasHostTimeDomain) {
            std::cerr << "[VkLayer_memstats] Profiler requested, but device does not support host time domain." << std::endl;
            useProfiler = false;
        }
    }

    {
        scoped_lock l(globalMutex);
        deviceDispatchTables[getDispatchKey(*pDevice)] = dispatchTable;
        MemStatsLayerSettingsDevice settings{};
        settings.useProfiler = useProfiler;
        settings.useMeshShaderEXT = hasMeshShaderEXT && hasMeshShaderEXT;
        settings.useRayTracingPipelineKHR = hasRayTracingPipelineKHR && hasRequestedRayTracingPipelineKHR;
        deviceLayerSettings[getDispatchKey(*pDevice)] = settings;
        auto* profiler = new MemStatsLayer_Profiler;
        profiler->device = *pDevice;
        profiler->pCreateQueryPool = pCreateQueryPool;
        profiler->pDestroyQueryPool = pDestroyQueryPool;
        profiler->pResetQueryPool = pResetQueryPool;
        profiler->pCmdResetQueryPool = pCmdResetQueryPool;
        profiler->pGetQueryPoolResults = pGetQueryPoolResults;
        profiler->pCmdWriteTimestamp = pCmdWriteTimestamp;
        profiler->pGetCalibratedTimestampsKHR = pGetCalibratedTimestampsKHR;
        profiler->supportsQueries = useProfiler;
        profiler->timestampPeriod = deviceProperties.limits.timestampPeriod;
        profilerMap[getDispatchKey(*pDevice)] = profiler;
    }
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_DestroyDevice(
        VkDevice device, const VkAllocationCallbacks* pAllocator) {
    scoped_lock l(globalMutex);
    auto* profiler = profilerMap[getDispatchKey(device)];
    for (auto* frameData : profiler->submitsInFlight) {
        frameData->readBack();
        profiler->freeSubmitDataList.push_back(frameData);
    }
    profiler->submitsInFlight.clear();
    for (auto* frameData : profiler->freeSubmitDataList) {
        delete frameData;
    }
    delete profiler;
    profilerMap.erase(getDispatchKey(device));
    deviceDispatchTables.erase(getDispatchKey(device));
    deviceLayerSettings.erase(getDispatchKey(device));
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

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL MemStatsLayer_BindBufferMemory(
        VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset) {
    scoped_lock l(globalMutex);

    fprintf_wrapper(
            MemStatsLayer_outFile, "bind_buffer_memory,%" PRIu64 ",0x%p,0x%p,%" PRIu64 "\n",
            getTimeStamp(), buffer, memory, memoryOffset);

    return deviceDispatchTables[getDispatchKey(device)].BindBufferMemory(device, buffer, memory, memoryOffset);
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL MemStatsLayer_BindImageMemory(
        VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize memoryOffset) {
    scoped_lock l(globalMutex);

    fprintf_wrapper(
            MemStatsLayer_outFile, "bind_image_memory,%" PRIu64 ",0x%p,0x%p,%" PRIu64 "\n",
            getTimeStamp(), image, memory, memoryOffset);

    return deviceDispatchTables[getDispatchKey(device)].BindImageMemory(device, image, memory, memoryOffset);
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_DestroyBuffer(
        VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* pAllocator) {
    scoped_lock l(globalMutex);

    fprintf_wrapper(MemStatsLayer_outFile, "destroy_buffer,%" PRIu64 ",0x%p\n", getTimeStamp(), buffer);

    deviceDispatchTables[getDispatchKey(device)].DestroyBuffer(device, buffer, pAllocator);
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_DestroyImage(
        VkDevice device, VkImage image, const VkAllocationCallbacks* pAllocator) {
    scoped_lock l(globalMutex);

    fprintf_wrapper(MemStatsLayer_outFile, "destroy_image,%" PRIu64 ",0x%p\n", getTimeStamp(), image);

    deviceDispatchTables[getDispatchKey(device)].DestroyImage(device, image, pAllocator);
}


VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL MemStatsLayer_BeginCommandBuffer(
        VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo* pBeginInfo) {
    scoped_lock l(globalMutex);

    const auto& settings = deviceLayerSettings[getDispatchKey(commandBuffer)];
    auto* profiler = profilerMap[getDispatchKey(commandBuffer)];
    fprintf_wrapper(
            MemStatsLayer_outFile, "begin_command_buffer,%" PRIu64 ",%" PRIu64 "\n",
            getTimeStamp(), profiler->commandIndex++);

    VkResult res = deviceDispatchTables[getDispatchKey(commandBuffer)].BeginCommandBuffer(commandBuffer, pBeginInfo);

    if (res == VK_SUCCESS && settings.useProfiler) {
        auto queryAdder = std::make_shared<MemStatsLayer_QueryAdder>(
                commandBuffer, MemStatsLayerQueryType::COMMAND_BUFFER_SUBMISSION);
        if (queryAdder->isValid) {
            queryAdder->submitData->submissionQueryAdder = queryAdder;
        }
    }

    return res;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL MemStatsLayer_EndCommandBuffer(VkCommandBuffer commandBuffer) {
    scoped_lock l(globalMutex);

    const auto& settings = deviceLayerSettings[getDispatchKey(commandBuffer)];
    auto* profiler = profilerMap[getDispatchKey(commandBuffer)];
    fprintf_wrapper(
            MemStatsLayer_outFile, "end_command_buffer,%" PRIu64 ",%" PRIu64 "\n",
            getTimeStamp(), profiler->commandIndex++);

    if (settings.useProfiler) {
        auto submitData = profiler->findSubmitInFlight(commandBuffer);
        if (submitData) {
            submitData->submissionQueryAdder.reset();
        }
    }

    return deviceDispatchTables[getDispatchKey(commandBuffer)].EndCommandBuffer(commandBuffer);
}


VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_CmdUpdateBuffer(
        VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize dataSize,
        const void* pData) {
    scoped_lock l(globalMutex);

    uint64_t copySize = dataSize;
    auto* profiler = profilerMap[getDispatchKey(commandBuffer)];
    fprintf_wrapper(
            MemStatsLayer_outFile, "update_buffer,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",0x%p\n",
            getTimeStamp(), profiler->commandIndex++, copySize, dstBuffer);

    PROFILER_CREATE_QUERY(commandBuffer, MemStatsLayerQueryType::CMD_UPDATE_BUFFER);
    deviceDispatchTables[getDispatchKey(commandBuffer)].CmdUpdateBuffer(
            commandBuffer, dstBuffer, dstOffset, dataSize, pData);
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_CmdCopyBuffer(
        VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer, uint32_t regionCount,
        const VkBufferCopy* pRegions) {
    scoped_lock l(globalMutex);

    uint64_t copySize = 0;
    for (uint32_t i = 0; i < regionCount; i++) {
        copySize += pRegions[i].size;
    }
    auto* profiler = profilerMap[getDispatchKey(commandBuffer)];
    fprintf_wrapper(
            MemStatsLayer_outFile, "copy_buffer,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",0x%p,0x%p\n",
            getTimeStamp(), profiler->commandIndex++, copySize, srcBuffer, dstBuffer);

    PROFILER_CREATE_QUERY(commandBuffer, MemStatsLayerQueryType::CMD_COPY_BUFFER);
    deviceDispatchTables[getDispatchKey(commandBuffer)].CmdCopyBuffer(
            commandBuffer, srcBuffer, dstBuffer, regionCount, pRegions);
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_CmdCopyImage(
        VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout, VkImage dstImage,
        VkImageLayout dstImageLayout, uint32_t regionCount, const VkImageCopy* pRegions) {
    scoped_lock l(globalMutex);

    uint64_t copySize = 0;
    for (uint32_t i = 0; i < regionCount; i++) {
        copySize += pRegions[i].extent.width * pRegions[i].extent.height * pRegions[i].extent.depth;
    }
    auto* profiler = profilerMap[getDispatchKey(commandBuffer)];
    fprintf_wrapper(
            MemStatsLayer_outFile, "copy_image,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",0x%p,0x%p\n",
            getTimeStamp(), profiler->commandIndex++, copySize, srcImage, dstImage);

    PROFILER_CREATE_QUERY(commandBuffer, MemStatsLayerQueryType::CMD_COPY_IMAGE);
    deviceDispatchTables[getDispatchKey(commandBuffer)].CmdCopyImage(
            commandBuffer, srcImage, srcImageLayout, dstImage, dstImageLayout, regionCount, pRegions);
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_CmdCopyBufferToImage(
        VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkImage dstImage, VkImageLayout dstImageLayout,
        uint32_t regionCount, const VkBufferImageCopy* pRegions) {
    scoped_lock l(globalMutex);

    uint64_t copySize = 0;
    for (uint32_t i = 0; i < regionCount; i++) {
        copySize += pRegions[i].imageExtent.width * pRegions[i].imageExtent.height * pRegions[i].imageExtent.depth;
    }
    auto* profiler = profilerMap[getDispatchKey(commandBuffer)];
    fprintf_wrapper(
            MemStatsLayer_outFile, "copy_buffer_to_image,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",0x%p,0x%p\n",
            getTimeStamp(), profiler->commandIndex++, copySize, srcBuffer, dstImage);

    PROFILER_CREATE_QUERY(commandBuffer, MemStatsLayerQueryType::CMD_COPY_BUFFER_TO_IMAGE);
    deviceDispatchTables[getDispatchKey(commandBuffer)].CmdCopyBufferToImage(
            commandBuffer, srcBuffer, dstImage, dstImageLayout, regionCount, pRegions);
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_CmdCopyImageToBuffer(
        VkCommandBuffer commandBuffer, VkImage srcImage, VkImageLayout srcImageLayout, VkBuffer dstBuffer,
        uint32_t regionCount, const VkBufferImageCopy* pRegions) {
    scoped_lock l(globalMutex);

    uint64_t copySize = 0;
    for (uint32_t i = 0; i < regionCount; i++) {
        copySize += pRegions[i].imageExtent.width * pRegions[i].imageExtent.height * pRegions[i].imageExtent.depth;
    }
    auto* profiler = profilerMap[getDispatchKey(commandBuffer)];
    fprintf_wrapper(
            MemStatsLayer_outFile, "copy_image_to_buffer,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",0x%p,0x%p\n",
            getTimeStamp(), profiler->commandIndex++, copySize, srcImage, dstBuffer);

    PROFILER_CREATE_QUERY(commandBuffer, MemStatsLayerQueryType::CMD_COPY_IMAGE_TO_BUFFER);
    deviceDispatchTables[getDispatchKey(commandBuffer)].CmdCopyImageToBuffer(
            commandBuffer, srcImage, srcImageLayout, dstBuffer, regionCount, pRegions);
}


VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_CmdDispatch(
        VkCommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
    scoped_lock l(globalMutex);

    auto* profiler = profilerMap[getDispatchKey(commandBuffer)];
    fprintf_wrapper(
            MemStatsLayer_outFile, "dispatch,%" PRIu64 ",%" PRIu64 "\n",
            getTimeStamp(), profiler->commandIndex++);

    PROFILER_CREATE_QUERY(commandBuffer, MemStatsLayerQueryType::CMD_DISPATCH);
    deviceDispatchTables[getDispatchKey(commandBuffer)].CmdDispatch(
            commandBuffer, groupCountX, groupCountY, groupCountZ);
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_CmdDispatchIndirect(
        VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset) {
    scoped_lock l(globalMutex);

    auto* profiler = profilerMap[getDispatchKey(commandBuffer)];
    fprintf_wrapper(
            MemStatsLayer_outFile, "dispatch_indirect,%" PRIu64 ",%" PRIu64 "\n",
            getTimeStamp(), profiler->commandIndex++);

    PROFILER_CREATE_QUERY(commandBuffer, MemStatsLayerQueryType::CMD_DISPATCH_INDIRECT);
    deviceDispatchTables[getDispatchKey(commandBuffer)].CmdDispatchIndirect(commandBuffer, buffer, offset);
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_CmdDraw(
        VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
        uint32_t firstInstance) {
    scoped_lock l(globalMutex);

    auto* profiler = profilerMap[getDispatchKey(commandBuffer)];
    fprintf_wrapper(
            MemStatsLayer_outFile, "draw,%" PRIu64 ",%" PRIu64 "\n",
            getTimeStamp(), profiler->commandIndex++);

    PROFILER_CREATE_QUERY(commandBuffer, MemStatsLayerQueryType::CMD_DRAW);
    deviceDispatchTables[getDispatchKey(commandBuffer)].CmdDraw(
            commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_CmdDrawIndirect(
        VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride) {
    scoped_lock l(globalMutex);

    auto* profiler = profilerMap[getDispatchKey(commandBuffer)];
    fprintf_wrapper(
            MemStatsLayer_outFile, "draw_indirect,%" PRIu64 ",%" PRIu64 "\n",
            getTimeStamp(), profiler->commandIndex++);

    PROFILER_CREATE_QUERY(commandBuffer, MemStatsLayerQueryType::CMD_DRAW_INDIRECT);
    deviceDispatchTables[getDispatchKey(commandBuffer)].CmdDrawIndirect(
            commandBuffer, buffer, offset, drawCount, stride);
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_CmdDrawIndirectCount(
        VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer,
        VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride) {
    scoped_lock l(globalMutex);

    auto* profiler = profilerMap[getDispatchKey(commandBuffer)];
    fprintf_wrapper(
            MemStatsLayer_outFile, "draw_indirect_count,%" PRIu64 ",%" PRIu64 "\n",
            getTimeStamp(), profiler->commandIndex++);

    PROFILER_CREATE_QUERY(commandBuffer, MemStatsLayerQueryType::CMD_DRAW_INDIRECT_COUNT);
    deviceDispatchTables[getDispatchKey(commandBuffer)].CmdDrawIndirectCount(
            commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_CmdDrawIndexed(
        VkCommandBuffer commandBuffer, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex,
        int32_t vertexOffset, uint32_t firstInstance) {
    scoped_lock l(globalMutex);

    auto* profiler = profilerMap[getDispatchKey(commandBuffer)];
    fprintf_wrapper(
            MemStatsLayer_outFile, "draw_indexed,%" PRIu64 ",%" PRIu64 "\n",
            getTimeStamp(), profiler->commandIndex++);

    PROFILER_CREATE_QUERY(commandBuffer, MemStatsLayerQueryType::CMD_DRAW_INDEXED);
    deviceDispatchTables[getDispatchKey(commandBuffer)].CmdDrawIndexed(
            commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_CmdDrawIndexedIndirect(
        VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride) {
    scoped_lock l(globalMutex);

    auto* profiler = profilerMap[getDispatchKey(commandBuffer)];
    fprintf_wrapper(
            MemStatsLayer_outFile, "draw_indexed_indirect,%" PRIu64 ",%" PRIu64 "\n",
            getTimeStamp(), profiler->commandIndex++);

    PROFILER_CREATE_QUERY(commandBuffer, MemStatsLayerQueryType::CMD_DRAW_INDEXED_INDIRECT);
    deviceDispatchTables[getDispatchKey(commandBuffer)].CmdDrawIndexedIndirect(
            commandBuffer, buffer, offset, drawCount, stride);
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_CmdDrawIndexedIndirectCount(
        VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer,
        VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride) {
    scoped_lock l(globalMutex);

    auto* profiler = profilerMap[getDispatchKey(commandBuffer)];
    fprintf_wrapper(
            MemStatsLayer_outFile, "draw_indexed_indirect_count,%" PRIu64 ",%" PRIu64 "\n",
            getTimeStamp(), profiler->commandIndex++);

    PROFILER_CREATE_QUERY(commandBuffer, MemStatsLayerQueryType::CMD_DRAW_INDEXED_INDIRECT_COUNT);
    deviceDispatchTables[getDispatchKey(commandBuffer)].CmdDrawIndexedIndirectCount(
            commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_CmdDrawMeshTasksEXT(
        VkCommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
    scoped_lock l(globalMutex);

    auto* profiler = profilerMap[getDispatchKey(commandBuffer)];
    fprintf_wrapper(
            MemStatsLayer_outFile, "cmd_draw_mesh_tasks,%" PRIu64 ",%" PRIu64 "\n",
            getTimeStamp(), profiler->commandIndex++);

    PROFILER_CREATE_QUERY(commandBuffer, MemStatsLayerQueryType::CMD_DRAW_MESH_TASKS);
    deviceDispatchTables[getDispatchKey(commandBuffer)].CmdDrawMeshTasksEXT(
            commandBuffer, groupCountX, groupCountY, groupCountZ);
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_CmdDrawMeshTasksIndirectEXT(
        VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride) {
    scoped_lock l(globalMutex);

    auto* profiler = profilerMap[getDispatchKey(commandBuffer)];
    fprintf_wrapper(
            MemStatsLayer_outFile, "cmd_draw_mesh_tasks_indirect,%" PRIu64 ",%" PRIu64 "\n",
            getTimeStamp(), profiler->commandIndex++);

    PROFILER_CREATE_QUERY(commandBuffer, MemStatsLayerQueryType::CMD_DRAW_MESH_TASKS_INDIRECT);
    deviceDispatchTables[getDispatchKey(commandBuffer)].CmdDrawMeshTasksIndirectEXT(
            commandBuffer, buffer, offset, drawCount, stride);
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_CmdDrawMeshTasksIndirectCountEXT(
        VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset, VkBuffer countBuffer,
        VkDeviceSize countBufferOffset, uint32_t maxDrawCount, uint32_t stride) {
    scoped_lock l(globalMutex);

    auto* profiler = profilerMap[getDispatchKey(commandBuffer)];
    fprintf_wrapper(
            MemStatsLayer_outFile, "cmd_draw_mesh_tasks_indirect_count,%" PRIu64 ",%" PRIu64 "\n",
            getTimeStamp(), profiler->commandIndex++);

    PROFILER_CREATE_QUERY(commandBuffer, MemStatsLayerQueryType::CMD_DRAW_MESH_TASKS_INDIRECT_COUNT);
    deviceDispatchTables[getDispatchKey(commandBuffer)].CmdDrawMeshTasksIndirectCountEXT(
            commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_CmdTraceRaysKHR(
        VkCommandBuffer commandBuffer, const VkStridedDeviceAddressRegionKHR* pRaygenShaderBindingTable,
        const VkStridedDeviceAddressRegionKHR* pMissShaderBindingTable,
        const VkStridedDeviceAddressRegionKHR* pHitShaderBindingTable,
        const VkStridedDeviceAddressRegionKHR* pCallableShaderBindingTable,
        uint32_t width, uint32_t height, uint32_t depth) {
    scoped_lock l(globalMutex);

    auto* profiler = profilerMap[getDispatchKey(commandBuffer)];
    fprintf_wrapper(
            MemStatsLayer_outFile, "trace_rays,%" PRIu64 ",%" PRIu64 "\n",
            getTimeStamp(), profiler->commandIndex++);

    PROFILER_CREATE_QUERY(commandBuffer, MemStatsLayerQueryType::CMD_TRACE_RAYS);
    deviceDispatchTables[getDispatchKey(commandBuffer)].CmdTraceRaysKHR(
            commandBuffer, pRaygenShaderBindingTable, pMissShaderBindingTable, pHitShaderBindingTable,
            pCallableShaderBindingTable, width, height, depth);
}

VK_LAYER_EXPORT VKAPI_ATTR void VKAPI_CALL MemStatsLayer_CmdTraceRaysIndirectKHR(
        VkCommandBuffer commandBuffer, const VkStridedDeviceAddressRegionKHR* pRaygenShaderBindingTable,
        const VkStridedDeviceAddressRegionKHR* pMissShaderBindingTable,
        const VkStridedDeviceAddressRegionKHR* pHitShaderBindingTable,
        const VkStridedDeviceAddressRegionKHR* pCallableShaderBindingTable,
        VkDeviceAddress indirectDeviceAddress) {
    scoped_lock l(globalMutex);

    auto* profiler = profilerMap[getDispatchKey(commandBuffer)];
    fprintf_wrapper(
            MemStatsLayer_outFile, "trace_rays_indirect,%" PRIu64 ",%" PRIu64 "\n",
            getTimeStamp(), profiler->commandIndex++);

    PROFILER_CREATE_QUERY(commandBuffer, MemStatsLayerQueryType::CMD_TRACE_RAYS_INDIRECT);
    deviceDispatchTables[getDispatchKey(commandBuffer)].CmdTraceRaysIndirectKHR(
            commandBuffer, pRaygenShaderBindingTable, pMissShaderBindingTable, pHitShaderBindingTable,
            pCallableShaderBindingTable, indirectDeviceAddress);
}


VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL MemStatsLayer_QueueSubmit(
        VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence) {
    scoped_lock l(globalMutex);

    VkResult res = deviceDispatchTables[getDispatchKey(queue)].QueueSubmit(queue, submitCount, pSubmits, fence);
    if (res != VK_SUCCESS) {
        return res;
    }

    const auto& settings = deviceLayerSettings[getDispatchKey(queue)];
    auto* profiler = profilerMap[getDispatchKey(queue)];

    auto timestamp = getTimeStamp();
    fprintf_wrapper(MemStatsLayer_outFile, "submit,%" PRIu64 "\n", timestamp);

    if (settings.useProfiler) {
        for (uint32_t submitIdx = 0; submitIdx < submitCount; submitIdx++) {
            for (uint32_t cmdBufIdx = 0; cmdBufIdx < pSubmits[submitIdx].commandBufferCount; cmdBufIdx++) {
                auto submitData = profiler->findSubmitInFlight(pSubmits[submitIdx].pCommandBuffers[cmdBufIdx]);
                if (submitData) {
                    submitData->submitted = true;
                    submitData->submitTimestamp = timestamp;
                    submitData->queue = queue;
                    submitData->fence = fence;
                    submitData->signalSemaphores.resize(pSubmits[submitIdx].signalSemaphoreCount);
                    for (uint32_t signalSemIdx = 0; signalSemIdx < pSubmits[submitIdx].signalSemaphoreCount; signalSemIdx++) {
                        submitData->signalSemaphores[signalSemIdx] = pSubmits[submitIdx].pSignalSemaphores[signalSemIdx];
                    }
                    submitData->waitSemaphores.resize(pSubmits[submitIdx].waitSemaphoreCount);
                    for (uint32_t waitSemIdx = 0; waitSemIdx < pSubmits[submitIdx].waitSemaphoreCount; waitSemIdx++) {
                        submitData->waitSemaphores[waitSemIdx] = pSubmits[submitIdx].pWaitSemaphores[waitSemIdx];
                    }
                    auto* pNext = pSubmits[submitIdx].pNext;
                    while (pNext) {
                        auto* baseStructurePtr = reinterpret_cast<const VkBaseInStructure*>(pNext);
                        if (baseStructurePtr->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) {
                            auto* timelineSemaphoreSubmitInfo = reinterpret_cast<const VkTimelineSemaphoreSubmitInfo*>(pNext);
                            submitData->signalSemaphoreValues.resize(timelineSemaphoreSubmitInfo->signalSemaphoreValueCount);
                            for (uint32_t signalSemIdx = 0; signalSemIdx < timelineSemaphoreSubmitInfo->signalSemaphoreValueCount; signalSemIdx++) {
                                submitData->signalSemaphoreValues[signalSemIdx] = timelineSemaphoreSubmitInfo->pSignalSemaphoreValues[signalSemIdx];
                            }
                            submitData->waitSemaphoreValues.resize(timelineSemaphoreSubmitInfo->waitSemaphoreValueCount);
                            for (uint32_t waitSemIdx = 0; waitSemIdx < timelineSemaphoreSubmitInfo->waitSemaphoreValueCount; waitSemIdx++) {
                                submitData->waitSemaphoreValues[waitSemIdx] = timelineSemaphoreSubmitInfo->pWaitSemaphoreValues[waitSemIdx];
                            }
                            break;
                        }
                        pNext = baseStructurePtr->pNext;
                    }
                    submitData->signalSemaphoreValues.resize(submitData->signalSemaphores.size(), 1);
                    submitData->waitSemaphoreValues.resize(submitData->waitSemaphores.size(), 1);
                }
            }
        }
    }

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

    fprintf_wrapper(MemStatsLayer_outFile, "acquire_next_image,%" PRIu64 "\n", getTimeStamp());

    return res;
}


VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL MemStatsLayer_QueuePresentKHR(
        VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    scoped_lock l(globalMutex);

    VkResult res = deviceDispatchTables[getDispatchKey(queue)].QueuePresentKHR(queue, pPresentInfo);
    if (res != VK_SUCCESS) {
        return res;
    }

    auto* profiler = profilerMap[getDispatchKey(queue)];
    fprintf_wrapper(
            MemStatsLayer_outFile, "present,%" PRIu64 ",%" PRIu64 "\n",
            getTimeStamp(), profiler->currentFrameIndex);
    profiler->currentFrameIndex++;

    return res;
}


static void onSemaphoreSignaled(void* dispatchKey, VkSemaphore semaphore, uint64_t semaphoreValue);

static void onSubmitDataFinished(void* dispatchKey, MemStatsLayer_SubmitData* submitData, bool transitive) {
    auto* profiler = profilerMap[dispatchKey];
    submitData->readBack();
    if (transitive) {
        for (size_t waitSemIdx = 0; waitSemIdx < submitData->waitSemaphores.size(); waitSemIdx++) {
            onSemaphoreSignaled(
                    dispatchKey, submitData->waitSemaphores.at(waitSemIdx),
                    submitData->waitSemaphoreValues.at(waitSemIdx));
        }
    }
    submitData->reset();
    profiler->freeSubmitDataList.push_back(submitData);
}

static void onSemaphoreSignaled(void* dispatchKey, VkSemaphore semaphore, uint64_t semaphoreValue) {
    auto* profiler = profilerMap[dispatchKey];
    std::vector<MemStatsLayer_SubmitData*> finishedSubmitDataList;
    for (auto it = profiler->submitsInFlight.begin(); it != profiler->submitsInFlight.end(); ++it) {
        auto& submitData = *it;
        for (size_t signalSemIdx = 0; signalSemIdx < submitData->signalSemaphores.size(); signalSemIdx++) {
            if (submitData->submitted && submitData->signalSemaphores.at(signalSemIdx) == semaphore
                    && submitData->signalSemaphoreValues.at(signalSemIdx) <= semaphoreValue) {
                profiler->submitsInFlight.erase(it);
                finishedSubmitDataList.push_back(submitData);
                break;
            }
        }
    }
    for (auto* submitData : finishedSubmitDataList) {
        onSubmitDataFinished(dispatchKey, submitData, true);
    }
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL MemStatsLayer_WaitForFences(
        VkDevice device, uint32_t fenceCount, const VkFence* pFences, VkBool32 waitAll, uint64_t timeout) {
    scoped_lock l(globalMutex);

    VkResult res = deviceDispatchTables[getDispatchKey(device)].WaitForFences(
            device, fenceCount, pFences, waitAll, timeout);
    if (res != VK_SUCCESS) {
        return res;
    }

    void* dispatchKey = getDispatchKey(device);
    const auto& settings = deviceLayerSettings[dispatchKey];
    auto* profiler = profilerMap[dispatchKey];
    if (settings.useProfiler && (waitAll || fenceCount == 1)) {
        for (uint32_t fenceIdx = 0; fenceIdx < fenceCount; fenceIdx++) {
            for (auto it = profiler->submitsInFlight.begin(); it != profiler->submitsInFlight.end(); ++it) {
                auto* submitData = *it;
                if (submitData->submitted && submitData->fence == pFences[fenceIdx]) {
                    profiler->submitsInFlight.erase(it);
                    onSubmitDataFinished(dispatchKey, submitData, true);
                    break;
                }
            }
        }
    }

    return res;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL MemStatsLayer_WaitSemaphores(
        VkDevice device, const VkSemaphoreWaitInfo* pWaitInfo, uint64_t timeout) {
    scoped_lock l(globalMutex);

    VkResult res = deviceDispatchTables[getDispatchKey(device)].WaitSemaphores(device, pWaitInfo, timeout);
    if (res != VK_SUCCESS) {
        return res;
    }

    void* dispatchKey = getDispatchKey(device);
    const auto& settings = deviceLayerSettings[dispatchKey];
    if (settings.useProfiler) {
        for (uint32_t semIdx = 0; semIdx < pWaitInfo->semaphoreCount; semIdx++) {
            onSemaphoreSignaled(dispatchKey, pWaitInfo->pSemaphores[semIdx], pWaitInfo->pValues[semIdx]);
        }
    }

    return res;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL MemStatsLayer_DeviceWaitIdle(VkDevice device) {
    scoped_lock l(globalMutex);

    VkResult res = deviceDispatchTables[getDispatchKey(device)].DeviceWaitIdle(device);
    if (res != VK_SUCCESS) {
        return res;
    }

    void* dispatchKey = getDispatchKey(device);
    const auto& settings = deviceLayerSettings[dispatchKey];
    auto* profiler = profilerMap[dispatchKey];
    if (settings.useProfiler) {
        uint64_t timestamp = getTimeStamp();
        bool changed = false;
        do {
            changed = false;
            for (auto it = profiler->submitsInFlight.begin(); it != profiler->submitsInFlight.end(); ++it) {
                auto* submitData = *it;
                if (submitData->submitted && submitData->submitTimestamp <= timestamp) {
                    auto itOld = it;
                    profiler->submitsInFlight.erase(itOld);
                    changed = true;
                    onSubmitDataFinished(dispatchKey, submitData, false);
                    break;
                }
            }
        } while (changed);
    }

    return res;
}

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL MemStatsLayer_QueueWaitIdle(VkQueue queue) {
    scoped_lock l(globalMutex);

    VkResult res = deviceDispatchTables[getDispatchKey(queue)].QueueWaitIdle(queue);
    if (res != VK_SUCCESS) {
        return res;
    }

    void* dispatchKey = getDispatchKey(queue);
    const auto& settings = deviceLayerSettings[dispatchKey];
    auto* profiler = profilerMap[dispatchKey];
    if (settings.useProfiler) {
        uint64_t timestamp = getTimeStamp();
        bool changed = false;
        do {
            changed = false;
            for (auto it = profiler->submitsInFlight.begin(); it != profiler->submitsInFlight.end(); ++it) {
                auto* submitData = *it;
                if (submitData->submitted && submitData->submitTimestamp <= timestamp && submitData->queue == queue) {
                    auto itOld = it;
                    profiler->submitsInFlight.erase(itOld);
                    changed = true;
                    onSubmitDataFinished(dispatchKey, submitData, false);
                    break;
                }
            }
        } while (changed);
    }

    return res;
}


#define GETPROCADDR_VK(func) if (strcmp(pName, "vk" #func) == 0) { return (PFN_vkVoidFunction)&MemStatsLayer_##func; }

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL MemStatsLayer_GetDeviceProcAddr(VkDevice device, const char* pName) {
    MemStatsLayerSettingsDevice settings;
    {
        scoped_lock l(globalMutex);
        settings = deviceLayerSettings[getDispatchKey(device)];
    }

    GETPROCADDR_VK(GetDeviceProcAddr);
    GETPROCADDR_VK(EnumerateDeviceLayerProperties);
    GETPROCADDR_VK(EnumerateDeviceExtensionProperties);
    GETPROCADDR_VK(CreateDevice);
    GETPROCADDR_VK(DestroyDevice);
    GETPROCADDR_VK(AllocateMemory);
    GETPROCADDR_VK(FreeMemory);
    GETPROCADDR_VK(BindBufferMemory);
    GETPROCADDR_VK(BindImageMemory);
    GETPROCADDR_VK(DestroyBuffer);
    GETPROCADDR_VK(DestroyImage);
    GETPROCADDR_VK(BeginCommandBuffer);
    GETPROCADDR_VK(EndCommandBuffer);
    GETPROCADDR_VK(CmdUpdateBuffer);
    GETPROCADDR_VK(CmdCopyBuffer);
    GETPROCADDR_VK(CmdCopyImage);
    GETPROCADDR_VK(CmdCopyBufferToImage);
    GETPROCADDR_VK(CmdCopyImageToBuffer);
    GETPROCADDR_VK(CmdDispatch);
    GETPROCADDR_VK(CmdDispatchIndirect);
    GETPROCADDR_VK(CmdDraw);
    GETPROCADDR_VK(CmdDrawIndirect);
    GETPROCADDR_VK(CmdDrawIndirectCount);
    GETPROCADDR_VK(CmdDrawIndexed);
    GETPROCADDR_VK(CmdDrawIndexedIndirect);
    GETPROCADDR_VK(CmdDrawIndexedIndirectCount);
    if (settings.useMeshShaderEXT) {
        GETPROCADDR_VK(CmdDrawMeshTasksEXT);
        GETPROCADDR_VK(CmdDrawMeshTasksIndirectEXT);
        GETPROCADDR_VK(CmdDrawMeshTasksIndirectCountEXT);
    }
    if (settings.useRayTracingPipelineKHR) {
        GETPROCADDR_VK(CmdTraceRaysKHR);
        GETPROCADDR_VK(CmdTraceRaysIndirectKHR);
    }
    GETPROCADDR_VK(QueueSubmit);
    GETPROCADDR_VK(AcquireNextImageKHR);
    GETPROCADDR_VK(QueuePresentKHR);
    GETPROCADDR_VK(WaitForFences);
    GETPROCADDR_VK(WaitSemaphores);
    GETPROCADDR_VK(DeviceWaitIdle);
    GETPROCADDR_VK(QueueWaitIdle);

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
    GETPROCADDR_VK(BindBufferMemory);
    GETPROCADDR_VK(BindImageMemory);
    GETPROCADDR_VK(DestroyBuffer);
    GETPROCADDR_VK(DestroyImage);
    GETPROCADDR_VK(BeginCommandBuffer);
    GETPROCADDR_VK(EndCommandBuffer);
    GETPROCADDR_VK(CmdUpdateBuffer);
    GETPROCADDR_VK(CmdCopyBuffer);
    GETPROCADDR_VK(CmdCopyImage);
    GETPROCADDR_VK(CmdCopyBufferToImage);
    GETPROCADDR_VK(CmdCopyImageToBuffer);
    GETPROCADDR_VK(CmdDraw);
    GETPROCADDR_VK(CmdDrawIndirect);
    GETPROCADDR_VK(CmdDrawIndirectCount);
    GETPROCADDR_VK(CmdDrawIndexed);
    GETPROCADDR_VK(CmdDrawIndexedIndirect);
    GETPROCADDR_VK(CmdDrawIndexedIndirectCount);
    GETPROCADDR_VK(CmdDrawMeshTasksEXT);
    GETPROCADDR_VK(CmdDrawMeshTasksIndirectEXT);
    GETPROCADDR_VK(CmdDrawMeshTasksIndirectCountEXT);
    GETPROCADDR_VK(CmdTraceRaysKHR);
    GETPROCADDR_VK(CmdTraceRaysIndirectKHR);
    GETPROCADDR_VK(QueueSubmit);
    GETPROCADDR_VK(AcquireNextImageKHR);
    GETPROCADDR_VK(QueuePresentKHR);
    GETPROCADDR_VK(WaitForFences);
    GETPROCADDR_VK(WaitSemaphores);
    GETPROCADDR_VK(DeviceWaitIdle);
    GETPROCADDR_VK(QueueWaitIdle);

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
        fprintf_save(MemStatsLayer_outFile, "version,%d,%d\n", 0, FILE_FORMAT_VERSION_NUMBER);
        initializeTimer();

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
        fprintf_save(MemStatsLayer_outFile, "version,%d,%d\n", 0, FILE_FORMAT_VERSION_NUMBER);

        initializeTimer();

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
    initializeTimer();
    fprintf_save(MemStatsLayer_outFile, "version,%d,%d\n", 0, FILE_FORMAT_VERSION_NUMBER);

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
    if (hooked_alloc || !MemStatsLayer_outFile || MemStatsLayer_pid != getpid()) {
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
    if (hooked_alloc || !MemStatsLayer_outFile || MemStatsLayer_pid != getpid()) {
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
    if (hooked_alloc || !MemStatsLayer_outFile || MemStatsLayer_pid != getpid()) {
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
    if (hooked_alloc || !MemStatsLayer_outFile || MemStatsLayer_pid != getpid()) {
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
