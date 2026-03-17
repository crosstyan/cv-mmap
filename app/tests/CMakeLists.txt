add_executable(app_dummy_backend_tests
	dummy_backend_test.cpp
)

target_include_directories(app_dummy_backend_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(app_dummy_backend_tests
	PRIVATE
		app_video_backends
		app_config
		spdlog::spdlog
		fmt::fmt
)

add_test(
	NAME app_dummy_backend_tests
	COMMAND app_dummy_backend_tests
)
