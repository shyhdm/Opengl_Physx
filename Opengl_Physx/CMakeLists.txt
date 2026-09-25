cmake_minimum_required(VERSION 3.26)
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
set(FLOW_ROOT "${PHYSX_ROOT}/flow" CACHE PATH "NVIDIA Flow root")
set(FLOW_BIN "${FLOW_ROOT}/_build/windows-x86_64/release")
set(GLFW_ROOT "${APP_SOURCE_DIR}/../packages/glfw.3.4.0/build/native" CACHE PATH "GLFW native package root")
set(GLM_ROOT "${APP_SOURCE_DIR}/../packages/glm.1.0.3/build/native" CACHE PATH "GLM native package root")
set(IMGUI_ROOT "${APP_SOURCE_DIR}/ThirdParty/imgui")

foreach(required_path IN ITEMS
    "${PHYSX_ROOT}/physx/include"
    "${PHYSX_BIN}"
    "${BLAST_SDK}/include"
    "${BLAST_SDK}/bin"
    "${FLOW_ROOT}/include"
    "${FLOW_BIN}"
    "${GLFW_ROOT}"
    "${GLM_ROOT}"
    "${IMGUI_ROOT}"
)
    if(NOT EXISTS "${required_path}")
        message(FATAL_ERROR "Required dependency path does not exist: ${required_path}")
    endif()
endforeach()

add_executable(Opengl_Physx WIN32
    "${APP_SOURCE_DIR}/src/main.cpp"
    "${IMGUI_ROOT}/imgui.cpp"
    "${IMGUI_ROOT}/imgui_draw.cpp"
    "${IMGUI_ROOT}/imgui_tables.cpp"
    "${IMGUI_ROOT}/imgui_widgets.cpp"
    "${IMGUI_ROOT}/backends/imgui_impl_glfw.cpp"
    "${IMGUI_ROOT}/backends/imgui_impl_opengl3.cpp"
)

file(GLOB_RECURSE PROJECT_HEADERS CONFIGURE_DEPENDS "${APP_SOURCE_DIR}/src/*.h")
target_sources(Opengl_Physx PRIVATE ${PROJECT_HEADERS})

target_compile_definitions(Opengl_Physx PRIVATE GLFW_DLL NOMINMAX)
target_compile_options(Opengl_Physx PRIVATE /utf-8)
target_link_options(Opengl_Physx PRIVATE /ENTRY:mainCRTStartup)

target_include_directories(Opengl_Physx PRIVATE
    "${APP_SOURCE_DIR}"
    "${APP_SOURCE_DIR}/src"
    "${APP_SOURCE_DIR}/src/Platform"
    "${APP_SOURCE_DIR}/src/Scene"
    "${APP_SOURCE_DIR}/src/Data"
    "${APP_SOURCE_DIR}/src/Editor"
    "${APP_SOURCE_DIR}/src/Graphics"
    "${APP_SOURCE_DIR}/src/Physics/Blast"
    "${APP_SOURCE_DIR}/src/Physics/Flow"
    "${APP_SOURCE_DIR}/src/Physics/PhysicsWorld"
    "${APP_SOURCE_DIR}/src/Physics/RigidBody"
    "${APP_SOURCE_DIR}/src/Physics/SoftBody"
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
    "${FLOW_ROOT}/include"
    "${FLOW_ROOT}/include/nvflow"
    "${FLOW_ROOT}/include/nvflowext"
)

target_link_directories(Opengl_Physx PRIVATE
    "${PHYSX_BIN}"
    "${BLAST_SDK}/bin"
    "${FLOW_BIN}"
)

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
    nvflow
    nvflowext
)

set_target_properties(Opengl_Physx PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:Opengl_Physx>"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Check assets whenever the application target is built, even if no C++ input
# changed. This copies only changed files and does not trigger reconfiguration.
add_custom_target(SyncAssets
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        "${APP_SOURCE_DIR}/Assets"
        "$<TARGET_FILE_DIR:Opengl_Physx>/Assets"
    COMMENT "Syncing changed application assets"
    VERBATIM
)
add_dependencies(Opengl_Physx SyncAssets)

add_custom_command(TARGET Opengl_Physx POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${GLFW_ROOT}/bin/dynamic/v143/x64/glfw3.dll"
        "$<TARGET_FILE_DIR:Opengl_Physx>"
)

file(GLOB PHYSX_DLLS "${PHYSX_BIN}/*.dll")
file(GLOB BLAST_DLLS "${BLAST_SDK}/bin/NvBlast*.dll")
file(GLOB FLOW_DLLS
    "${FLOW_BIN}/nvflow*.dll"
    "${FLOW_BIN}/slang*.dll"
)

foreach(runtime_dll IN LISTS PHYSX_DLLS BLAST_DLLS FLOW_DLLS)
    add_custom_command(TARGET Opengl_Physx POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${runtime_dll}"
            "$<TARGET_FILE_DIR:Opengl_Physx>"
    )
endforeach()

# Optional portable distribution, enabled only by the release build script.
option(APP_PORTABLE_RUNTIME "Copy MSVC redistributable runtime beside the EXE" OFF)
if(APP_PORTABLE_RUNTIME)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    include(InstallRequiredSystemLibraries)
    if(NOT CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        message(FATAL_ERROR "MSVC redistributable runtime was not found")
    endif()
    foreach(runtime_file IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        add_custom_command(TARGET Opengl_Physx POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${runtime_file}" "$<TARGET_FILE_DIR:Opengl_Physx>"
            VERBATIM)
    endforeach()
endif()

