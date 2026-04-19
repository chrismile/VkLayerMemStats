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

#include <string>
#include <cinttypes>
#include <cstdarg>

#include <gtest/gtest.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <strsafe.h>
#endif

#include "fprintf_save.hpp"

void testEquality(const char* expected, const char* format, ...) {
#ifdef _WIN32
    FILE* file = nullptr;
    fopen_s(&file, "test.txt", "w");
#else
    FILE* file = fopen("test.txt", "w");
#endif
    va_list vlist;
    va_start(vlist, format);
    vfprintf_save(file, format, vlist);
    va_end(vlist);
    fclose(file);

#ifdef _WIN32
    fopen_s(&file, "test.txt", "r");
#else
    file = fopen("test.txt", "r");
#endif
    fseek(file, 0, SEEK_END);
    long fsize = ftell(file);
    fseek(file, 0, SEEK_SET);  /* same as rewind(f); */
    std::string readString(fsize, '\0');
    fread(readString.data(), fsize, 1, file);
    fclose(file);
    EXPECT_EQ(std::string(expected), readString);
}

TEST(FormatTest, TestInt) {
    testEquality("int12", "int%d", 12);
}

TEST(FormatTest, TestIntNegative) {
    testEquality("int-1", "int%d", -1);
}

TEST(FormatTest, TestUint) {
    testEquality("int12", "int%u", 12);
}

TEST(FormatTest, TestChar) {
    testEquality("a char", "%c char", 'a');
}

TEST(FormatTest, TestString) {
    testEquality("hello world", "hello %s", "world");
}

TEST(FormatTest, TestUint64) {
    testEquality("64-bit 1234567891245 uint", "64-bit %" PRIu64 " uint", 1234567891245ull);
}

TEST(FormatTest, TestPointer) {
    testEquality("0x123a456789", "0x%" PRIxPTR, static_cast<uintptr_t>(0x123a456789ull));
}

TEST(FormatTest, TestPointerZero) {
    testEquality("0x0", "0x%" PRIxPTR, static_cast<uintptr_t>(0x0ull));
}

TEST(FormatTest, TestPointerUpperCase) {
    testEquality("0x123A456789", "0x%" PRIXPTR, static_cast<uintptr_t>(0x123a456789ull));
}

TEST(FormatTest, TestMacrosUint64) {
#ifdef _WIN32
    EXPECT_STREQ(PRIu64, "llu");
#else
    EXPECT_STREQ(PRIu64, "lu");
#endif
}

TEST(FormatTest, TestMacrosPointer) {
#ifdef _WIN32
    EXPECT_STREQ(PRIxPTR, "llx");
#else
    EXPECT_STREQ(PRIxPTR, "lx");
#endif
}

TEST(FormatTest, TestPointerImpl) {
#if (defined(_WIN32) && defined(HOOK_MALLOC))
    testEquality("0x00000000000123a4", "%p", reinterpret_cast<void*>(0x123a4ull));
#elif _MSC_VER
    testEquality("0x00000000000123A4", "0x%p", reinterpret_cast<void*>(0x123a4ull));
#elif defined(__MINGW32__)
    testEquality("0x00000000000123a4", "0x%p", reinterpret_cast<void*>(0x123a4ull));
#else
    testEquality("0x123a4", "%p", reinterpret_cast<void*>(0x123a4ull));
#endif
}
