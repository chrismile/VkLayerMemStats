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
from memory_properties import convert_memory_property_flags_to_string, to_mem_string
from utils import CopyStatistics


def main():
    parser = argparse.ArgumentParser(
        prog='stats.py', description='Prints statistics for a memstats.csv file.')
    parser.add_argument('filename')
    parser.add_argument('--frame-idx', type=int, required=True)
    parser.add_argument('--show-image-stats', action='store_true', default=False)
    args = parser.parse_args()

    file_format_version = 0
    frame_idx_curr = 0
    frame_start_timestamp = -1
    frame_stop_timestamp = -1
    is_frame_current = False

    cpu_num_allocations = 0
    cpu_allocated_memory = 0

    # GPU data (memory allocations)
    num_gpu_mem_types = 0
    curr_mem_gpu_types_flags = []
    gpu_types_num_allocations = []
    gpu_types_allocated_memory = []

    # GPU data (memory copies)
    copy_statistics = CopyStatistics()

    with open(args.filename, 'r') as f:
        for line in f:
            entries = line.strip().split(',')
            if len(entries) < 2:
                continue
            timestamp = float(entries[1]) * 1e-9
            if entries[0] == 'version':
                file_format_version = int(entries[2])
            elif entries[0] == 'devinfo':
                device_type = int(entries[2])
                device_name = entries[3]
            elif entries[0] == 'memtype':
                num_gpu_mem_types += 1
                gpu_types_num_allocations.append(0)
                gpu_types_allocated_memory.append(0)
                curr_mem_gpu_types_flags.append(entries[4])
            elif entries[0] == 'alloc':
                if entries[2] == '0' and is_frame_current:
                    cpu_num_allocations += 1
                    cpu_allocated_memory += int(entries[3])
                if entries[2] == '1':
                    mem_type_idx = int(entries[5])
                    if is_frame_current:
                        gpu_types_num_allocations[mem_type_idx] += 1
                        gpu_types_allocated_memory[mem_type_idx] += int(entries[3])
                    copy_statistics.gpu_mem_ptr_to_type_map[entries[4]] = mem_type_idx
            elif entries[0] == 'free':
                if entries[2] == '1' and entries[3] in copy_statistics.gpu_mem_ptr_to_type_map:
                    del copy_statistics.gpu_mem_ptr_to_type_map[entries[3]]
            elif entries[0] == 'acquire_next_image':
                if frame_idx_curr == args.frame_idx:
                    is_frame_current = True
                    frame_start_timestamp = timestamp
                if frame_idx_curr == args.frame_idx + 1:
                    is_frame_current = False
                    frame_stop_timestamp = timestamp
                frame_idx_curr += 1
            # Copies
            elif entries[0] == 'bind_buffer_memory':
                copy_statistics.bind_buffer_memory(entries[2], entries[3])
            elif entries[0] == 'bind_image_memory':
                copy_statistics.bind_image_memory(entries[2], entries[3])
            elif entries[0] == 'destroy_buffer':
                copy_statistics.destroy_buffer(entries[2])
            elif entries[0] == 'destroy_image':
                copy_statistics.destroy_image(entries[2])
            elif entries[0] == 'update_buffer' and is_frame_current:
                copy_size = float(entries[3])
                buffer_dst_ptr = entries[4]
                copy_statistics.add_update_buffer(copy_size, buffer_dst_ptr)
            elif entries[0] == 'copy_buffer' and is_frame_current:
                if file_format_version == 0:
                    copy_size = float(entries[2])
                    buffer_src_ptr = entries[3]
                    buffer_dst_ptr = entries[4]
                else:
                    copy_size = float(entries[3])
                    buffer_src_ptr = entries[4]
                    buffer_dst_ptr = entries[5]
                copy_statistics.add_copy_buffer(copy_size, buffer_src_ptr, buffer_dst_ptr)
            elif entries[0] == 'copy_image' and is_frame_current:
                if file_format_version == 0:
                    copy_size = float(entries[2])
                    image_src_ptr = entries[3]
                    image_dst_ptr = entries[4]
                else:
                    copy_size = float(entries[3])
                    image_src_ptr = entries[4]
                    image_dst_ptr = entries[5]
                copy_statistics.add_copy_image(copy_size, image_src_ptr, image_dst_ptr)
            elif entries[0] == 'copy_buffer_to_image' and is_frame_current:
                if file_format_version == 0:
                    copy_size = float(entries[2])
                    buffer_src_ptr = entries[3]
                    image_dst_ptr = entries[4]
                else:
                    copy_size = float(entries[3])
                    buffer_src_ptr = entries[4]
                    image_dst_ptr = entries[5]
                copy_statistics.add_copy_buffer_to_image(copy_size, buffer_src_ptr, image_dst_ptr)
            elif entries[0] == 'copy_image_to_buffer' and is_frame_current:
                if file_format_version == 0:
                    copy_size = float(entries[2])
                    image_src_ptr = entries[3]
                    buffer_dst_ptr = entries[4]
                else:
                    copy_size = float(entries[3])
                    image_src_ptr = entries[4]
                    buffer_dst_ptr = entries[5]
                copy_statistics.add_copy_image_to_buffer(copy_size, image_src_ptr, buffer_dst_ptr)

    print(f'Frame duration: {(frame_stop_timestamp - frame_start_timestamp) * 1e3}ms')
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

    copy_statistics.print_statistics(args.show_image_stats)


if __name__ == '__main__':
    main()
