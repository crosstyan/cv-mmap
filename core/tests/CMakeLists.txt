add_executable(cvmmap_core_tests
	body_tracking_parser_test.cpp
)

target_link_libraries(cvmmap_core_tests
	PRIVATE
		cvmmap_target
		cvmmap_ipc
)

add_test(
	NAME cvmmap_core_tests
	COMMAND cvmmap_core_tests
)

add_executable(cvmmap_frame_metadata_tests
	frame_metadata_parser_test.cpp
)

target_link_libraries(cvmmap_frame_metadata_tests
	PRIVATE
		cvmmap_ipc
)

add_test(
	NAME cvmmap_frame_metadata_tests
	COMMAND cvmmap_frame_metadata_tests
)

add_executable(cvmmap_control_api_tests
	control_api_test.cpp
)

target_link_libraries(cvmmap_control_api_tests
	PRIVATE
		cvmmap_ipc
)

add_test(
	NAME cvmmap_control_api_tests
	COMMAND cvmmap_control_api_tests
)

add_executable(cvmmap_compat_functional_tests
	compat_functional_test.cpp
)

target_link_libraries(cvmmap_compat_functional_tests
	PRIVATE
		cvmmap_target
)

target_compile_definitions(cvmmap_compat_functional_tests
	PRIVATE
		CVMMAP_FORCE_COMPAT_MOVE_ONLY_FUNCTION=1
)

add_test(
	NAME cvmmap_compat_functional_tests
	COMMAND cvmmap_compat_functional_tests
)

add_executable(cvmmap_protocol_fixture_writer
	protocol_fixture_writer.cpp
)

target_link_libraries(cvmmap_protocol_fixture_writer
	PRIVATE
		cvmmap_target
		cvmmap_ipc
)

add_test(
	NAME cvmmap_protocol_fixture_verify
	COMMAND
		cvmmap_protocol_fixture_writer
		--verify
		${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/protocol
)
