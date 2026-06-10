cmake_minimum_required(VERSION 3.21)
project(exsearcher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8 /W4 /permissive-)
endif()

# --- core static library (Qt-independent) ---
add_library(exsearcher-core STATIC
    core/src/Index.cpp
    core/src/CrawlIndexer.cpp
    core/src/SearchEngine.cpp
    core/src/Snapshot.cpp
    core/src/WinText.cpp
)
target_include_directories(exsearcher-core PUBLIC core/include)

# --- CLI test tool ---
add_executable(exsearcher-cli cli/main.cpp)
target_link_libraries(exsearcher-cli PRIVATE exsearcher-core)

# NOTE: the whole project builds against the dynamic CRT (/MD, CMake default).
# Qt's prebuilt binaries are /MD; mixing a /MT core lib into the Qt GUI app would
# be a CRT mismatch. CRT DLLs ship app-local via windeployqt --compiler-runtime.

# --- Qt6 Widgets GUI app ---
find_package(Qt6 COMPONENTS Widgets Concurrent REQUIRED)

# Version/company metadata reduces antivirus heuristic false positives on
# unsigned executables (a bare exe with zero VERSIONINFO scores worst).
qt_add_executable(exsearcher WIN32
    app/exsearcher.rc
    app/resources.qrc
    app/main.cpp
    app/MainWindow.cpp
    app/MainWindow.h
    app/ResultsModel.cpp
    app/ResultsModel.h
    app/IndexController.cpp
    app/IndexController.h
    app/DriveChipBar.cpp
    app/DriveChipBar.h
    app/Watcher.cpp
    app/Watcher.h
)
# AUTORCC is required for app/resources.qrc — without it the qrc is silently
# treated as an inert file and :/style.qss + :/icon256.png don't exist at
# runtime (the app then renders completely unstyled).
set_target_properties(exsearcher PROPERTIES AUTOMOC ON AUTORCC ON)
# dwmapi: DwmExtendFrameIntoClientArea for custom frameless titlebar shadow
target_link_libraries(exsearcher PRIVATE exsearcher-core Qt6::Widgets Qt6::Concurrent dwmapi)

# --- deploy: stage exe + Qt runtime into build/dist/exsearcher/ ---
# windeployqt copies Qt6 DLLs, platform plugin, and (with --compiler-runtime)
# the VC++ runtime DLLs so the dist folder is fully portable. The VC++ runtime
# copy requires VCINSTALLDIR, which is only set inside a VS dev environment, so
# we source vcvars64.bat before invoking windeployqt.
set(_dist_dir "${CMAKE_BINARY_DIR}/dist/exsearcher")
find_program(WINDEPLOYQT_EXE windeployqt HINTS "${Qt6_DIR}/../../../bin")

