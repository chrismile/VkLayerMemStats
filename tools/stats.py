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

import argparse
from memory_properties import MemoryProperty, convert_memory_property_flags_to_string, to_mem_string


def main():
    parser = argparse.ArgumentParser(
        prog='stats.py', description='Prints statistics for a memstats.csv file.')
    parser.add_argument('filename')
    args = parser.parse_args()

    num_frames = 0
    num_submits = 0

    cpu_num_allocations = 0
    cpu_allocated_memory = 0

    # GPU data (memory allocations)
    num_gpu_mem_types = 0
    curr_mem_gpu_types_flags = []
    gpu_mem_ptr_to_type_map = {}
    gpu_types_num_allocations = []
    gpu_types_allocated_memory = []

    # GPU data (buffers & images)
    gpu_buffer_ptr_to_alloc_ptr_map = {}
    gpu_image_ptr_to_alloc_ptr_map = {}

    # GPU data (memory copies)
    gpu_device_to_device_num_copies = 0
    gpu_device_to_device_copy_size = 0
    gpu_host_to_device_num_copies = 0
    gpu_host_to_device_copy_size = 0
    gpu_device_to_host_num_copies = 0
    gpu_device_to_host_copy_size = 0

    def add_memcpy(copy_size, mem_type_src, mem_type_dst):
        nonlocal gpu_device_to_device_num_copies, gpu_device_to_device_copy_size
        nonlocal gpu_host_to_device_num_copies, gpu_host_to_device_copy_size
        nonlocal gpu_device_to_host_num_copies, gpu_device_to_host_copy_size
        is_src_host_visible = (mem_type_src & MemoryProperty.HOST_VISIBLE_BIT) != 0
        is_dst_host_visible = (mem_type_dst & MemoryProperty.HOST_VISIBLE_BIT) != 0
        if not is_src_host_visible and not is_dst_host_visible:
            gpu_device_to_device_num_copies += 1
            gpu_device_to_device_copy_size += copy_size
        elif is_src_host_visible and not is_dst_host_visible:
            gpu_host_to_device_num_copies += 1
            gpu_host_to_device_copy_size += copy_size
        elif not is_src_host_visible and is_dst_host_visible:
            gpu_device_to_host_num_copies += 1
            gpu_device_to_host_copy_size += copy_size

    max_timestamp = 0.0
    with open(args.filename, 'r') as f:
        for line in f:
            entries = line.strip().split(',')
            if len(entries) < 2:
                continue
            max_timestamp = float(entries[1]) * 1e-9
            if entries[0] == 'memtype':
                num_gpu_mem_types += 1
                gpu_types_num_allocations.append(0)
                gpu_types_allocated_memory.append(0)
                curr_mem_gpu_types_flags.append(entries[4])
            elif entries[0] == 'alloc':
                if entries[2] == '0':
                    cpu_num_allocations += 1
                    cpu_allocated_memory += int(entries[3])
                if entries[2] == '1':
                    mem_type_idx = int(entries[5])
                    gpu_types_num_allocations[mem_type_idx] += 1
                    gpu_types_num_allocations[mem_type_idx] += int(entries[3])
                    gpu_mem_ptr_to_type_map[entries[4]] = mem_type_idx
            elif entries[0] == 'free':
                if entries[2] == '1' and entries[3] in gpu_mem_ptr_to_type_map:
                    del gpu_mem_ptr_to_type_map[entries[3]]
            elif entries[0] == 'submit':
                num_submits += 1
            elif entries[0] == 'acquire_next_image':
                num_frames += 1
            # Copies
            elif entries[0] == 'bind_buffer_memory':
                gpu_buffer_ptr_to_alloc_ptr_map[entries[2]] = entries[3]
            elif entries[0] == 'bind_image_memory':
                gpu_image_ptr_to_alloc_ptr_map[entries[2]] = entries[3]
            elif entries[0] == 'destroy_buffer':
                del gpu_buffer_ptr_to_alloc_ptr_map[entries[2]]
            elif entries[0] == 'destroy_image':
                del gpu_image_ptr_to_alloc_ptr_map[entries[2]]
            elif entries[0] == 'copy_buffer':
                copy_size = float(entries[2])
                buffer_src_ptr = entries[3]
                buffer_dst_ptr = entries[4]
                mem_type_src = gpu_mem_ptr_to_type_map[gpu_buffer_ptr_to_alloc_ptr_map[buffer_src_ptr]]
                mem_type_dst = gpu_mem_ptr_to_type_map[gpu_buffer_ptr_to_alloc_ptr_map[buffer_dst_ptr]]
                add_memcpy(copy_size, mem_type_src, mem_type_dst)
            elif entries[0] == 'copy_buffer_to_image':
                copy_size = float(entries[2])
                buffer_src_ptr = entries[3]
                image_dst_ptr = entries[4]
                mem_type_src = gpu_mem_ptr_to_type_map[gpu_buffer_ptr_to_alloc_ptr_map[buffer_src_ptr]]
                mem_type_dst = gpu_mem_ptr_to_type_map[gpu_image_ptr_to_alloc_ptr_map[image_dst_ptr]]
                add_memcpy(copy_size, mem_type_src, mem_type_dst)
            elif entries[0] == 'copy_image_to_buffer':
                copy_size = float(entries[2])
                image_src_ptr = entries[3]
                buffer_dst_ptr = entries[4]
                mem_type_src = gpu_mem_ptr_to_type_map[gpu_image_ptr_to_alloc_ptr_map[image_src_ptr]]
                mem_type_dst = gpu_mem_ptr_to_type_map[gpu_buffer_ptr_to_alloc_ptr_map[buffer_dst_ptr]]
                add_memcpy(copy_size, mem_type_src, mem_type_dst)

    print(f'Total duration: {max_timestamp}s')
    print(f'Number of frames: {num_frames}')
    print(f'Number of submits: {num_submits}')
    print()
    print(f'#CPU allocations: {cpu_num_allocations}')
    print(f'Total CPU allocated memory: {to_mem_string(cpu_allocated_memory)}')
    print()
    for mem_type_idx in range(num_gpu_mem_types):
        flags = int(curr_mem_gpu_types_flags[mem_type_idx])
        mem_type_name = convert_memory_property_flags_to_string(flags)
        print(f'Vulkan memory type #{mem_type_idx} ({mem_type_name}):')
        print(f'- #Allocations: {gpu_types_num_allocations[mem_type_idx]}')
        print(f'- Total allocated memory: {to_mem_string(gpu_types_allocated_memory[mem_type_idx])}')
        print()

    print(f'#Device-to-device copies: {gpu_device_to_device_num_copies}')
    print(f'Device-to-device copy size: {to_mem_string(gpu_device_to_device_copy_size)}')
    print(f'#Host-to-device copies: {gpu_host_to_device_num_copies}')
    print(f'Host-to-device copy size: {to_mem_string(gpu_host_to_device_copy_size)}')
    print(f'#Device-to-host copies: {gpu_device_to_host_num_copies}')
    print(f'Device-to-host copy size: {to_mem_string(gpu_device_to_host_copy_size)}')


if __name__ == '__main__':
    main()
