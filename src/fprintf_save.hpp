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

#ifndef VKLAYERMEMSTATS_FPRINTF_SAVE_HPP
#define VKLAYERMEMSTATS_FPRINTF_SAVE_HPP

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
 * - %p: Pointer (assumes 64-bit pointer size); standard library dependent (inclusion of "0x", leading zeros)
 * - %llx: uintptr_t (assumes 64-bit pointer size); no leading zeros nor "0x", lower case hexadecimal string
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
                buffer[bufPos++] = '0';
                if (bufPos < sizeof(buffer)) {
                    buffer[bufPos++] = 'x';
                }
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
            } else if (format[fmtPos] == 'l' && format[fmtPos + 1] == 'l' && format[fmtPos + 2] == 'x') {
                // Unsigned 64-bit integer
                fmtPos += 2;
                uintptr_t num = va_arg(vlist, uintptr_t);
                _ui64toa_s(num, temp, _countof(temp), 16);
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

#endif //VKLAYERMEMSTATS_FPRINTF_SAVE_HPP
