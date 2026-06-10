cmake_minimum_required(VERSION 3.20)
project(BakeItEngine LANGUAGES CXX)

# Portable build: every dependency (GLFW, glm, stb, assimp, OpenVR and the
# Vulkan SDK headers/libs) is bundled under ThirdParty/, so a fresh clone
# only needs CMake + a C++17 compiler. On Windows:
#
#   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
#   cmake --build build --config Debug
#
# (or open build/BakeItEngine.sln in Visual Studio).
#
# Run from the repo root: main() invokes compile.bat at startup (needs
# glslc.exe on PATH) and loads models/textures/shaders via relative paths.
# The Visual Studio debugger working directory is set up accordingly.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(THIRDPARTY ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty)
set(VULKAN_SDK_DIR ${THIRDPARTY}/VulkanSDK/1.4.304.0)
set(SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Source)

# Engine sources: everything under Source/ except vendored Imgui (it ships
# backends for APIs we don't use, e.g. GLUT). New engine files are picked up
# on the next CMake regenerate.
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS ${SOURCE_DIR}/*.cpp)
list(FILTER ENGINE_SOURCES EXCLUDE REGEX "/Imgui/")

# Vendored ImGui: core + the two backends the engine uses.
set(IMGUI_SOURCES
    ${SOURCE_DIR}/Imgui/imgui.cpp
    ${SOURCE_DIR}/Imgui/imgui_demo.cpp
    ${SOURCE_DIR}/Imgui/imgui_draw.cpp
    ${SOURCE_DIR}/Imgui/imgui_tables.cpp
    ${SOURCE_DIR}/Imgui/imgui_widgets.cpp
    ${SOURCE_DIR}/Imgui/imgui_impl_glfw.cpp
    ${SOURCE_DIR}/Imgui/imgui_impl_vulkan.cpp
)

add_executable(BakeItEngine ${ENGINE_SOURCES} ${IMGUI_SOURCES})

target_include_directories(BakeItEngine PRIVATE
    ${SOURCE_DIR}                      # project-root-relative includes ("Core/...")
    ${THIRDPARTY}/Include              # GLFW/, glm/, stb/, assimp/
    ${VULKAN_SDK_DIR}/Include
    ${THIRDPARTY}/OpenVR/Include
)

target_link_directories(BakeItEngine PRIVATE
    ${VULKAN_SDK_DIR}/Lib
    ${THIRDPARTY}/OpenVR/Libs
    ${THIRDPARTY}/Libs
)

target_link_libraries(BakeItEngine PRIVATE
    vulkan-1
    glfw3
    assimp
    openvr_api
)

if(MSVC)
    # Match the original vcxproj: warnings off, parallel compile, conformance,
    # Unicode character set (TCHAR = wchar_t in VulkanUtils).
    target_compile_options(BakeItEngine PRIVATE /w /MP /permissive-)
    target_compile_definitions(BakeItEngine PRIVATE _CONSOLE UNICODE _UNICODE)
endif()

# Debugging from Visual Studio: working directory must be the repo root so
# compile.bat and the relative asset paths resolve.
set_target_properties(BakeItEngine PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT BakeItEngine)

# Runtime DLLs next to the exe.
add_custom_command(TARGET BakeItEngine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/assimp.dll
        ${CMAKE_CURRENT_SOURCE_DIR}/openvr_api.dll
        ${SOURCE_DIR}/assimp-vc143-mtd.dll
        ${THIRDPARTY}/Libs/glfw3.dll
        $<TARGET_FILE_DIR:BakeItEngine>
)
