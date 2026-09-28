cmake_minimum_required(VERSION 3.16)
project(Ksila VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(KSILA_PREBUILT_SHADERS "Use the checked-in SPIR-V binaries even if a GLSL compiler is found" OFF)
option(KSILA_GLFW_NULL "Build GLFW with its headless 'null' platform (for builds without X11/Wayland dev packages)" OFF)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(Vulkan REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

if(NOT ANDROID)
	# GLFW: prefer a system install, otherwise fetch and build it.
	find_package(glfw3 3.3 QUIET)
	if(NOT glfw3_FOUND)
		message(STATUS "GLFW not found - fetching from GitHub (needs git)")
		include(FetchContent)
		FetchContent_Declare(glfw
			GIT_REPOSITORY https://github.com/glfw/glfw.git
			GIT_TAG 3.4)
		set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
		set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
		set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
		set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
		if(KSILA_GLFW_NULL)
			set(GLFW_BUILD_X11 OFF CACHE BOOL "" FORCE)
			set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
			set(GLFW_BUILD_NULL ON CACHE BOOL "" FORCE)
		endif()
		FetchContent_MakeAvailable(glfw)
	endif()
endif()

# ---------------------------------------------------------------------------
# Generated sources (embedded fonts + shaders)
# ---------------------------------------------------------------------------
set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GEN_DIR})

