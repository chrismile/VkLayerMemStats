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
import numpy as np
import matplotlib
import matplotlib.pyplot as plt
from memory_properties import MemoryProperty, convert_memory_property_flags_to_string


def main():
    #matplotlib.rcParams['pdf.fonttype'] = 42
    #matplotlib.rcParams['ps.fonttype'] = 42
    #matplotlib.rcParams.update({'font.family': 'Linux Biolinum O'})
    #matplotlib.rcParams.update({'font.size': 17.5})
    parser = argparse.ArgumentParser(
        prog='visualize.py', description='Visualizes a memstats.csv file.')
    parser.add_argument('filename')
    parser.add_argument('--hide-gpu-allocations', action='store_true', default=False)
    parser.add_argument('--hide-cpu-allocations', action='store_true', default=False)
    parser.add_argument('--show-submits', action='store_true', default=False)
    parser.add_argument('--show-acquire', action='store_true', default=False)
    parser.add_argument('--show-memcpy-device-to-device', action='store_true', default=False)
    parser.add_argument('--show-memcpy-host-to-device', action='store_true', default=False)
    parser.add_argument('--show-memcpy-device-to-host', action='store_true', default=False)
    parser.add_argument('--frame-idx', type=int, default=-1)
    parser.add_argument('--out', type=str, default=None)
    args = parser.parse_args()

    # For frame subselection
    frame_idx_curr = 0
    frame_start_timestamp = -1
    frame_stop_timestamp = -1

    # CPU data
    curr_mem_cpu = 0.0
    cpu_buffer_to_size_map = {}
    time_points_cpu = []
    mem_points_cpu = []

    # GPU data (general)
    submit_timestamps = []
    image_acquire_timestamps = []
    DEVTYPE_INTEGRATED = 1
    DEVTYPE_DISCRETE = 2
    device_type = DEVTYPE_DISCRETE  # VkPhysicalDeviceType
    device_name = 'Unknown'
    num_gpu_mem_types = 0

    # GPU data (memory allocations)
    curr_mem_gpu_types = []
    curr_mem_gpu_types_flags = []
    gpu_mem_ptr_to_type_map = {}
    gpu_mem_to_size_map = {}
    time_points_gpu_types = []
    mem_points_gpu_types = []

    # GPU data (buffers & images)
    gpu_buffer_ptr_to_alloc_ptr_map = {}
    gpu_image_ptr_to_alloc_ptr_map = {}

    # GPU data (memory copies)
    gpu_device_to_device_copy_timestamps = []
    gpu_device_to_device_copy_sizes = []
    gpu_host_to_device_copy_timestamps = []
    gpu_host_to_device_copy_sizes = []
    gpu_device_to_host_copy_timestamps = []
    gpu_device_to_host_copy_sizes = []
    def add_memcpy(copy_size, mem_type_src, mem_type_dst):
        is_src_host_visible = (mem_type_src & MemoryProperty.HOST_VISIBLE_BIT) != 0
        is_dst_host_visible = (mem_type_dst & MemoryProperty.HOST_VISIBLE_BIT) != 0
        if not is_src_host_visible and not is_dst_host_visible:
            gpu_device_to_device_copy_timestamps.append(timestamp)
            gpu_device_to_device_copy_sizes.append(copy_size)
        elif is_src_host_visible and not is_dst_host_visible:
            gpu_host_to_device_copy_timestamps.append(timestamp)
            gpu_host_to_device_copy_sizes.append(copy_size)
        elif not is_src_host_visible and is_dst_host_visible:
            gpu_device_to_host_copy_timestamps.append(timestamp)
            gpu_device_to_host_copy_sizes.append(copy_size)

    with open(args.filename, 'r') as f:
        for line in f:
            entries = line.strip().split(',')
            if len(entries) < 2:
                continue
            timestamp = float(entries[1]) * 1e-9
            if entries[0] == 'devinfo':
                device_type = int(entries[2])
                device_name = entries[3]
            elif entries[0] == 'memtype':
                num_gpu_mem_types += 1
                time_points_gpu_types.append([])
                mem_points_gpu_types.append([])
                curr_mem_gpu_types.append(0.0)
                curr_mem_gpu_types_flags.append(entries[4])
            elif entries[0] == 'alloc':
                if entries[2] == '0':
                    time_points_cpu.append(timestamp)
                    mem_points_cpu.append(curr_mem_cpu)
                    curr_mem_cpu += int(entries[3])
                    time_points_cpu.append(timestamp)
                    mem_points_cpu.append(curr_mem_cpu)
                    cpu_buffer_to_size_map[entries[4]] = int(entries[3])
                if entries[2] == '1':
                    mem_type_idx = int(entries[5])
                    time_points_gpu_types[mem_type_idx].append(timestamp)
                    mem_points_gpu_types[mem_type_idx].append(curr_mem_gpu_types[mem_type_idx])
                    curr_mem_gpu_types[mem_type_idx] += int(entries[3])
                    time_points_gpu_types[mem_type_idx].append(timestamp)
                    mem_points_gpu_types[mem_type_idx].append(curr_mem_gpu_types[mem_type_idx])
                    gpu_mem_ptr_to_type_map[entries[4]] = mem_type_idx
                    gpu_mem_to_size_map[entries[4]] = int(entries[3])
            elif entries[0] == 'free':
                if entries[2] == '0' and entries[3] in cpu_buffer_to_size_map:
                    time_points_cpu.append(timestamp)
                    mem_points_cpu.append(curr_mem_cpu)
                    curr_mem_cpu -= cpu_buffer_to_size_map[entries[3]]
                    time_points_cpu.append(timestamp)
                    mem_points_cpu.append(curr_mem_cpu)
                    del cpu_buffer_to_size_map[entries[3]]
                if entries[2] == '1' and entries[3] in gpu_mem_to_size_map:
                    mem_type_idx = gpu_mem_ptr_to_type_map[entries[3]]
                    time_points_gpu_types[mem_type_idx].append(timestamp)
                    mem_points_gpu_types[mem_type_idx].append(curr_mem_gpu_types[mem_type_idx])
                    curr_mem_gpu_types[mem_type_idx] -= gpu_mem_to_size_map[entries[3]]
                    time_points_gpu_types[mem_type_idx].append(timestamp)
                    mem_points_gpu_types[mem_type_idx].append(curr_mem_gpu_types[mem_type_idx])
                    del gpu_mem_ptr_to_type_map[entries[3]]
                    del gpu_mem_to_size_map[entries[3]]
            elif entries[0] == 'submit':
                submit_timestamps.append(timestamp)
            elif entries[0] == 'acquire_next_image':
                if frame_idx_curr == args.frame_idx:
                    frame_start_timestamp = timestamp
                if frame_idx_curr == args.frame_idx + 1:
                    frame_stop_timestamp = timestamp - 1e-9
                image_acquire_timestamps.append(timestamp)
                frame_idx_curr += 1
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

    mem_type_names = []
    for mem_type_idx in range(num_gpu_mem_types):
        flags = int(curr_mem_gpu_types_flags[mem_type_idx])
        mem_type_names.append(convert_memory_property_flags_to_string(flags))

    mem_points_cpu = np.asarray(mem_points_cpu)
    max_mem = np.max(mem_points_cpu)
    for mem_type_idx in range(num_gpu_mem_types):
        mem_points_gpu_types[mem_type_idx] = np.asarray(mem_points_gpu_types[mem_type_idx])
        if mem_points_gpu_types[mem_type_idx].shape[0] != 0:
            max_mem = max(max_mem, np.max(mem_points_gpu_types[mem_type_idx]))
    if max_mem > 1e9:
        units_mem = 'GiB'
        scale_mem = 1e9
    else:
        units_mem = 'MiB'
        scale_mem = 1e6
    mem_points_cpu /= scale_mem
    for mem_type_idx in range(num_gpu_mem_types):
        mem_points_gpu_types[mem_type_idx] /= scale_mem
    gpu_device_to_device_copy_sizes = np.asarray(gpu_device_to_device_copy_sizes)
    gpu_device_to_device_copy_sizes /= scale_mem
    gpu_host_to_device_copy_sizes = np.asarray(gpu_host_to_device_copy_sizes)
    gpu_host_to_device_copy_sizes /= scale_mem
    gpu_device_to_host_copy_sizes = np.asarray(gpu_device_to_host_copy_sizes)
    gpu_device_to_host_copy_sizes /= scale_mem

    plt.figure(1, (10, 6))
    plt.title(f'Memory plot ({device_name})')
    plt.xlabel('Time (s)')
    plt.ylabel(f'Memory ({units_mem})')
    if not args.hide_cpu_allocations:
        plt.plot(time_points_cpu, mem_points_cpu, label='CPU')
    if not args.hide_gpu_allocations:
        for mem_type_idx in range(num_gpu_mem_types):
            if len(time_points_gpu_types[mem_type_idx]) > 0:
                plt.plot(time_points_gpu_types[mem_type_idx], mem_points_gpu_types[mem_type_idx], label=f'GPU ({mem_type_names[mem_type_idx]})')
    if args.show_memcpy_device_to_device:
        plt.vlines(gpu_device_to_device_copy_timestamps, 0, gpu_device_to_device_copy_sizes, label='Device-to-device copy', colors='C0')
    if args.show_memcpy_host_to_device:
        plt.vlines(gpu_host_to_device_copy_timestamps, 0, gpu_host_to_device_copy_sizes, label='Host-to-device copy', colors='C1')
    if args.show_memcpy_device_to_host:
        plt.vlines(gpu_device_to_host_copy_timestamps, 0, gpu_device_to_host_copy_sizes, label='Device-to-host copy', colors='C2')
    if args.show_submits:
        plt.vlines(submit_timestamps, 0, max_mem / scale_mem, label='Submit', colors='C0')
    if args.show_acquire:
        plt.vlines(image_acquire_timestamps, 0, max_mem / scale_mem, label='Acquire image', colors='C1')
    if args.frame_idx >= 0:
        plt.xlim((frame_start_timestamp, frame_stop_timestamp))
    plt.legend()
    plt.tight_layout()
    if args.out is not None:
        plt.savefig(args.out, bbox_inches='tight', pad_inches=0.01)
    plt.show()


if __name__ == '__main__':
    main()
