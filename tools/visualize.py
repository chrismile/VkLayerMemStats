import argparse
import matplotlib
import matplotlib.pyplot as plt


def main():
    #matplotlib.rcParams['pdf.fonttype'] = 42
    #matplotlib.rcParams['ps.fonttype'] = 42
    #matplotlib.rcParams.update({'font.family': 'Linux Biolinum O'})
    #matplotlib.rcParams.update({'font.size': 17.5})
    parser = argparse.ArgumentParser(
        prog='visualize.py', description='Visualizes a memstats.csv file.')
    parser.add_argument('filename')
    parser.add_argument('--show-submits', action='store_true', default=False)
    parser.add_argument('--show-acquire', action='store_true', default=False)
    args = parser.parse_args()

    DEVTYPE_INTEGRATED = 1
    DEVTYPE_DISCRETE = 2
    device_type = DEVTYPE_DISCRETE  # VkPhysicalDeviceType
    device_name = 'Unknown'
    curr_mem_cpu = 0.0
    curr_mem_gpu_types = []
    curr_mem_gpu_types_flags = []
    time_points_cpu = []
    mem_points_cpu = []
    gpu_ptr_to_heap_map = {}
    num_gpu_mem_types = 0
    time_points_gpu_types = []
    mem_points_gpu_types = []
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
                if entries[2] == '1':
                    mem_type_idx = int(entries[5])
                    time_points_gpu_types[mem_type_idx].append(timestamp)
                    mem_points_gpu_types[mem_type_idx].append(curr_mem_gpu_types[mem_type_idx])
                    curr_mem_gpu_types[mem_type_idx] += int(entries[3])
                    time_points_gpu_types[mem_type_idx].append(timestamp)
                    mem_points_gpu_types[mem_type_idx].append(curr_mem_gpu_types[mem_type_idx])
                    gpu_ptr_to_heap_map[entries[4]] = mem_type_idx
            elif entries[0] == 'free':
                if entries[2] == '0':
                    time_points_cpu.append(timestamp)
                    mem_points_cpu.append(curr_mem_cpu)
                    curr_mem_cpu -= int(entries[3])
                    time_points_cpu.append(timestamp)
                    mem_points_cpu.append(curr_mem_cpu)
                if entries[2] == '1':
                    mem_type_idx = gpu_ptr_to_heap_map[entries[4]]
                    del gpu_ptr_to_heap_map[entries[4]]
                    time_points_gpu_types[mem_type_idx].append(timestamp)
                    mem_points_gpu_types[mem_type_idx].append(curr_mem_gpu_types[mem_type_idx])
                    curr_mem_gpu_types[mem_type_idx] -= int(entries[3])
                    time_points_gpu_types[mem_type_idx].append(timestamp)
                    mem_points_gpu_types[mem_type_idx].append(curr_mem_gpu_types[mem_type_idx])

    mem_type_names = []
    for mem_type_idx in range(num_gpu_mem_types):
        flags = curr_mem_gpu_types_flags[mem_type_idx]
        mem_type_names = str(flags)  # TODO

    plt.title(f'Memory plot ({device_name})')
    plt.xlabel('Time (s)')
    plt.ylabel('Memory (MiB)')
    plt.figure(1)
    plt.plot(time_points_cpu, mem_points_cpu, label='CPU')
    for mem_type_idx in range(num_gpu_mem_types):
        if len(time_points_gpu_types[mem_type_idx]) > 0:
            plt.plot(time_points_gpu_types[mem_type_idx], mem_points_gpu_types[mem_type_idx], label=f'GPU ({mem_type_names[mem_type_idx]})')
    plt.legend()
    plt.tight_layout()
    #plt.savefig(os.path.join(dataset_dir, f'{test_name}.svg'), bbox_inches='tight', pad_inches=0.01)
    plt.show()


if __name__ == '__main__':
    main()
