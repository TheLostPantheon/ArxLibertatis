
# PS Vita CMake Toolchain File for Arx Libertatis
# Requires VitaSDK to be installed (https://vitasdk.org/)

if(DEFINED ENV{VITASDK})
	set(VITASDK "$ENV{VITASDK}")
else()
	set(VITASDK "/usr/local/vitasdk")
endif()

if(NOT EXISTS "${VITASDK}")
	message(FATAL_ERROR "VitaSDK not found at ${VITASDK}. Set VITASDK environment variable.")
endif()

# Include the official VitaSDK toolchain
include("${VITASDK}/share/vita.toolchain.cmake")

# Include vita.cmake for vita_create_self() and vita_create_vpk()
include("${VITASDK}/share/vita.cmake" OPTIONAL)

# Platform identification
set(VITA TRUE CACHE BOOL "Building for PS Vita")
set(CMAKE_SYSTEM_NAME "Generic")
set(CMAKE_SYSTEM_PROCESSOR "armv7-a")

# Ensure __vita__ is defined for platform detection
add_definitions(-D__vita__)

# C++17 support
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# vitaGL is built automatically by CMakeLists.txt with the 60 FPS flag set:
#   NO_DEBUG=1 MATH_SPEEDHACK=1 DRAW_SPEEDHACK=2
#   INDICES_DRAW_SPEEDHACK=1 USE_SCRATCH_MEMORY=1
#   CIRCULAR_VERTEX_POOL=2 HAVE_SHADER_CACHE=1
#   SINGLE_THREADED_GC=1

#   Do not underestimate the power of
#	BUFFERS_SPEEDHACK=1(This one is most important since it fixes buffer limits for cpu gpu interactions)
#	DRAW_SPEEDHACK=2
#	CIRCULAR_VERTEX_POOL=2

# CIRCULAR_VERTEX_POOL=2 is CRITICAL: double-buffered vertex pool allows
# CPU/GPU pipelining without glFinish() stalls.
#
# Do NOT use:
#   USE_SCRATCH_MEMORY=1
#	INDICES_DRAW_SPEEDHACK=1
#	TEXTURES_SPEEDHACK=1(breaks rendering)
#   NO_TEX_COMBINER (Arx uses GL_COMBINE)
#   DRAW_SPEEDHACK=1 (too aggressive — use =2)
#   PRIMITIVES_SPEEDHACK (breaks polygon mode)
#   HAVE_WVP_ON_GPU (Arx submits pre-transformed 4D clip-space vertices — double-transforms)
