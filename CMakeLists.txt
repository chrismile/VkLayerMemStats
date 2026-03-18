cmake_minimum_required(VERSION 3.12...4.0)
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
set(DATA_PATH "${CMAKE_SOURCE_DIR}/Data" CACHE PATH "Location of folder 'Data'")

if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Vulkan-Headers/include")
    message(FATAL_ERROR "Error: Submodules are not cloned. Please call \"git submodule update --init --recursive\".")
endif()

file(GLOB_RECURSE SOURCES src/*.cpp src/*.c src/*.hpp src/*.h)
list(APPEND SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/hashtable/hashtable.c")
list(APPEND SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Vulkan-Utility-Libraries/src/layer/vk_layer_settings.cpp")
list(APPEND SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Vulkan-Utility-Libraries/src/layer/vk_layer_settings_helper.cpp")
list(APPEND SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Vulkan-Utility-Libraries/src/layer/layer_settings_util.cpp")
list(APPEND SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Vulkan-Utility-Libraries/src/layer/layer_settings_manager.cpp")

add_library(VkLayer_memstats SHARED ${SOURCES})
target_include_directories(VkLayer_memstats PRIVATE third_party/Vulkan-Headers/include)
target_include_directories(VkLayer_memstats PRIVATE third_party/Vulkan-Utility-Libraries/include)
target_include_directories(VkLayer_memstats PRIVATE third_party/hashtable)
target_include_directories(VkLayer_memstats PRIVATE third_party)

if (${USE_STATIC_STD_LIBRARIES})
    if (MSYS OR MINGW OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_link_options(VkLayer_memstats PRIVATE -static-libgcc -static-libstdc++)
    endif()
    if (MSYS OR MINGW OR (WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"))
        target_link_options(VkLayer_memstats PRIVATE -static -lpthread)
    endif()
endif()

if (${HOOK_MALLOC})
    target_compile_definitions(VkLayer_memstats PRIVATE HOOK_MALLOC)
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
