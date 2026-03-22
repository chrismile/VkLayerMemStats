cmake_minimum_required(VERSION 3.18...4.0)
project(VkLayerMemStats)

set(CMAKE_CXX_STANDARD 17)

if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -W3 /EHsc /Zc:__cplusplus")
elseif(CMAKE_COMPILER_IS_GNUCXX)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")
endif()

option(HOOK_MALLOC "Hook CPU memory allocation/deallocation." OFF)
option(USE_STATIC_STD_LIBRARIES "Link with standard libraries statically." OFF)
if (WIN32)
    option(USE_DETOURS "Use Detours for CPU memory allocation/deallocation hooking (Windows only)." OFF)
    option(USE_MINHOOK "Use MinHook for CPU memory allocation/deallocation hooking (Windows only)." OFF)
endif()
set(DATA_PATH "${CMAKE_SOURCE_DIR}/Data" CACHE PATH "Location of folder 'Data'")

if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Vulkan-Headers/include")
    message(FATAL_ERROR "Error: Submodules are not cloned. Please call \"git submodule update --init --recursive\".")
endif()

list(APPEND SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/VkLayerMemStats.cpp")
list(APPEND SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Vulkan-Utility-Libraries/src/layer/vk_layer_settings.cpp")
list(APPEND SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Vulkan-Utility-Libraries/src/layer/vk_layer_settings_helper.cpp")
list(APPEND SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Vulkan-Utility-Libraries/src/layer/layer_settings_util.cpp")
list(APPEND SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Vulkan-Utility-Libraries/src/layer/layer_settings_manager.cpp")

add_library(VkLayer_memstats SHARED ${SOURCES})
target_include_directories(VkLayer_memstats PRIVATE third_party/Vulkan-Headers/include)
target_include_directories(VkLayer_memstats PRIVATE third_party/Vulkan-Utility-Libraries/include)
target_include_directories(VkLayer_memstats PRIVATE third_party)

if (${USE_STATIC_STD_LIBRARIES})
    if (MSYS OR MINGW OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_link_options(VkLayer_memstats PRIVATE -static-libgcc -static-libstdc++)
    endif()
    if (MSYS OR MINGW OR (WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"))
        target_link_options(VkLayer_memstats PRIVATE -static -lpthread)
    endif()
endif()

if (WIN32)
    if (${USE_DETOURS} OR ${USE_MINHOOK})
        set(HOOK_MALLOC ON)
    endif()
endif()
if (${HOOK_MALLOC})
    target_compile_definitions(VkLayer_memstats PRIVATE HOOK_MALLOC)
endif()

# Unit tests.
if (WIN32 AND ${HOOK_MALLOC})
    include(FetchContent)
    if (${USE_DETOURS})
        FetchContent_Declare(
                detours
                GIT_REPOSITORY https://github.com/microsoft/Detours.git
                GIT_TAG        9764cebcb1a75940e68fa83d6730ffaf0f669401
        )
        FetchContent_MakeAvailable(detours)
        list(APPEND DETOURS_SOURCES "${detours_SOURCE_DIR}/src/detours.cpp")
        list(APPEND DETOURS_SOURCES "${detours_SOURCE_DIR}/src/creatwth.cpp")
        list(APPEND DETOURS_SOURCES "${detours_SOURCE_DIR}/src/image.cpp")
        list(APPEND DETOURS_SOURCES "${detours_SOURCE_DIR}/src/modules.cpp")
        list(APPEND DETOURS_SOURCES "${detours_SOURCE_DIR}/src/disasm.cpp")
        if (CMAKE_SYSTEM_PROCESSOR STREQUAL "AMD64")
            list(APPEND DETOURS_SOURCES "${detours_SOURCE_DIR}/src/disolx64.cpp")
        elseif (CMAKE_SYSTEM_PROCESSOR STREQUAL "X86")
            list(APPEND DETOURS_SOURCES "${detours_SOURCE_DIR}/src/disolx86.cpp")
        elseif (CMAKE_SYSTEM_PROCESSOR STREQUAL "ARM64")
            list(APPEND DETOURS_SOURCES "${detours_SOURCE_DIR}/src/disolarm64.cpp")
        else()
            message(FATAL_ERROR "Unsupported architecture.")
        endif()
        add_library(detours STATIC ${DETOURS_SOURCES})
        target_include_directories(detours PUBLIC "${detours_SOURCE_DIR}/src")
        target_compile_definitions(VkLayer_memstats PRIVATE USE_DETOURS)
        target_link_libraries(VkLayer_memstats PRIVATE detours)

        add_executable(withdll "${detours_SOURCE_DIR}/samples/withdll/withdll.cpp")
        target_link_libraries(withdll PRIVATE detours)

        add_executable(preload "${CMAKE_CURRENT_SOURCE_DIR}/src/DetoursPreload.cpp")
        target_link_libraries(preload PRIVATE detours)
        target_link_libraries(preload PRIVATE shlwapi)
    elseif (${USE_MINHOOK})
        FetchContent_Declare(
                minhook
                GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
                GIT_TAG        1e9ad1eb42db11bfcb65461f687c656612d1b555
                SOURCE_SUBDIR  nosubdir
        )
        FetchContent_MakeAvailable(minhook)
        list(APPEND MINHOOK_SOURCES "${minhook_SOURCE_DIR}/src/buffer.c")
        list(APPEND MINHOOK_SOURCES "${minhook_SOURCE_DIR}/src/hook.c")
        list(APPEND MINHOOK_SOURCES "${minhook_SOURCE_DIR}/src/trampoline.c")
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            list(APPEND MINHOOK_SOURCES "${minhook_SOURCE_DIR}/src/hde/hde64.c")
        else()
            list(APPEND MINHOOK_SOURCES "${minhook_SOURCE_DIR}/src/hde/hde32.c")
        endif()
        add_library(minhook STATIC ${MINHOOK_SOURCES})
        target_include_directories(minhook PUBLIC ${minhook_SOURCE_DIR}/include)
        target_include_directories(minhook PRIVATE ${minhook_SOURCE_DIR}/src)
        target_include_directories(minhook PRIVATE ${minhook_SOURCE_DIR}/src/hde)
        target_compile_definitions(VkLayer_memstats PRIVATE USE_MINHOOK)
        target_link_libraries(VkLayer_memstats PRIVATE minhook)
    else()
        message(FATAL_ERROR "HOOK_MALLOC enabled, but no library selected.")
    endif()
endif()

if (UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(VkLayer_memstats PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/CMake/symbols_linux.map")
endif()

if (NOT WIN32)
    set(VK_LAYER_LIBRARY_PATH "./libVkLayer_memstats.so")
elseif(MSYS OR MINGW OR (${CMAKE_GENERATOR} STREQUAL "MinGW Makefiles") OR (${CMAKE_GENERATOR} STREQUAL "MSYS Makefiles"))
    set(VK_LAYER_LIBRARY_PATH ".\\\\libVkLayer_memstats.dll")
else()
    set(VK_LAYER_LIBRARY_PATH ".\\\\VkLayer_memstats.dll")
endif()
configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/CMake/VkLayer_memstats.json.in"
        "${CMAKE_CURRENT_BINARY_DIR}/VkLayer_memstats.json"
)

install(
        FILES "${CMAKE_CURRENT_BINARY_DIR}/VkLayer_memstats.json"
        DESTINATION ${CMAKE_INSTALL_PREFIX}
)
install(
        TARGETS VkLayer_memstats
        LIBRARY DESTINATION .
)
