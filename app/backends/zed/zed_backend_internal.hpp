#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sl/Camera.hpp>

#include "app_backends_facade.hpp"
#include "app_config.hpp"
#include "app_enum_models.hpp"
#include "zed_sdk_utils.hpp"

namespace app::backends {

struct ZedBackendOptions {
	app::ZedConfig zed_config;
	app::VideoConfig video_config;
};

struct ZedBackendImpl {
	struct CapturedFrame {
		frame_info_t info{};
		source_info_t source_info{};
		std::vector<uint8_t> payload{};
		std::optional<cvmmap::body_tracking_frame_t> body_tracking{};
		uint64_t timestamp_ns{0};
		bool depth_requested{false};
	};

	struct PublishedSnapshot {
		frame_metadata_t metadata{};
		source_info_t source_info{};
		std::string last_recording_error{};
		std::chrono::steady_clock::time_point last_publish_at{};
		bool has_last_publish_at{false};
	};

	struct PublishedFrame {
		frame_metadata_t metadata{};
		std::vector<uint8_t> payload{};
		std::optional<cvmmap::body_tracking_frame_t> body_tracking{};
	};

	struct DirectPublishedFrame {
		frame_metadata_t metadata{};
		direct_frame_fill_fn_t fill_payload{};
		std::optional<cvmmap::body_tracking_frame_t> body_tracking{};
	};

	struct EthernetExtensionStubs {
		void log_placeholder_status() const;
	};

	struct DirectOutputLayout {
		uint16_t width{0};
		uint16_t height{0};
		sl::MAT_TYPE left_mat_type{sl::MAT_TYPE::U8_C1};
		size_t left_row_bytes{0};
		size_t aux_row_bytes{0};
		size_t left_offset{0};
		size_t left_size{0};
		size_t depth_offset{0};
		size_t depth_size{0};
		size_t confidence_offset{0};
		size_t confidence_size{0};
	};

	struct DirectOutputBindingState {
		uint8_t *buffer_ptr{nullptr};
		size_t buffer_size{0};
		DirectOutputLayout layout{};
		sl::Mat left_view{};
		sl::Mat depth_view{};
		sl::Mat confidence_view{};
	};

	ZedBackendOptions options;
	sl::Camera camera;
	sl::InitParameters init_parameters{};
	sl::RuntimeParameters runtime_parameters{};
	sl::BodyTrackingParameters body_tracking_parameters{};
	sl::BodyTrackingRuntimeParameters body_tracking_runtime_parameters{};
	sl::Bodies bodies{};
	sl::PositionalTrackingParameters positional_tracking_parameters{};
	bool depth_enabled{false};
	bool body_tracking_enabled{false};
	sl::VIEW left_view{sl::VIEW::LEFT};
	sl::Mat left_frame;
	sl::Mat depth_frame;
	sl::Mat confidence_frame;
	std::vector<uint8_t> last_good_depth_plane;
	DirectOutputBindingState direct_output_binding{};
	const uint8_t *last_direct_depth_payload_ptr{nullptr};
	size_t last_direct_depth_payload_size{0};
	sl::REFERENCE_FRAME body_reference_frame{sl::REFERENCE_FRAME::CAMERA};
	std::string active_recording_path{};
	bool svo_mode{false};
	int total_svo_frames{0};
	double effective_source_fps{0.0};
	uint32_t depth_publish_period_frames{1};
	uint32_t frames_since_last_depth_request{0};
	bool force_depth_on_next_capture{true};
	sl::ERROR_CODE last_grab_error{sl::ERROR_CODE::SUCCESS};
	uint64_t timeline_start_ns{0};
	uint64_t timeline_end_ns{0};
	std::jthread worker_thread;
	std::atomic<bool> initialized{false};
	mutable std::mutex camera_mutex{};
	mutable std::mutex snapshot_mutex{};

	on_metadata_fn_t _on_metadata{nullptr};
	on_frame_fn_t _on_frame{nullptr};
	on_direct_frame_fn_t _on_direct_frame{nullptr};
	on_body_tracking_fn_t _on_body_tracking{nullptr};
	on_error_fn_t _on_error{nullptr};

	PublishedSnapshot published{};
	EthernetExtensionStubs eth_stubs{};

	ZedBackendImpl();
	explicit ZedBackendImpl(ZedBackendOptions opts);
	~ZedBackendImpl();

	void on_metadata(const frame_metadata_t &m);
	void on_frame(std::span<uint8_t> frame_buffer, const frame_metadata_t &m);
	void on_direct_frame(direct_frame_t frame);
	void on_error(error_t error_code, std::string_view message);
	void on_body_tracking(const cvmmap::body_tracking_frame_t &frame);

