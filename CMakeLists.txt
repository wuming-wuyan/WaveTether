cmake_minimum_required(VERSION 3.20)
project(WaveTether VERSION 1.1.0 LANGUAGES CXX RC)

if(NOT MSVC)
    message(FATAL_ERROR "WaveTether 只支持 Windows + MSVC")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

# CMake 默认会往编译行塞 /DWIN32 /D_WINDOWS /W3 /GR /EHsc；build.bat 没有这些。
# 为了让两条链的产物尽量一致，这里清空默认值，只保留下面 target 上显式给的参数。
set(CMAKE_CXX_FLAGS "")
set(CMAKE_CXX_FLAGS_RELEASE "/O1")
set(CMAKE_C_FLAGS "")
set(CMAKE_C_FLAGS_RELEASE "/O1")

set(NATIVE "${CMAKE_CURRENT_SOURCE_DIR}/native")
set(WT_SOURCES "${NATIVE}/main.cpp" "${NATIVE}/audio.cpp" "${NATIVE}/adb.cpp")
set(WT_LIBS windowsapp user32 gdi32 gdiplus shell32 advapi32 ole32 oleaut32 imm32)

# 与 build.bat 保持同一组编译/链接参数，产物才可能逐字节一致。
function(wt_configure target)
    target_include_directories(${target} PRIVATE "${NATIVE}")
    target_compile_options(${target} PRIVATE
        /O1 /utf-8 /EHsc /Brepro /Gy /Gw /GL /DUNICODE /D_UNICODE)
    target_link_options(${target} PRIVATE /LTCG /OPT:REF /OPT:ICF)
    target_link_libraries(${target} PRIVATE ${WT_LIBS})
endfunction()

add_executable(WaveTether WIN32 ${WT_SOURCES} "${NATIVE}/app.rc")
wt_configure(WaveTether)

add_executable(WaveTether-BT-only WIN32 ${WT_SOURCES} "${NATIVE}/app.rc")
wt_configure(WaveTether-BT-only)
target_compile_definitions(WaveTether-BT-only PRIVATE BT_ONLY)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
# gen_embed 的输入是 tools/scrcpy 下的文件，不是 _embedded_tools.inc；
# 漏掉这里会在换了内嵌工具后继续产出旧 payload 的单文件 exe。
file(GLOB WT_SCRCPY_INPUTS CONFIGURE_DEPENDS "${NATIVE}/tools/scrcpy/*/*")
add_custom_command(
    OUTPUT "${NATIVE}/embed.rc"
    BYPRODUCTS "${NATIVE}/embed_manifest.txt" "${NATIVE}/embed_hash.txt" "${NATIVE}/embed_blobs"
    COMMAND ${Python3_EXECUTABLE} gen_embed.py
    WORKING_DIRECTORY "${NATIVE}"
    DEPENDS "${NATIVE}/gen_embed.py" ${WT_SCRCPY_INPUTS}
    COMMENT "生成单文件版内嵌资源")
add_custom_target(gen_embed DEPENDS "${NATIVE}/embed.rc")

add_executable(WaveTether-single WIN32 ${WT_SOURCES} "${NATIVE}/app.rc" "${NATIVE}/embed.rc")
wt_configure(WaveTether-single)
add_dependencies(WaveTether-single gen_embed)