# Derive vcvars64.bat from the compiler path. cl.exe lives at
# <VSROOT>/VC/Tools/MSVC/<ver>/bin/Hostx64/x64, so up 5 dirs reaches <VSROOT>/VC
# and vcvars64.bat is at <VSROOT>/VC/Auxiliary/Build.
get_filename_component(_msvc_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
get_filename_component(_vc_dir "${_msvc_bin}/../../../../../.." ABSOLUTE)
set(_vcvars "${_vc_dir}/Auxiliary/Build/vcvars64.bat")

# Generate a batch wrapper so quoting of paths containing spaces survives the
# cmd -> windeployqt hop (passing this inline as `cmd /c "..."` mis-parses the
# "C:\Program Files" segments).
file(TO_NATIVE_PATH "${_vcvars}" _vcvars_native)
file(TO_NATIVE_PATH "${WINDEPLOYQT_EXE}" _windeployqt_native)
file(TO_NATIVE_PATH "${_dist_dir}/exsearcher.exe" _dist_exe_native)
set(_deploy_bat "${CMAKE_BINARY_DIR}/run_windeployqt.bat")
# windeployqt flags:
#   --no-compiler-runtime  skip vc_redist.x64.exe (we copy loose CRT DLLs instead)
#   --no-opengl-sw         skip opengl32sw.dll  (~20 MB)
#   --no-system-d3d-compiler  skip d3dcompiler_47.dll + dxil.dll  (~6 MB)
#   --no-system-dxc-compiler  skip dxcompiler.dll  (~14 MB)
#   --skip-plugin-types    omit plugin dirs the app never loads
# Qt6Network / Qt6Svg are pulled in only by the excluded plugin types
# (networkinformation, tls, iconengines) so they disappear automatically.
file(GENERATE OUTPUT "${_deploy_bat}" CONTENT
"@echo off\r\n\
call \"${_vcvars_native}\" || exit /b 1\r\n\
\"${_windeployqt_native}\" --release --no-compiler-runtime --no-translations\
 --no-opengl-sw --no-system-d3d-compiler --no-system-dxc-compiler\
 --skip-plugin-types generic,networkinformation,tls,iconengines\
 \"${_dist_exe_native}\" || exit /b 1\r\n\
rem Remove the redist installer if windeployqt dropped it despite --no-compiler-runtime\r\n\
if exist \"${_dist_dir}\\vc_redist.x64.exe\" del /q \"${_dist_dir}\\vc_redist.x64.exe\"\r\n\
rem Remove any leftover large shader/opengl blobs\r\n\
if exist \"${_dist_dir}\\opengl32sw.dll\"    del /q \"${_dist_dir}\\opengl32sw.dll\"\r\n\
if exist \"${_dist_dir}\\d3dcompiler_47.dll\" del /q \"${_dist_dir}\\d3dcompiler_47.dll\"\r\n\
if exist \"${_dist_dir}\\dxcompiler.dll\"     del /q \"${_dist_dir}\\dxcompiler.dll\"\r\n\
if exist \"${_dist_dir}\\dxil.dll\"           del /q \"${_dist_dir}\\dxil.dll\"\r\n\
rem Remove unneeded plugin subdirs\r\n\
if exist \"${_dist_dir}\\generic\"            rd /s /q \"${_dist_dir}\\generic\"\r\n\
if exist \"${_dist_dir}\\networkinformation\" rd /s /q \"${_dist_dir}\\networkinformation\"\r\n\
if exist \"${_dist_dir}\\tls\"               rd /s /q \"${_dist_dir}\\tls\"\r\n\
if exist \"${_dist_dir}\\iconengines\"        rd /s /q \"${_dist_dir}\\iconengines\"\r\n\
if exist \"${_dist_dir}\\imageformats\"       rd /s /q \"${_dist_dir}\\imageformats\"\r\n\
")

# Locate the loose VC++ CRT redist DLLs to stage app-local.
file(GLOB _crt_dirs "${_vc_dir}/Redist/MSVC/*/x64/Microsoft.VC*.CRT")
set(_crt_dlls "")
if(_crt_dirs)
    list(GET _crt_dirs 0 _crt_dir)
    file(GLOB _crt_dlls "${_crt_dir}/vcruntime140*.dll"
                        "${_crt_dir}/msvcp140*.dll"
                        "${_crt_dir}/concrt140.dll")
endif()

# Fail loudly if the CRT DLLs could not be located: shipping an incomplete
# portable app silently is worse than a noisy configure/build error.
if(MSVC AND NOT _crt_dlls)
    message(WARNING
        "deploy: CRT DLLs not found under ${_vc_dir}/Redist/MSVC/*/x64/Microsoft.VC*.CRT -- "
        "the `deploy` target will exit with an error at build time. "
        "Check that the VC++ runtime redistributable is installed with Visual Studio.")
    # Generate a bat that exits 1 so the build system surfaces the problem.
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/copy_crt_dlls.bat"
         CONTENT "@echo off\r\necho ERROR: CRT DLLs not found; deploy cannot produce a portable app.\r\nexit /b 1\r\n")
    set(_copy_crt_cmd cmd /c call "${CMAKE_BINARY_DIR}/copy_crt_dlls.bat")
else()
    # Happy path: copy the DLLs directly via cmake -E copy_if_different.
    set(_copy_crt_cmd ${CMAKE_COMMAND} -E copy_if_different ${_crt_dlls} "${_dist_dir}/")
endif()

add_custom_target(deploy
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_dist_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:exsearcher>" "${_dist_dir}/"
    COMMAND cmd /c call "${_deploy_bat}"
    COMMAND ${_copy_crt_cmd}
    DEPENDS exsearcher
    COMMENT "Staging exsearcher + Qt runtime into ${_dist_dir}"
)
