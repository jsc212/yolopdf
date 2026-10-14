# Keep MuPDF and its headers private to the engine implementation.
include(ExternalProject)
find_package(Git REQUIRED)
set(mupdf_source "${PROJECT_SOURCE_DIR}/third_party/mupdf")
set(mupdf_output "${PROJECT_BINARY_DIR}/mupdf")
set(mupdf_commit "8ad45e92f0935d3d87f1db3f873086472a5e1b24") # 1.28.5
if(EXISTS "${mupdf_source}/.git")
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${mupdf_source}" rev-parse HEAD
        OUTPUT_VARIABLE actual_mupdf_commit OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE git_result)
elseif(EXISTS "${PROJECT_SOURCE_DIR}/SOURCE_COMMITS.json")
    # Corresponding-source archives contain exact dependency provenance rather
    # than Git internals. The bundler verifies every initialized submodule.
    file(READ "${PROJECT_SOURCE_DIR}/SOURCE_COMMITS.json" bundled_provenance)
    string(JSON actual_mupdf_commit ERROR_VARIABLE provenance_error GET "${bundled_provenance}" commits "third_party/mupdf")
    if(provenance_error)
        set(git_result 1)
    else()
        set(git_result 0)
    endif()
else()
    set(git_result 1)
endif()
if(NOT git_result EQUAL 0 OR NOT actual_mupdf_commit STREQUAL mupdf_commit)
    message(FATAL_ERROR "Expected MuPDF 1.28.5 (${mupdf_commit}). Run git submodule update --init --recursive.")
endif()
if(NOT EXISTS "${mupdf_source}/thirdparty/freetype/include/ft2build.h")
    message(FATAL_ERROR "MuPDF dependencies missing. Run git submodule update --init --recursive.")
endif()

if(MSVC)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        message(FATAL_ERROR "The current MSVC MuPDF wrapper supports Release builds only.")
    endif()
    find_program(mupdf_msbuild MSBuild REQUIRED)
    # Upstream's libmupdf project incorporates its project-reference libraries.
    set(mupdf_archive "${mupdf_output}/libmupdf.lib")
    ExternalProject_Add(mupdf_build SOURCE_DIR "${mupdf_source}"
        CONFIGURE_COMMAND ""
        BUILD_COMMAND "${mupdf_msbuild}" "${mupdf_source}/platform/win32/libmupdf.vcxproj"
            /m:${YOLOPDF_MUPDF_JOBS} /p:Configuration=Release /p:Platform=x64
            /p:PlatformToolset=v143 "/p:OutDir=${mupdf_output}/"
        BUILD_BYPRODUCTS "${mupdf_archive}"
        BUILD_ALWAYS TRUE INSTALL_COMMAND "")
    add_library(yolopdf_mupdf INTERFACE)
    target_link_libraries(yolopdf_mupdf INTERFACE "${mupdf_archive}" crypt32)
else()
    find_program(mupdf_make NAMES gmake make REQUIRED)
    set(mupdf_archive "${mupdf_output}/libmupdf.a")
    set(mupdf_third_archive "${mupdf_output}/libmupdf-third.a")
    set(mupdf_cross_flags)
    set(mupdf_extra_cflags "-fPIC")
    if(ANDROID)
        # The pinned upstream random.c includes unistd.h for getentropy,
        # but Bionic declares it in sys/random.h. Keep the vendor tree pristine.
        set(mupdf_extra_cflags "-fPIC -include sys/random.h")
        # Avoid host pkg-config and host objcopy during NDK cross compilation.
        if(ANDROID_ABI STREQUAL "arm64-v8a")
            set(mupdf_android_triple "aarch64-linux-android")
        elseif(ANDROID_ABI STREQUAL "x86_64")
            set(mupdf_android_triple "x86_64-linux-android")
        else()
            message(FATAL_ERROR "MuPDF cross build currently supports arm64-v8a and x86_64.")
        endif()
        get_filename_component(mupdf_ndk_bin "${CMAKE_C_COMPILER}" DIRECTORY)
        get_filename_component(mupdf_ndk_sysroot "${mupdf_ndk_bin}/../sysroot" ABSOLUTE)
        if(NOT EXISTS "${mupdf_ndk_sysroot}/usr/include/android")
            message(FATAL_ERROR "Cannot locate the NDK sysroot beside ${CMAKE_C_COMPILER}.")
        endif()
        # Qt/NDK legacy toolchains use CMAKE_SYSTEM_VERSION=1 as a sentinel.
        # The actual API level is ANDROID_PLATFORM_LEVEL, not that sentinel.
        if(DEFINED ANDROID_PLATFORM_LEVEL)
            set(mupdf_android_api "${ANDROID_PLATFORM_LEVEL}")
        else()
            string(REGEX MATCH "[0-9]+$" mupdf_android_api "${ANDROID_PLATFORM}")
        endif()
        if(NOT mupdf_android_api MATCHES "^[0-9]+$" OR mupdf_android_api LESS 28)
            message(FATAL_ERROR "MuPDF cross build requires Android API 28 or newer.")
        endif()
        set(mupdf_cross_flags "OS=Android" "HAVE_OBJCOPY=no" "HAVE_LIBCRYPTO=no"
            "HAVE_GLUT=no" "HAVE_X11=no"
            "CC=${CMAKE_C_COMPILER} --target=${mupdf_android_triple}${mupdf_android_api} --sysroot=${mupdf_ndk_sysroot}"
            "CXX=${CMAKE_CXX_COMPILER} --target=${mupdf_android_triple}${mupdf_android_api} --sysroot=${mupdf_ndk_sysroot}"
            "AR=${CMAKE_AR}")
    endif()
    ExternalProject_Add(mupdf_build SOURCE_DIR "${mupdf_source}"
        CONFIGURE_COMMAND ""
        BUILD_COMMAND "${mupdf_make}" -C "${mupdf_source}" -j${YOLOPDF_MUPDF_JOBS}
            "OUT=${mupdf_output}" build=release shared=no USE_SYSTEM_LIBS=no
            HAVE_LIBCRYPTO=no extract=no tesseract=no barcode=no
            "XCFLAGS=${mupdf_extra_cflags}" ${mupdf_cross_flags} libs
        BUILD_BYPRODUCTS "${mupdf_archive}" "${mupdf_third_archive}"
        BUILD_ALWAYS TRUE INSTALL_COMMAND "")
    find_package(Threads REQUIRED)
    add_library(yolopdf_mupdf INTERFACE)
    target_link_libraries(yolopdf_mupdf INTERFACE
        "${mupdf_archive}" "${mupdf_third_archive}" Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
target_include_directories(yolopdf_mupdf INTERFACE "${mupdf_source}/include")
add_dependencies(yolopdf_mupdf mupdf_build)