set(EMBED_TOOL ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_binary.py)
set(FONT_REGULAR ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/NotoSans-Regular.ttf)
set(FONT_BOLD ${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/NotoSans-Bold.ttf)

add_custom_command(OUTPUT ${GEN_DIR}/font_data.cpp
	COMMAND ${Python3_EXECUTABLE} ${EMBED_TOOL} --input ${FONT_REGULAR}
		--symbol FONT_REGULAR_TTF --output ${GEN_DIR}/font_data.cpp
	DEPENDS ${EMBED_TOOL} ${FONT_REGULAR}
	COMMENT "Embedding NotoSans-Regular.ttf")
add_custom_command(OUTPUT ${GEN_DIR}/font_data_bold.cpp
	COMMAND ${Python3_EXECUTABLE} ${EMBED_TOOL} --input ${FONT_BOLD}
		--symbol FONT_BOLD_TTF --output ${GEN_DIR}/font_data_bold.cpp
	DEPENDS ${EMBED_TOOL} ${FONT_BOLD}
	COMMENT "Embedding NotoSans-Bold.ttf")

# --- SPIR-V: compile from GLSL when possible, fall back to checked-in .spv ---
set(SHADER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_PREBUILT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders/prebuilt)

find_program(GLSLC_BIN NAMES glslc)
find_program(GLSLANG_BIN NAMES glslangValidator)

set(SHADER_COMPILER "")
if(GLSLC_BIN)
	set(SHADER_COMPILER ${GLSLC_BIN})
elseif(GLSLANG_BIN)
	set(SHADER_COMPILER ${GLSLANG_BIN})
endif()

if(NOT SHADER_COMPILER AND NOT EXISTS ${SHADER_PREBUILT_DIR}/lobby.vert.spv)
	message(FATAL_ERROR "No GLSL-to-SPIR-V compiler (glslc/glslangValidator) found and no prebuilt shaders in ${SHADER_PREBUILT_DIR}. Install the Vulkan SDK.")
endif()

function(ksila_shader GLSL_FILE OUT_SPV)
	if(KSILA_PREBUILT_SHADERS OR NOT SHADER_COMPILER)
		if(NOT KSILA_PREBUILT_SHADERS)
			message(STATUS "No GLSL compiler found - using prebuilt SPIR-V for ${GLSL_FILE}")
		endif()
		add_custom_command(OUTPUT ${OUT_SPV}
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
				${SHADER_PREBUILT_DIR}/${GLSL_FILE}.spv ${OUT_SPV}
			DEPENDS ${SHADER_PREBUILT_DIR}/${GLSL_FILE}.spv
			COMMENT "Copying prebuilt ${GLSL_FILE}.spv")
	else()
		if(SHADER_COMPILER STREQUAL GLSLC_BIN)
			add_custom_command(OUTPUT ${OUT_SPV}
				COMMAND ${GLSLC_BIN} ${SHADER_SRC_DIR}/${GLSL_FILE} -o ${OUT_SPV}
				DEPENDS ${SHADER_SRC_DIR}/${GLSL_FILE}
				COMMENT "glslc ${GLSL_FILE}")
		else()
			add_custom_command(OUTPUT ${OUT_SPV}
				COMMAND ${GLSLANG_BIN} -V ${SHADER_SRC_DIR}/${GLSL_FILE} -o ${OUT_SPV}
				DEPENDS ${SHADER_SRC_DIR}/${GLSL_FILE}
				COMMENT "glslangValidator ${GLSL_FILE}")
		endif()
	endif()
endfunction()

ksila_shader(lobby.vert ${GEN_DIR}/lobby.vert.spv)
ksila_shader(lobby.frag ${GEN_DIR}/lobby.frag.spv)

add_custom_command(OUTPUT ${GEN_DIR}/shader_lobby_vert.h
	COMMAND ${Python3_EXECUTABLE} ${EMBED_TOOL} --input ${GEN_DIR}/lobby.vert.spv
		--symbol LOBBY_VERT_SPV --type words --output ${GEN_DIR}/shader_lobby_vert.h
	DEPENDS ${EMBED_TOOL} ${GEN_DIR}/lobby.vert.spv
	COMMENT "Embedding lobby.vert SPIR-V")
add_custom_command(OUTPUT ${GEN_DIR}/shader_lobby_frag.h
	COMMAND ${Python3_EXECUTABLE} ${EMBED_TOOL} --input ${GEN_DIR}/lobby.frag.spv
		--symbol LOBBY_FRAG_SPV --type words --output ${GEN_DIR}/shader_lobby_frag.h
	DEPENDS ${EMBED_TOOL} ${GEN_DIR}/lobby.frag.spv
	COMMENT "Embedding lobby.frag SPIR-V")

set(KSILA_COMMON_SOURCES
	src/ui.cpp
	${GEN_DIR}/font_data.cpp
	${GEN_DIR}/font_data_bold.cpp
	${GEN_DIR}/shader_lobby_vert.h
	${GEN_DIR}/shader_lobby_frag.h)

# ---------------------------------------------------------------------------
# Desktop targets (Windows / Linux / macOS)
# ---------------------------------------------------------------------------
if(NOT ANDROID)
	add_executable(ksila
		src/main.cpp
		src/renderer.cpp
		${KSILA_COMMON_SOURCES})
	target_include_directories(ksila PRIVATE src ${GEN_DIR} thirdparty/stb)
	target_link_libraries(ksila PRIVATE Vulkan::Vulkan glfw)

	add_executable(ksila-preview
		tools/preview.cpp
		${KSILA_COMMON_SOURCES})
	target_include_directories(ksila-preview PRIVATE src ${GEN_DIR} thirdparty/stb)
endif()

# ---------------------------------------------------------------------------
# Android target: libksila.so loaded by android.app.NativeActivity
# ---------------------------------------------------------------------------
if(ANDROID)
	if(NOT DEFINED ENV{ANDROID_NDK_HOME} AND NOT DEFINED ANDROID_NDK)
		message(FATAL_ERROR "Use the NDK toolchain file: -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake")
	endif()
	if("${ANDROID_PLATFORM}" STREQUAL "")
		message(FATAL_ERROR "Set -DANDROID_PLATFORM=android-24 or higher (Vulkan requires API 24).")
	endif()

	# native_app_glue ships with the NDK.
	add_library(native_app_glue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
	target_include_directories(native_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)

	add_library(ksila SHARED
		src/platform_android.cpp
		src/renderer.cpp
		${KSILA_COMMON_SOURCES})
	target_include_directories(ksila PRIVATE src ${GEN_DIR} thirdparty/stb)
	target_link_libraries(ksila PRIVATE Vulkan::Vulkan android log native_app_glue)
endif()

# ---------------------------------------------------------------------------
# Shared compiler tweaks
# ---------------------------------------------------------------------------
# stb headers trigger -Wmissing-field-initializers under GCC/Clang.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	foreach(target ksila ksila-preview)
		if(TARGET ${target})
			target_compile_options(${target} PRIVATE -Wno-missing-field-initializers)
		endif()
	endforeach()
endif()
# UTF-8 sources (Cyrillic UI strings) on MSVC.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
	foreach(target ksila ksila-preview)
		if(TARGET ${target})
			target_compile_options(${target} PRIVATE /utf-8)
		endif()
	endforeach()
endif()
