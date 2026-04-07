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

from memory_properties import MemoryProperty, to_mem_string


class CopySubStats:
    def __init__(self):
        self.num_copies = 0
        self.copy_size = 0
        self.num_copies_image = 0
        self.copy_size_image = 0

    def add_copy(self, copy_size, src_is_buffer, dst_is_buffer):
        self.num_copies += 1
        self.copy_size += copy_size
        if not src_is_buffer or not dst_is_buffer:
            self.num_copies_image += 1
            self.copy_size_image += copy_size


class CopyStatistics:
    def __init__(self):
        self.gpu_mem_ptr_to_type_map = {}
        self.gpu_buffer_ptr_to_alloc_ptr_map = {}
        self.gpu_image_ptr_to_alloc_ptr_map = {}
        self.device_to_device_copies = CopySubStats()
        self.host_to_device_copies = CopySubStats()
        self.device_to_host_copies = CopySubStats()
        self.host_to_host_copies = CopySubStats()

    def bind_buffer_memory(self, buffer_ptr, memory_ptr):
        self.gpu_buffer_ptr_to_alloc_ptr_map[buffer_ptr] = memory_ptr

    def bind_image_memory(self, image_ptr, memory_ptr):
        self.gpu_image_ptr_to_alloc_ptr_map[image_ptr] = memory_ptr

    def destroy_buffer(self, buffer_ptr):
        del self.gpu_buffer_ptr_to_alloc_ptr_map[buffer_ptr]

    def destroy_image(self, image_ptr):
        del self.gpu_image_ptr_to_alloc_ptr_map[image_ptr]

    def add_update_buffer(self, copy_size, buffer_dst_ptr):
        mem_type_dst = self.gpu_mem_ptr_to_type_map[self.gpu_buffer_ptr_to_alloc_ptr_map[buffer_dst_ptr]]
        self.add_memcpy(copy_size, MemoryProperty.HOST_VISIBLE_BIT, mem_type_dst, True, True)

    def add_copy_buffer(self, copy_size, buffer_src_ptr, buffer_dst_ptr):
        mem_type_src = self.gpu_mem_ptr_to_type_map[self.gpu_buffer_ptr_to_alloc_ptr_map[buffer_src_ptr]]
        mem_type_dst = self.gpu_mem_ptr_to_type_map[self.gpu_buffer_ptr_to_alloc_ptr_map[buffer_dst_ptr]]
        self.add_memcpy(copy_size, mem_type_src, mem_type_dst, True, True)

    def add_copy_image(self, copy_size, image_src_ptr, image_dst_ptr):
        mem_type_src = self.gpu_mem_ptr_to_type_map[self.gpu_image_ptr_to_alloc_ptr_map[image_src_ptr]]
        mem_type_dst = self.gpu_mem_ptr_to_type_map[self.gpu_image_ptr_to_alloc_ptr_map[image_dst_ptr]]
        self.add_memcpy(copy_size, mem_type_src, mem_type_dst, True, False)

    def add_copy_buffer_to_image(self, copy_size, buffer_src_ptr, image_dst_ptr):
        mem_type_src = self.gpu_mem_ptr_to_type_map[self.gpu_buffer_ptr_to_alloc_ptr_map[buffer_src_ptr]]
        mem_type_dst = self.gpu_mem_ptr_to_type_map[self.gpu_image_ptr_to_alloc_ptr_map[image_dst_ptr]]
        self.add_memcpy(copy_size, mem_type_src, mem_type_dst, True, False)

    def add_copy_image_to_buffer(self, copy_size, image_src_ptr, buffer_dst_ptr):
        mem_type_src = self.gpu_mem_ptr_to_type_map[self.gpu_image_ptr_to_alloc_ptr_map[image_src_ptr]]
        mem_type_dst = self.gpu_mem_ptr_to_type_map[self.gpu_buffer_ptr_to_alloc_ptr_map[buffer_dst_ptr]]
        self.add_memcpy(copy_size, mem_type_src, mem_type_dst, False, True)

    def add_memcpy(self, copy_size, mem_type_src, mem_type_dst, src_is_buffer, dst_is_buffer):
        is_src_host_visible = (mem_type_src & MemoryProperty.HOST_VISIBLE_BIT) != 0
        is_dst_host_visible = (mem_type_dst & MemoryProperty.HOST_VISIBLE_BIT) != 0
        if not is_src_host_visible and not is_dst_host_visible:
            self.device_to_device_copies.add_copy(copy_size, src_is_buffer, dst_is_buffer)
        elif is_src_host_visible and not is_dst_host_visible:
            self.host_to_device_copies.add_copy(copy_size, src_is_buffer, dst_is_buffer)
        elif not is_src_host_visible and is_dst_host_visible:
            self.device_to_host_copies.add_copy(copy_size, src_is_buffer, dst_is_buffer)
        elif is_src_host_visible and is_dst_host_visible:
            self.host_to_host_copies.add_copy(copy_size, src_is_buffer, dst_is_buffer)

    def print_statistics(self, show_image_stats):
        if self.device_to_device_copies.num_copies > 0:
            print(f'#Device-to-device copies: {self.device_to_device_copies.num_copies}')
            print(f'Device-to-device copy size: {to_mem_string(self.device_to_device_copies.copy_size)}')
            if show_image_stats:
                print(f'- Image copies: {self.device_to_device_copies.num_copies_image}')
                print(f'- Image copy size: {to_mem_string(self.device_to_device_copies.copy_size_image)}')
        if self.host_to_device_copies.num_copies > 0:
            print(f'#Host-to-device copies: {self.host_to_device_copies.num_copies}')
            print(f'Host-to-device copy size: {to_mem_string(self.host_to_device_copies.copy_size)}')
            if show_image_stats:
                print(f'- Image copies: {self.host_to_device_copies.num_copies_image}')
                print(f'- Image copy size: {to_mem_string(self.host_to_device_copies.copy_size_image)}')
        if self.device_to_host_copies.num_copies > 0:
            print(f'#Device-to-host copies: {self.device_to_host_copies.num_copies}')
            print(f'Device-to-host copy size: {to_mem_string(self.device_to_host_copies.copy_size)}')
            if show_image_stats:
                print(f'- Image copies: {self.device_to_host_copies.num_copies_image}')
                print(f'- Image copy size: {to_mem_string(self.device_to_host_copies.copy_size_image)}')
        if self.host_to_host_copies.num_copies > 0:
            print(f'#Host-to-host copies: {self.host_to_host_copies.num_copies}')
            print(f'Host-to-host copy size: {to_mem_string(self.host_to_host_copies.copy_size)}')
            if show_image_stats:
                print(f'- Image copies: {self.host_to_host_copies.num_copies_image}')
                print(f'- Image copy size: {to_mem_string(self.host_to_host_copies.copy_size_image)}')
