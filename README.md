# VkLayer_memstats

A Vulkan layer for dumping memory statistics.

When using the layer, memory statistics will be dumped to a file memstats.csv. The layer can either only dump Vulkan
memory allocations, or also CPU memory allocations (see "CPU memory allocation hooking").


## How to build

```shell
git submodule update --init --recursive

# Linux
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j $(nproc)

# Windows
cmake -B build -S .
cmake --build build --config Release -j 8
```


## Usage on Linux

Vulkan changed how layer configuration works for loaders built against the 1.3.234 Vulkan headers or later.
For more details, please refer to
https://github.com/KhronosGroup/Vulkan-Utility-Libraries/blob/main/docs/layer_configuration.md.
If using a recent Vulkan loader, the environment variables below can be used to enable the layer.

```shell
export VK_ADD_LAYER_PATH=$(pwd)/build
export VK_LOADER_LAYERS_ENABLE=*memstats
```

For older Vulkan loaders, please use the deprecated `VK_INSTANCE_LAYERS` variable.

```shell
export VK_ADD_LAYER_PATH=$(pwd)/build
export VK_INSTANCE_LAYERS=VK_LAYER_CHRISMILE_memstats
```

If the layer should be installed globally, please add the following arguments to `cmake -B build -S .`.
- Linux (user global): `-DCMAKE_INSTALL_PREFIX="$HOME/.local/share/vulkan/implicit_layer.d"`

Independent of the operating system, `--target install` needs to be added to `cmake --build build` in this case as well.


## Usage on Windows

The usage on Windows is the same as on Linux, but depending on the used terminal (cmd.exe/PowerShell/MSYS2).
Examples are given below.

### MSYS2 (bash)

```shell
export VK_ADD_LAYER_PATH=<...>
export VK_LOADER_LAYERS_ENABLE=*memstats
```

### cmd.exe

```shell
set VK_ADD_LAYER_PATH=<...>
set VK_LOADER_LAYERS_ENABLE=*memstats
```

### PowerShell

```shell
$env:VK_ADD_LAYER_PATH = "<...>"
$env:VK_LOADER_LAYERS_ENABLE = "*memstats"
```


## CPU memory allocation hooking

By default, the Vulkan layer will only dump statistics for Vulkan memory allocations.
When configuring with the option `-DHOOK_MALLOC=ON`, CPU memory allocation functions will be hooked as well.
Additionally, on Linux `LD_PRELOAD` needs to point to the Vulkan layer shared library, e.g.:
```shell
export LD_PRELOAD=<PATH>/libVkLayer_memstats.so
export VK_ADD_LAYER_PATH=<PATH>
export VK_LOADER_LAYERS_ENABLE=*memstats
```

For more details on the malloc hooking mechanism on Linux, please refer to
https://sourceware.org/glibc/manual/2.43/html_mono/libc.html#Replacing-malloc.

On Windows, [Detours](https://github.com/microsoft/Detours) or [MinHook](https://github.com/TsudaKageyu/minhook)
can be used for similarly hooking CPU memory allocations.

Please note that during testing this on Linux, weird things happened when using malloc hooking together with Vulkan
apps using DLSS. Calling `NVSDK_NGX_VULKAN_GetFeatureRequirements` caused the shared library constructor and destructor
to be triggered two additional times.

Furthermore, please note that compiling with `-DHOOK_MALLOC=ON` and not using `LD_PRELOAD` will lead to crashes when
the Vulkan layer calls fprintf to write to the output file.


## File output format

When using the layer, memory statistics will be dumped to a file memstats.csv.
One line of the file contains one data record. Individual entries are separated by commas.

The first entry denotes the type of the record. The second entry is always a timestamp in nanoseconds.
- An entry starting with `alloc` denotes a memory allocation.
  Additional entries: Type (CPU = 0, GPU = 1), size in bytes, pointer
  Optional entries: Memory type index (only for GPU)
- An entry starting with `free` denotes a memory deallocation.
  Additional entries: Type (CPU = 0, GPU = 1), pointer
- An entry starting with `devinfo` is written when a new Vulkan device is created.
  Additional entries: Device type (VkPhysicalDeviceType), device name
- An entry starting with `memheap` is written when a new Vulkan device is created.
  Additional entries: Heap index, size in bytes, flags
- An entry starting with `memtype` is written when a new Vulkan device is created.
  Additional entries: Type index, heap index, property flags
- An entry starting with `submit` is written when `vkQueueSubmit` is called. 
- An entry starting with `acquire_next_image` is written when `vkAcquireNextImageKHR` is called. 


# Future considerations

- We could use similar code like https://stackoverflow.com/questions/69838353/heapalloc-hooking-with-minihook-deadlock-on-windows-10-works-on-windows-7
  to also log the name of the DLL that makes a CPU memory allocation (e.g., to separate application and driver).
- Supply Windows executable for achieving similar effect as `LD_PRELOAD` on Linux.
  See: https://github.com/microsoft/detours/wiki/SampleWithdll, https://github.com/microsoft/detours/wiki/DetourCreateProcessWithDlls