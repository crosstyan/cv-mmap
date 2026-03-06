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
