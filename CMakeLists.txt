cmake_minimum_required(VERSION 3.21)

project(DungeonDoorWarp VERSION 1.0.4 LANGUAGES CXX)

if("${PROJECT_SOURCE_DIR}" STREQUAL "${PROJECT_BINARY_DIR}")
	message(FATAL_ERROR "in-source builds are not allowed")
endif()

# CommonLibSSE-NG 6.7.0 source tree (supports 1.6.1170 and 1.7.99)
if(DEFINED ENV{CommonLibSSEPath_NG} AND IS_DIRECTORY "$ENV{CommonLibSSEPath_NG}")
	set(COMMONLIB_DIR "$ENV{CommonLibSSEPath_NG}")
else()
	set(COMMONLIB_DIR "C:/dllmaker/CommonLibSSE-NG")
endif()
message(STATUS "Using CommonLibSSE-NG from: ${COMMONLIB_DIR}")

set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SKSE_SUPPORT_PATCH_SAFETY OFF CACHE BOOL "" FORCE)

add_subdirectory("${COMMONLIB_DIR}" CommonLibSSE EXCLUDE_FROM_ALL)

add_library(${PROJECT_NAME} SHARED
	src/Main.cpp
	src/Settings.cpp
	src/Warp.cpp
	src/PCH.h
	src/Settings.h
	src/Warp.h
)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

target_include_directories(${PROJECT_NAME}
	PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(${PROJECT_NAME}
	PRIVATE
		UNICODE
		_UNICODE
		NOMINMAX
		WIN32_LEAN_AND_MEAN
)

target_compile_options(${PROJECT_NAME}
	PRIVATE
		/permissive-
		/Zc:preprocessor
		/EHsc
		/utf-8
		/MP
		$<$<CONFIG:Release>:/Zi>
)

target_link_options(${PROJECT_NAME}
	PRIVATE
		$<$<CONFIG:Release>:/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO>
)

target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)

target_link_libraries(${PROJECT_NAME} PRIVATE CommonLibSSE::CommonLibSSE)

# No automatic copy: the finished DLL and PDB stay in build\Release\