	void clear_recording_error();
	void set_recording_error(std::string message);

	void invalidate_direct_depth_tracking_locked();
	void reset_depth_fallback_locked();
	void reset_depth_publish_cadence_locked();
	void configure_depth_publish_cadence_locked();
	bool should_request_depth_for_grab_locked(
		bool force_depth_request = false) const;
	void commit_depth_request_for_grab_locked(bool depth_requested);
	void snapshot_last_direct_depth_plane_locked(
		std::span<const uint8_t> output_buffer);
	void OnDirectOutputBufferWillReset(std::span<const uint8_t> output_buffer);

	std::string GetLastRecordingError();

	error_t camera_control_availability_error_locked() const;
	static error_t map_camera_control_operation_error(
		sl::ERROR_CODE code,
		error_t invalid_parameters_error);
	bool can_read_camera_control_locked(
		const ZedCameraControlDescriptor &descriptor);
	cvmmap::expected<camera_control_state_t, error_t> get_camera_control_locked(
		const ZedCameraControlDescriptor &descriptor);

	[[nodiscard]] std::string format_svo_skip_message(
		std::string_view prefix) const;
	uint64_t effective_timestamp_ns_locked();
	uint32_t next_frame_count() const;
	std::chrono::milliseconds publish_gap_warning_threshold() const;
	bool uses_live_camera_recovery() const;
	std::optional<std::string> fatal_capture_error_message(
		sl::ERROR_CODE code) const;

	void stop_worker_thread();
	void start_worker_thread();

	source_info_t make_source_info_locked() const;
	bool initialize_svo_timeline_locked();

	recording_status_t make_recording_status_locked();
	void stop_recording_locked(std::string_view reason);

	bool open_camera_locked();

	std::optional<size_t> expected_row_bytes(const sl::Mat &frame) const;
	bool copy_compact_plane(
		const sl::Mat &src,
		size_t row_bytes,
		std::span<uint8_t> dst) const;
	void clear_direct_output_bindings_locked();
	std::optional<DirectOutputLayout> compute_direct_output_layout_locked(
		const frame_info_t &info_out,
		bool depth_requested) const;
	bool bind_direct_output_views_locked(
		std::span<uint8_t> payload,
		const frame_info_t &info_out,
		bool depth_requested);
	bool retrieve_direct_measure_plane_locked(
		sl::Mat &target,
		sl::MEASURE measure,
		std::span<uint8_t> plane_payload,
		const char *label);
	std::optional<cvmmap::body_tracking_frame_t> capture_body_tracking_frame_locked(
		uint32_t frame_count,
		uint64_t timestamp_ns);
	std::optional<size_t> pack_frame_locked(
		std::span<uint8_t> payload,
		frame_info_t &info_out,
		bool direct_output,
		bool depth_requested);
	cvmmap::expected<CapturedFrame, sl::ERROR_CODE> capture_frame_locked(
		uint32_t frame_count,
		bool force_depth_request = false);
	std::optional<frame_info_t> make_frame_info(const sl::Mat &frame);

	PublishedFrame publish_captured_frame(
		CapturedFrame captured,
		std::vector<uint8_t> payload,
		uint32_t frame_count,
		bool log_publish_gap);
	DirectPublishedFrame publish_captured_frame_direct(
		CapturedFrame captured,
		uint32_t frame_count,
		bool log_publish_gap);
	void emit_published_frame(PublishedFrame &published_frame);
	void emit_published_frame_direct(DirectPublishedFrame published_frame);

	void Init();
	void worker_loop(std::stop_token stop_token);
	void SetOnDirectFrame(on_direct_frame_fn_t on_direct_frame_);
	void Shutdown();
	void SetOnMetadata(on_metadata_fn_t on_metadata_);
	void SetOnFrame(on_frame_fn_t on_frame_);
	void SetOnBodyTracking(on_body_tracking_fn_t on_body_tracking_);
	void SetOnError(on_error_fn_t on_error_);
	source_info_t GetSourceInfo();
	error_t ResetFrameCount();

	camera_control_capabilities_t GetCameraControlCapabilities();
	cvmmap::expected<camera_control_state_t, error_t> GetCameraControl(
		cvmmap::CameraControlSetting setting);
	cvmmap::expected<camera_control_state_t, error_t> SetCameraControl(
		const camera_control_request_t &request);
	cvmmap::expected<camera_control_state_t, error_t> SetCameraControlRange(
		const camera_control_range_request_t &request);

	cvmmap::expected<recording_status_t, error_t> StartRecording(
		const svo_recording_request_t &request);
	cvmmap::expected<recording_status_t, error_t> StopRecording();
	cvmmap::expected<recording_status_t, error_t> GetRecordingStatus();
};

} // namespace app::backends
