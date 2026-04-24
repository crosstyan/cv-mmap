include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

function(cvmmap_configure_compat target_name)
	if(CVMMAP_HAS_STD_EXPECTED)
		target_compile_definitions(${target_name} PUBLIC CVMMAP_HAS_STD_EXPECTED=1)
	else()
		target_compile_definitions(${target_name} PUBLIC CVMMAP_HAS_STD_EXPECTED=0)
		target_link_libraries(${target_name} PUBLIC tl::expected)
	endif()

	if(CVMMAP_HAS_STD_FORMAT)
		target_compile_definitions(${target_name} PUBLIC CVMMAP_HAS_STD_FORMAT=1)
	else()
		target_compile_definitions(${target_name} PUBLIC CVMMAP_HAS_STD_FORMAT=0)
		target_link_libraries(${target_name} PUBLIC fmt::fmt)
	endif()

	target_compile_definitions(
		${target_name}
		PUBLIC CVMMAP_HAS_STD_MOVE_ONLY_FUNCTION=${CVMMAP_HAS_STD_MOVE_ONLY_FUNCTION})
endfunction()


add_library(cvmmap_target STATIC
	src/target.cpp
)
set_target_properties(cvmmap_target PROPERTIES EXPORT_NAME target)
target_compile_features(cvmmap_target PUBLIC cxx_std_23)
target_include_directories(cvmmap_target
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
cvmmap_configure_compat(cvmmap_target)
add_library(cvmmap::target ALIAS cvmmap_target)

add_library(cvmmap_ipc STATIC
	src/protocol.cpp
	src/parser.cpp
)
set_target_properties(cvmmap_ipc PROPERTIES EXPORT_NAME ipc)
target_compile_features(cvmmap_ipc PUBLIC cxx_std_23)
target_include_directories(cvmmap_ipc
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
cvmmap_configure_compat(cvmmap_ipc)
add_library(cvmmap::ipc ALIAS cvmmap_ipc)

add_library(cvmmap_client STATIC
	src/client.cpp
)
set_target_properties(cvmmap_client PROPERTIES EXPORT_NAME client)
target_compile_features(cvmmap_client PUBLIC cxx_std_23)
target_include_directories(cvmmap_client
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(cvmmap_client
	PUBLIC
		cvmmap_target
		cvmmap_ipc
		cvmmap_nats
		cppzmq
		spdlog::spdlog
)
cvmmap_configure_compat(cvmmap_client)
add_library(cvmmap::client ALIAS cvmmap_client)

# -- Protobuf generated library --
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS
	proto/cvmmap/control.proto
)
add_library(cvmmap_proto STATIC ${PROTO_SRCS} ${PROTO_HDRS})
set_target_properties(cvmmap_proto PROPERTIES EXPORT_NAME proto)
target_compile_features(cvmmap_proto PUBLIC cxx_std_23)
target_include_directories(cvmmap_proto
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
		$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(cvmmap_proto
	PUBLIC
		protobuf::libprotobuf
)
cvmmap_configure_compat(cvmmap_proto)
add_library(cvmmap::proto ALIAS cvmmap_proto)

# -- NATS control library --
add_library(cvmmap_nats STATIC
	src/nats_service.cpp
	src/nats_client.cpp
)
set_target_properties(cvmmap_nats PROPERTIES EXPORT_NAME nats)
target_compile_features(cvmmap_nats PUBLIC cxx_std_23)
target_include_directories(cvmmap_nats
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(cvmmap_nats
	PUBLIC
		cvmmap_target
		cvmmap_ipc
		cvmmap_proto
		cnats::nats_static
		spdlog::spdlog
)
cvmmap_configure_compat(cvmmap_nats)
add_library(cvmmap::nats ALIAS cvmmap_nats)

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

export(
	TARGETS
		cvmmap_target
		cvmmap_ipc
		cvmmap_client
		cvmmap_proto
		cvmmap_nats
	FILE "${CMAKE_CURRENT_BINARY_DIR}/cvmmap-coreTargets.cmake"
	NAMESPACE cvmmap::
)

install(
	TARGETS
		cvmmap_target
		cvmmap_ipc
		cvmmap_client
		cvmmap_proto
		cvmmap_nats
	EXPORT cvmmap-coreTargets
	ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
	LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
	RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

install(
	FILES
		${PROTO_HDRS}
	DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(
	DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
	DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(
	DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/proto/"
	DESTINATION "${CMAKE_INSTALL_DATADIR}/cvmmap-core/proto"
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

if (CVMMAP_BUILD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()
