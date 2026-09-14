cmake_minimum_required(VERSION 3.25)
project(Opengl_Physx LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "This project requires 64-bit Windows and MSVC.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(APP_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "Application source directory")
set(PHYSX_ROOT "D:/Dev/.cpp/_Library/PhysX-main" CACHE PATH "PhysX source root")
set(PHYSX_BIN "${PHYSX_ROOT}/physx/bin/win.x86_64.vc143.md/release")
set(BLAST_SDK "${PHYSX_ROOT}/blast/_build/windows-x86_64/release/blast-sdk")
set(GLFW_ROOT "${APP_SOURCE_DIR}/../packages/glfw.3.4.0/build/native")
set(GLM_ROOT "${APP_SOURCE_DIR}/../packages/glm.1.0.3/build/native")
set(IMGUI_ROOT "${APP_SOURCE_DIR}/ThirdParty/imgui")

foreach(required_path IN ITEMS "${PHYSX_ROOT}/physx/include" "${PHYSX_BIN}" "${BLAST_SDK}/include" "${BLAST_SDK}/bin" "${GLFW_ROOT}" "${GLM_ROOT}" "${IMGUI_ROOT}")
    if(NOT EXISTS "${required_path}")
        message(FATAL_ERROR "Required dependency path does not exist: ${required_path}")
    endif()
endforeach()

add_executable(Opengl_Physx WIN32
    "${APP_SOURCE_DIR}/main.cpp"
    "${IMGUI_ROOT}/imgui.cpp"
    "${IMGUI_ROOT}/imgui_draw.cpp"
    "${IMGUI_ROOT}/imgui_tables.cpp"
    "${IMGUI_ROOT}/imgui_widgets.cpp"
    "${IMGUI_ROOT}/backends/imgui_impl_glfw.cpp"
    "${IMGUI_ROOT}/backends/imgui_impl_opengl3.cpp"
)

file(GLOB PROJECT_HEADERS CONFIGURE_DEPENDS "${APP_SOURCE_DIR}/*.h")
target_sources(Opengl_Physx PRIVATE ${PROJECT_HEADERS})

target_compile_definitions(Opengl_Physx PRIVATE GLFW_DLL NOMINMAX)
target_compile_options(Opengl_Physx PRIVATE /utf-8)
target_link_options(Opengl_Physx PRIVATE /ENTRY:mainCRTStartup)

target_include_directories(Opengl_Physx PRIVATE
    "${APP_SOURCE_DIR}"
    "${IMGUI_ROOT}"
    "${GLFW_ROOT}/include"
    "${GLM_ROOT}/include"
    "${PHYSX_ROOT}/physx/include"
    "${BLAST_SDK}/include/globals"
    "${BLAST_SDK}/include/lowlevel"
    "${BLAST_SDK}/include/toolkit"
    "${BLAST_SDK}/include/extensions/assetutils"
    "${BLAST_SDK}/include/extensions/authoring"
    "${BLAST_SDK}/include/extensions/authoringCommon"
    "${BLAST_SDK}/include/extensions/shaders"
    "${BLAST_SDK}/include/shared/NvFoundation"
)

target_link_directories(Opengl_Physx PRIVATE "${PHYSX_BIN}" "${BLAST_SDK}/bin")
target_link_libraries(Opengl_Physx PRIVATE
    "${GLFW_ROOT}/lib/dynamic/v143/x64/glfw3dll.lib"
    opengl32
    PhysX_64
    PhysXCommon_64
    PhysXFoundation_64
    PhysXExtensions_static_64
    PhysXPvdSDK_static_64
    PhysXCooking_64
    NvBlast
    NvBlastGlobals
    NvBlastTk
    NvBlastExtAssetUtils
    NvBlastExtAuthoring
    NvBlastExtShaders
)

set_target_properties(Opengl_Physx PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:Opengl_Physx>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_custom_command(TARGET Opengl_Physx POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${APP_SOURCE_DIR}/Assets" "$<TARGET_FILE_DIR:Opengl_Physx>/Assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GLFW_ROOT}/bin/dynamic/v143/x64/glfw3.dll" "$<TARGET_FILE_DIR:Opengl_Physx>"
)

file(GLOB PHYSX_DLLS "${PHYSX_BIN}/*.dll")
file(GLOB BLAST_DLLS "${BLAST_SDK}/bin/NvBlast*.dll")
foreach(runtime_dll IN LISTS PHYSX_DLLS BLAST_DLLS)
    add_custom_command(TARGET Opengl_Physx POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${runtime_dll}" "$<TARGET_FILE_DIR:Opengl_Physx>")
endforeach()
