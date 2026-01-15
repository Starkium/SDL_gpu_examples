cmake_minimum_required(VERSION 3.10)
project(SDL_gpu_examples)
find_package(SDL3 REQUIRED)

# OpenXR SDK path (can be overridden)
set(OPENXR_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../OpenXR-SDK-Source" CACHE PATH "Path to OpenXR SDK source")
set(OPENXR_SDK_BUILD "${OPENXR_SDK_PATH}/build" CACHE PATH "Path to OpenXR SDK build")

add_executable(SDL_gpu_examples
    Examples/main.c
    Examples/Common.h
    stb_image.h
    Examples/Common.c
    Examples/ClearScreen.c
    Examples/ClearScreenMultiWindow.c
    Examples/BasicTriangle.c
    Examples/BasicVertexBuffer.c
    Examples/CullMode.c
    Examples/BasicStencil.c
    Examples/InstancedIndexed.c
    Examples/TexturedQuad.c
    Examples/ComputeSampler.c
    Examples/TexturedAnimatedQuad.c
    Examples/Clear3DSlice.c
    Examples/BasicCompute.c
    Examples/ComputeUniforms.c
    Examples/ToneMapping.c
    Examples/CustomSampling.c
    Examples/DrawIndirect.c
    Examples/ComputeSpriteBatch.c
    Examples/CopyAndReadback.c
    Examples/CopyConsistency.c
    Examples/Texture2DArray.c
    Examples/TriangleMSAA.c
    Examples/Cubemap.c
    Examples/WindowResize.c
    Examples/Blit2DArray.c
    Examples/BlitCube.c
    Examples/BlitMirror.c
    Examples/GenerateMipmaps.c
    Examples/ASTC.c
    Examples/Latency.c
    Examples/BasicVr.c
    Examples/SpinningCubeVr.c
    # Examples/VrGallery.c  # Disabled - needs more work
)

target_link_libraries(SDL_gpu_examples
    SDL3::SDL3
)

# OpenXR includes and library - headers are generated to build/include
target_include_directories(SDL_gpu_examples PRIVATE "${OPENXR_SDK_BUILD}/include")

# First try to find OpenXR in the local SDK build
find_library(OPENXR_LOADER_LIBRARY 
    NAMES openxr_loader
    PATHS "${OPENXR_SDK_BUILD}/src/loader/Release" "${OPENXR_SDK_BUILD}/src/loader/Debug" "${OPENXR_SDK_BUILD}/src/loader"
    NO_DEFAULT_PATH
)

# If not found locally, try system OpenXR
if(NOT OPENXR_LOADER_LIBRARY)
    find_library(OPENXR_LOADER_LIBRARY NAMES openxr_loader)
endif()

if(OPENXR_LOADER_LIBRARY)
    target_link_libraries(SDL_gpu_examples ${OPENXR_LOADER_LIBRARY})
    message(STATUS "Found OpenXR loader: ${OPENXR_LOADER_LIBRARY}")
else()
    message(WARNING "OpenXR loader not found - install libopenxr-dev or build the OpenXR SDK first")
endif()

add_custom_command(TARGET SDL_gpu_examples POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/Content $<TARGET_FILE_DIR:SDL_gpu_examples>/Content
)
