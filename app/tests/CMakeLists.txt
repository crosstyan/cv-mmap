add_executable(app_dummy_backend_tests
	dummy_backend_test.cpp)

target_include_directories(app_dummy_backend_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(app_dummy_backend_tests
	PRIVATE
		app_video_backends
		app_config
		spdlog::spdlog
		fmt::fmt)

add_test(
	NAME app_dummy_backend_tests
	COMMAND app_dummy_backend_tests)

add_executable(app_backend_capability_tests
	backend_capability_test.cpp)

target_include_directories(app_backend_capability_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(app_backend_capability_tests
	PRIVATE
		app_video_backends
		app_config
		spdlog::spdlog
		fmt::fmt)

add_test(
	NAME app_backend_capability_tests
	COMMAND app_backend_capability_tests)

add_executable(app_frame_publisher_tests
	frame_publisher_test.cpp)

target_include_directories(app_frame_publisher_tests
	PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/.."
		"${CMAKE_CURRENT_SOURCE_DIR}/../.."
		"${CMAKE_CURRENT_SOURCE_DIR}/../../src")
target_link_libraries(app_frame_publisher_tests
	PRIVATE
		app_runtime
		spdlog::spdlog
		fmt::fmt)

add_test(
	NAME app_frame_publisher_tests
	COMMAND app_frame_publisher_tests)

add_executable(app_playlist_controller_tests
	playlist_controller_test.cpp)

target_include_directories(app_playlist_controller_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(app_playlist_controller_tests
	PRIVATE
		app_video_backends
		app_config
		spdlog::spdlog
		fmt::fmt)

add_test(
	NAME app_playlist_controller_tests
	COMMAND app_playlist_controller_tests)

add_executable(app_zed_depth_policy_tests
	zed_depth_policy_test.cpp)

target_link_libraries(app_zed_depth_policy_tests
	PRIVATE
		app_zed_depth_policy)

add_test(
	NAME app_zed_depth_policy_tests
	COMMAND app_zed_depth_policy_tests)
