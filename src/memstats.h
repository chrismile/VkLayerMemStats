/*
 * BSD 2-Clause License
 *
 * Copyright (c) 2026, Christoph Neuhauser
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
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

#ifndef VKLAYERMEMSTATS_MEMSTATS_H
#define VKLAYERMEMSTATS_MEMSTATS_H

#ifdef MEMSTATS_LINK_TIME

#ifdef _WIN32
#define MEMSTATS_EXPORT extern "C" __declspec(dllimport)
#else
#define MEMSTATS_EXPORT extern "C"
#endif

MEMSTATS_EXPORT void memstats_printf(const char* format, ...);
MEMSTATS_EXPORT uint64_t memstats_gettimestamp();

#else // !defined(MEMSTATS_LINK_TIME)

extern void (*memstats_printf)(const char* format, ...);
extern uint64_t (*memstats_gettimestamp)();

#ifdef MEMSTATS_IMPL

void (*memstats_printf)(const char* format, ...) = nullptr;
uint64_t (*memstats_gettimestamp)() = nullptr;

#include <vector>
#include <string>

#if defined(__linux__)
#include <dlfcn.h>
#include <unistd.h>
void* g_memstatsLibrary = nullptr;
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define dlsym GetProcAddress
HMODULE g_memstatsLibrary = nullptr;
#endif

/**
 * Converts strings like "This is a test!" with separator ' ' to { "This", "is", "a", "test!" }.
 * @tparam InputIterator The list class to use.
 * @param stringObject The string to split.
 * @param separator The separator to use for splitting.
 * @param listObject The split parts.
 */
template<class InputIterator>
void MemStatsLayer_splitString(const std::string& stringObject, char separator, InputIterator& listObject) {
    std::string buffer;
    for (char c : stringObject) {
        if (c != separator) {
            buffer += c;
        } else {
            if (buffer.length() > 0) {
                listObject.push_back(buffer);
                buffer = "";
            }
        }
    }
    if (buffer.length() > 0) {
        listObject.push_back(buffer);
        buffer = "";
    }
}

static bool memstats_load() {
    typedef void ( *PFN_memstats_printf )( const char* format, ... );
    typedef void ( *PFN_memstats_gettimestamp )( const char* format, ... );

    std::vector<std::string> pathList;
#ifdef _MSC_VER
    char* pathEnvVar = nullptr;
    size_t stringSize = 0;
    if (_dupenv_s(&pathEnvVar, &stringSize, "VK_ADD_LAYER_PATH") != 0) {
        pathEnvVar = nullptr;
    }
    if (pathEnvVar) {
        MemStatsLayer_splitString(pathEnvVar, ';', pathList);
    }
    free(pathEnvVar);
    pathEnvVar = nullptr;
#elif defined(_WIN32)
    const char* pathEnvVar = getenv("VK_ADD_LAYER_PATH");
    if (pathEnvVar) {
        MemStatsLayer_splitString(pathEnvVar, ';', pathList);
    }
#else
    const char* pathEnvVar = getenv("VK_ADD_LAYER_PATH");
    if (pathEnvVar) {
        MemStatsLayer_splitString(pathEnvVar, ':', pathList);
    }
#endif

#if defined(__linux__)
    g_memstatsLibrary = dlopen("libVkLayer_memstats.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_memstatsLibrary) {
        for (const std::string& path : pathList) {
            std::string libraryPath = path;
            if (!libraryPath.empty() && libraryPath.back() != '/') {
                libraryPath += '/';
                libraryPath += "libVkLayer_memstats.so";
            }
            g_memstatsLibrary = dlopen(libraryPath.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (g_memstatsLibrary) {
                break;
            }
        }
        if (!g_memstatsLibrary) {
            return false;
        }
    }
#elif defined(_WIN32)
    g_memstatsLibrary = LoadLibraryA("VkLayer_memstats.dll");
    if (!g_memstatsLibrary) {
        for (const std::string& path : pathList) {
            std::string libraryPath = path;
            if (!libraryPath.empty() && libraryPath.back() != '/' && libraryPath.back() != '\\') {
                libraryPath += '/';
                libraryPath += "VkLayer_memstats.dll";
            }
            g_memstatsLibrary = LoadLibraryA(libraryPath.c_str());
            if (g_memstatsLibrary) {
                break;
            }
        }
        if (!g_memstatsLibrary) {
            return false;
        }
    }
#endif

    memstats_printf = PFN_memstats_printf(dlsym(g_memstatsLibrary, "memstats_printf"));
    memstats_gettimestamp = PFN_memstats_gettimestamp(dlsym(g_memstatsLibrary, "memstats_gettimestamp"));

    return true;
}

static void memstats_unload() {
    memstats_printf = nullptr;
    memstats_gettimestamp = nullptr;

#if defined(__linux__)
    dlclose(g_memstatsLibrary);
#elif defined(_WIN32)
    FreeLibrary(g_memstatsLibrary);
#endif
    g_memstatsLibrary = {};
}

#endif

#endif  // MEMSTATS_LINK_TIME

#endif //VKLAYERMEMSTATS_MEMSTATS_H
