/*
 * This file is based on sample code from the Microsoft Research Detours Package.
 * The original code is licensed under the MIT license.
 * All changes are licensed under the terms of the BSD 3-Clause License.
 * Please find the text of the two licenses below.
 *
 * Copyright (c) Microsoft Corporation.
 *
 * MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED *AS IS*, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
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

#include <cstdio>
#include <windows.h>
#include <detours.h>
#include <filesystem>
#include <strsafe.h>
#include <shlwapi.h>

/*
 * This code verifies that the named DLL has been configured correctly to be imported into the target process.
 * DLLs must export a function with ordinal #1 so that the import table touch-up magic works.
 */
struct ExportContext {
    BOOL fHasOrdinal1;
    ULONG nExports;
};

static BOOL CALLBACK ExportCallback(
        _In_opt_ PVOID pContext, _In_ ULONG nOrdinal, _In_opt_ LPCSTR pszSymbol, _In_opt_ PVOID pbTarget) {
    (void)pContext;
    (void)pbTarget;
    (void)pszSymbol;

    auto *pec = static_cast<ExportContext*>(pContext);

    if (nOrdinal == 1) {
        pec->fHasOrdinal1 = TRUE;
    }
    pec->nExports++;

    return TRUE;
}

int CDECL main(int argc, char** argv) {
    if (argc < 2) {
        printf("preload.exe: Error: Expected executable as argument.\n");
        return 1;
    }

    // Replace name of preload executable with name of Vulkan layer library.
    LPCSTR libraryName = "VkLayer_memstats.dll";
    CHAR szDllPath[1024];
    DWORD modulePathEnd = GetModuleFileNameA(
            nullptr, szDllPath, static_cast<DWORD>(_countof(szDllPath) - strlen(libraryName)));
    DWORD lastBackslashPosition = 0;
    for (DWORD i = modulePathEnd; i > 0; i--) {
        if (szDllPath[i] == '\\') {
            lastBackslashPosition = i + 1;
            break;
        }
    }
    strcpy_s(szDllPath + lastBackslashPosition, strlen(libraryName) + 1, libraryName);

    // Check if the Vulkan layer library exists (should never fail unless the user deleted it).
    if (!PathFileExistsA(szDllPath)) {
        printf("withdll.exe: Error: %s could not be found in the preload executable directory.\n", szDllPath);
        return 9002;
    }

    HMODULE hDll = LoadLibraryExA(szDllPath, nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (hDll == nullptr) {
        printf("preload.exe: Error: %s failed to load (error %ld).\n", szDllPath, GetLastError());
        return 9003;
    }

    ExportContext ec{};
    ec.fHasOrdinal1 = FALSE;
    ec.nExports = 0;
    DetourEnumerateExports(hDll, &ec, ExportCallback);
    FreeLibrary(hDll);

    if (!ec.fHasOrdinal1) {
        printf("preload.exe: Error: %s does not export ordinal #1.\n", szDllPath);
        printf("             See help entry DetourCreateProcessWithDllEx in Detours.chm.\n");
        return 9004;
    }

    // Assemble executable name from command line arguments.
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    CHAR szCommand[2048];
    CHAR szExe[1024];
    CHAR szFullExe[1024] = "\0";
    PCHAR pszFileExe = nullptr;

    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    szCommand[0] = L'\0';

    int arg = 1;
    StringCchCopyA(szExe, sizeof(szExe), argv[arg]);
    for (; arg < argc; arg++) {
        if (strchr(argv[arg], ' ') != nullptr || strchr(argv[arg], '\t') != nullptr) {
            StringCchCatA(szCommand, sizeof(szCommand), "\"");
            StringCchCatA(szCommand, sizeof(szCommand), argv[arg]);
            StringCchCatA(szCommand, sizeof(szCommand), "\"");
        }
        else {
            StringCchCatA(szCommand, sizeof(szCommand), argv[arg]);
        }

        if (arg + 1 < argc) {
            StringCchCatA(szCommand, sizeof(szCommand), " ");
        }
    }

    if (arg + 1 < argc) {
        StringCchCatA(szCommand, sizeof(szCommand), " ");
    }

    printf("preload.exe: Starting: `%s'\n", szCommand);
    printf("preload.exe:   with `%s'\n", szDllPath);
    fflush(stdout);

    DWORD dwFlags = CREATE_DEFAULT_ERROR_MODE | CREATE_SUSPENDED;

    // The block below would need to append to GetEnvironmentStrings.
    /*CHAR chNewEnv[2048]; // 2048 is safe bounds; we have one up to 1024 path and the two short strings below.
    LPCSTR vkLoaderPathString = "set VK_ADD_LAYER_PATH=";
    LPCSTR vkLoaderLayerString = "set VK_LOADER_LAYERS_ENABLE=*memstats";
    auto lpszCurrentVariable = chNewEnv;
    StringCchCopyA(chNewEnv, 1024, vkLoaderPathString);
    lpszCurrentVariable += lstrlenA(lpszCurrentVariable);
    memcpy(lpszCurrentVariable, szDllPath, lastBackslashPosition - 1);
    lpszCurrentVariable += lastBackslashPosition - 1;
    lpszCurrentVariable[0] = '\0';
    lpszCurrentVariable += 1;
    StringCchCopyA(lpszCurrentVariable, 1024, vkLoaderLayerString);
    lpszCurrentVariable += lstrlenA(lpszCurrentVariable) + 1;
    lpszCurrentVariable[0] = '\0';*/

    // Set necessary environment variables for the Vulkan layer to be loaded.
    CHAR szLibraryPath[1024];
    memcpy(szLibraryPath, szDllPath, lastBackslashPosition - 1);
    szLibraryPath[lastBackslashPosition - 1] = '\0';
    SetEnvironmentVariableA("VK_ADD_LAYER_PATH", szLibraryPath);
    SetEnvironmentVariableA("VK_LOADER_LAYERS_ENABLE", "*memstats");

    SetLastError(0);
    SearchPathA(nullptr, szExe, ".exe", ARRAYSIZE(szFullExe), szFullExe, &pszFileExe);
    LPCSTR rlpDlls = szDllPath;
    if (!DetourCreateProcessWithDllsA(
            szFullExe[0] ? szFullExe : nullptr, szCommand,
            nullptr, nullptr, TRUE, dwFlags, nullptr, nullptr,
            &si, &pi, 1, &rlpDlls, nullptr)) {
        DWORD dwError = GetLastError();
        printf("preload.exe: DetourCreateProcessWithDllEx failed: %ld\n", dwError);
        if (dwError == ERROR_INVALID_HANDLE) {
#if DETOURS_64BIT
            printf("preload.exe: Can't detour a 32-bit target process from a 64-bit parent process.\n");
#else
            printf("preload.exe: Can't detour a 64-bit target process from a 32-bit parent process.\n");
#endif
        }
        ExitProcess(9009);
    }

    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD dwResult = 0;
    if (!GetExitCodeProcess(pi.hProcess, &dwResult)) {
        printf("preload.exe: GetExitCodeProcess failed: %ld\n", GetLastError());
        return 9010;
    }

    return static_cast<int>(dwResult);
}
