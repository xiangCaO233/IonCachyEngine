# cmake/configure-project.cmake

# 添加avx指令集编译选项
if(APPLE)

else()
    if(MSVC)
        # add_compile_options("/arch:AVX2")
    else()
        add_compile_options("-mavx2")
    endif()
endif()

# 运行时内存检查 set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fsanitize=address")
# set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -fsanitize=address")
# set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
