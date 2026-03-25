#
# BSD 3-Clause License
#
# Copyright (c) 2026, Christoph Neuhauser
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice, this
#    list of conditions and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright notice,
#    this list of conditions and the following disclaimer in the documentation
#    and/or other materials provided with the distribution.
#
# 3. Neither the name of the copyright holder nor the names of its
#    contributors may be used to endorse or promote products derived from
#    this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
# SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
# CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
# OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

from enum import IntEnum


# https://registry.khronos.org/VulkanSC/specs/1.0-extensions/man/html/VkMemoryPropertyFlagBits.html
class MemoryProperty(IntEnum):
    DEVICE_LOCAL_BIT = 0x00000001
    HOST_VISIBLE_BIT = 0x00000002
    HOST_COHERENT_BIT = 0x00000004
    HOST_CACHED_BIT = 0x00000008
    LAZILY_ALLOCATED_BIT = 0x00000010
    PROTECTED_BIT = 0x00000020


def convert_memory_property_flags_to_string(flags):
    flags_list = []
    if (flags & MemoryProperty.DEVICE_LOCAL_BIT) != 0:
        flags_list.append('device local')
    if (flags & MemoryProperty.HOST_VISIBLE_BIT) != 0:
        flags_list.append('host visible')
    if (flags & MemoryProperty.HOST_COHERENT_BIT) != 0:
        flags_list.append('host coherent')
    if (flags & MemoryProperty.HOST_CACHED_BIT) != 0:
        flags_list.append('host cached')
    if (flags & MemoryProperty.LAZILY_ALLOCATED_BIT) != 0:
        flags_list.append('lazily allocated')
    if (flags & MemoryProperty.PROTECTED_BIT) != 0:
        flags_list.append('protected')
    if len(flags_list) == 0:
        return 'none'
    return ', '.join(flags_list)


def to_mem_string(num_bytes):
    size_kib = 1024.0
    size_mib = size_kib * 1024.0
    size_gib = size_mib * 1024.0
    if num_bytes < size_kib:
        return f'{num_bytes}B'
    elif num_bytes < size_mib:
        return f'{num_bytes / size_kib}KiB'
    elif num_bytes < size_gib:
        return f'{num_bytes / size_mib}MiB'
    return f'{num_bytes / size_gib}GiB'
