cmake_minimum_required(VERSION 3.22)
project(qqauto CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(qqauto SHARED
        src/auto_qqspeed.cpp
)

target_compile_options(qqauto PRIVATE
        -O2 -fvisibility=hidden -fno-exceptions -fno-rtti
        -Wall -Wextra -Wno-unused-parameter
)

target_link_options(qqauto PRIVATE
        -Wl,-z,max-page-size=16384     # 适配 Android 15 的 16KB 页（原始 libqc.so 这里是 4KB，会在新机上加载失败）
        -Wl,--build-id=sha1
        -Wl,-soname,libqqauto.so
)

# 只依赖 NDK 内置的 log / dl
find_library(log-lib log)
find_library(dl-lib dl)
target_link_libraries(qqauto ${log-lib} ${dl-lib})

set_target_properties(qqauto PROPERTIES OUTPUT_NAME "qqauto")
