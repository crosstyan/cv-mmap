include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(cvmmap_target STATIC
	src/target.cpp
)
set_target_properties(cvmmap_target PROPERTIES EXPORT_NAME target)
target_include_directories(cvmmap_target
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
add_library(cvmmap::target ALIAS cvmmap_target)

add_library(cvmmap_ipc STATIC
	src/parser.cpp
)
set_target_properties(cvmmap_ipc PROPERTIES EXPORT_NAME ipc)
target_include_directories(cvmmap_ipc
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
add_library(cvmmap::ipc ALIAS cvmmap_ipc)

add_library(cvmmap_client STATIC
	src/client.cpp
)
set_target_properties(cvmmap_client PROPERTIES EXPORT_NAME client)
target_include_directories(cvmmap_client
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(cvmmap_client
	PUBLIC
		cvmmap_target
		cvmmap_ipc
		cppzmq
		spdlog
		fmt::fmt
)
add_library(cvmmap::client ALIAS cvmmap_client)

write_basic_package_version_file(
	"${CMAKE_CURRENT_BINARY_DIR}/cvmmap-coreConfigVersion.cmake"
	VERSION 0.1.0
	COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/cmake/cvmmap-coreConfig.cmake.in"
	"${CMAKE_CURRENT_BINARY_DIR}/cvmmap-coreConfig.cmake"
	INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cvmmap-core"
)

install(
	TARGETS
		cvmmap_target
		cvmmap_ipc
		cvmmap_client
	EXPORT cvmmap-coreTargets
	ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
	LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
	RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

install(
	DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
	DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(
	DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/"
	DESTINATION "${CMAKE_INSTALL_DATADIR}/cvmmap-core/fixtures"
)

install(
	EXPORT cvmmap-coreTargets
	FILE cvmmap-coreTargets.cmake
	NAMESPACE cvmmap::
	DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cvmmap-core"
)

install(
	FILES
		"${CMAKE_CURRENT_BINARY_DIR}/cvmmap-coreConfig.cmake"
		"${CMAKE_CURRENT_BINARY_DIR}/cvmmap-coreConfigVersion.cmake"
	DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cvmmap-core"
)
