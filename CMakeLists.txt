cmake_minimum_required(VERSION 3.16)
project(framewarp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

find_package(Vulkan REQUIRED)
find_program(GLSLANG glslangValidator REQUIRED)

set(SHADER_HEADERS)
foreach(shader fullscreen.vert warp.frag luma.comp pyramid.comp motion.comp filter.comp refine.comp global.comp)
    string(REPLACE "." "_" var ${shader})
    set(out ${CMAKE_BINARY_DIR}/shaders/${var}.h)
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
        COMMAND ${GLSLANG} -V --vn ${var} -o ${out} ${CMAKE_SOURCE_DIR}/shaders/${shader}
        DEPENDS ${CMAKE_SOURCE_DIR}/shaders/${shader} ${CMAKE_SOURCE_DIR}/shaders/common.glsl)
    list(APPEND SHADER_HEADERS ${out})
endforeach()

add_library(framewarp SHARED src/framewarp.cpp ${SHADER_HEADERS})
target_include_directories(framewarp PRIVATE ${CMAKE_BINARY_DIR}/shaders)
target_link_libraries(framewarp PRIVATE Vulkan::Headers)
# Steam's runtime container may ship an older libstdc++ than the host.
target_link_options(framewarp PRIVATE -static-libstdc++ -static-libgcc -Wl,--no-undefined)
target_compile_options(framewarp PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
set_target_properties(framewarp PROPERTIES OUTPUT_NAME VkLayer_framewarp)

set(FRAMEWARP_LIBRARY_PATH "./libVkLayer_framewarp.so" CACHE STRING "library_path written in the layer manifest")
configure_file(layer/VkLayer_framewarp.json.in ${CMAKE_BINARY_DIR}/VkLayer_framewarp.json @ONLY)

option(FRAMEWARP_TESTS "Build the shader tests (they need a Vulkan device to run)" ON)
if(FRAMEWARP_TESTS)
    enable_testing()
    add_executable(motion_test tests/motion_test.cpp ${SHADER_HEADERS})
    target_include_directories(motion_test PRIVATE ${CMAKE_BINARY_DIR}/shaders)
    target_link_libraries(motion_test PRIVATE Vulkan::Vulkan)
    add_test(NAME motion COMMAND motion_test)
endif()
