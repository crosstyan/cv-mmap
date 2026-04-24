#include "app_config.hpp"
#include "app_config_internal.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>
#include <toml++/toml.hpp>

namespace app {
using namespace config_detail;
using invalid_argument = std::invalid_argument;

std::string_view to_string(cvmmap::TimestampDomain domain) {
	switch (domain) {
	case cvmmap::TimestampDomain::UnixEpochNs:
		return "unix_epoch_ns";
	case cvmmap::TimestampDomain::MediaTimeNs:
		return "media_time_ns";
	default:
		return "unknown";
	}
}

cvmmap::TimestampDomain timestamp_domain_from_string(std::string_view s) {
	if (s == "unix_epoch_ns" || s == "unix" || s == "UnixEpochNs") {
		return cvmmap::TimestampDomain::UnixEpochNs;
	}
	if (s == "media_time_ns" || s == "media" || s == "MediaTimeNs") {
		return cvmmap::TimestampDomain::MediaTimeNs;
	}
	throw invalid_argument("unknown timestamp_domain: " + std::string(s));
}

std::string_view to_string(BackendType backend) {
	switch (backend) {
	case BackendType::Dummy:
		return "dummy";
	case BackendType::OpenCV:
		return "opencv";
	case BackendType::GStreamer:
		return "gstreamer";
	case BackendType::UdpRtp:
		return "udp_rtp";
	case BackendType::MCAP:
		return "mcap";
	case BackendType::ZED:
		return "zed";
	default:
		return "unknown";
	}
}

BackendType backend_from_string(std::string_view s) {
	if (s == "dummy" || s == "Dummy") {
		return BackendType::Dummy;
	} else if (s == "opencv" || s == "OpenCV") {
		return BackendType::OpenCV;
	} else if (s == "gstreamer" || s == "GStreamer" || s == "gst") {
		return BackendType::GStreamer;
	} else if (s == "udp_rtp" || s == "UdpRtp" || s == "udprtp") {
		return BackendType::UdpRtp;
	} else if (s == "mcap" || s == "MCAP") {
		return BackendType::MCAP;
	} else if (s == "zed" || s == "ZED") {
		return BackendType::ZED;
	}
	throw invalid_argument("unknown backend type: " + std::string(s));
}

std::string_view to_string(FiniteStreamEndingBehavior behavior) {
	switch (behavior) {
	case FiniteStreamEndingBehavior::Stop:
		return "stop";
	case FiniteStreamEndingBehavior::Loop:
		return "loop";
	case FiniteStreamEndingBehavior::LoopSilent:
		return "loop_silent";
	default:
		return "unknown";
	}
}

FiniteStreamEndingBehavior finite_stream_ending_behavior_from_string(std::string_view s) {
	auto normalized = normalize_ascii_lower(trim_ascii_spaces(std::string(s)));
	std::replace(normalized.begin(), normalized.end(), '-', '_');
	std::replace(normalized.begin(), normalized.end(), ' ', '_');
	if (normalized == "stop") {
		return FiniteStreamEndingBehavior::Stop;
	} else if (normalized == "loop") {
		return FiniteStreamEndingBehavior::Loop;
	} else if (normalized == "loop_silent") {
		return FiniteStreamEndingBehavior::LoopSilent;
	}
	throw invalid_argument("unknown finite_stream_ending_behavior: " + std::string(s));
}

std::string_view to_string(UndistortModel model) {
	switch (model) {
	case UndistortModel::Pinhole:
		return "pinhole";
	default:
		return "unknown";
	}
}

UndistortModel undistort_model_from_string(std::string_view s) {
	if (s == "pinhole" || s == "Pinhole") {
		return UndistortModel::Pinhole;
	}
	throw invalid_argument("unknown undistort model: " + std::string(s));
}

Config Config::Default() {
	return {
		.name  = "default",
		.video = VideoConfig{
			.backend                       = BackendType::Dummy,
			.finite_stream_ending_behavior = FiniteStreamEndingBehavior::Stop,
		},
		.opencv = std::nullopt,
		.gstreamer = std::nullopt,
		.udp_rtp = std::nullopt,
		.dummy = DummyConfig{
			.width            = 1280,
			.height           = 720,
			.fps              = 30,
			.frames           = 0,
			.startup_delay_ms = 0,
		},
		.mcap = std::nullopt,
		.preprocess = std::nullopt,
	};
}

Config Config::from_toml(const std::filesystem::path &path) {
	auto tbl = load_effective_config_table(path);


	Config config{};

	// name (required)
	if (auto val = tbl["name"].value<std::string>(); val) {
		config.name = *val;
	} else {
		throw invalid_argument("name is required");
	}

	if (auto ipc = tbl["ipc"].as_table(); ipc) {
		if (auto val = (*ipc)["namespace"].value<std::string>(); val) {
			config.ipc.name_space = trim_ascii_spaces(*val);
		} else {
			config.ipc.name_space = "cvmmap";
		}

		if (auto val = (*ipc)["prefix"].value<std::string>(); val) {
			config.ipc.prefix = validate_ipc_prefix(trim_ascii_spaces(*val));
		} else {
			config.ipc.prefix = "/tmp";
		}
	} else {
		config.ipc.name_space = "cvmmap";
		config.ipc.prefix     = "/tmp";
	}

	// [video] section
	if (auto video = tbl["video"].as_table(); video) {
		// backend
		if (auto val = (*video)["backend"].value<std::string>(); val) {
			config.video.backend = backend_from_string(*val);
		} else {
			config.video.backend = BackendType::Dummy; // default
		}

		if ((*video)["use_finite_as_infinite_stream"]) {
			throw invalid_argument(
				"video.use_finite_as_infinite_stream was removed; "
				"use video.finite_stream_ending_behavior = \"loop_silent\"");
		}

		// finite_stream_ending_behavior
		if (auto val = (*video)["finite_stream_ending_behavior"].value<std::string>(); val) {
			config.video.finite_stream_ending_behavior = finite_stream_ending_behavior_from_string(*val);
		} else {
			config.video.finite_stream_ending_behavior = FiniteStreamEndingBehavior::Stop;
		}
	}

	// [opencv] section
	if (auto opencv = tbl["opencv"].as_table(); opencv) {
		OpenCVConfig opencv_cfg{};

		// parameter: string or int
		if (auto node = (*opencv)["parameter"]; node) {
			if (auto s = node.value<std::string>(); s) {
				opencv_cfg.parameter = *s;
			} else if (auto i = node.value<int>(); i) {
				opencv_cfg.parameter = *i;
			} else {
				throw invalid_argument("opencv.parameter must be string or integer");
			}
		} else {
			throw invalid_argument("opencv.parameter is required when [opencv] section exists");
		}

		// api_preference (optional)
		if (auto val = (*opencv)["api"].value<std::string>(); val) {
			opencv_cfg.api_preference = from_string(*val);
		} else {
			opencv_cfg.api_preference = CAP_ANY;
		}

		config.opencv = opencv_cfg;
	}

	// [gstreamer] section
	if (auto gst = tbl["gstreamer"].as_table(); gst) {
		GStreamerConfig gst_cfg{};

		// pipeline (required)
		if (auto val = (*gst)["pipeline"].value<std::string>(); val) {
			gst_cfg.pipeline = normalize_pipeline_string(*val);
		} else {
			throw invalid_argument("gstreamer.pipeline is required when [gstreamer] section exists");
		}

		config.gstreamer = gst_cfg;
	}

	if (auto udp_rtp = tbl["udp_rtp"].as_table(); udp_rtp) {
		UdpRtpConfig udp_rtp_cfg{};
		const auto address = (*udp_rtp)["address"].value<std::string>();
		const auto multicast_group = (*udp_rtp)["multicast_group"].value<std::string>();
		if (address && multicast_group) {
			throw invalid_argument("udp_rtp.address and udp_rtp.multicast_group must not both be set");
		}
		if (address) {
			udp_rtp_cfg.address = trim_ascii_spaces(*address);
			if (udp_rtp_cfg.address.empty()) {
				throw invalid_argument("udp_rtp.address must not be empty when set");
			}
		} else if (multicast_group) {
			udp_rtp_cfg.address = trim_ascii_spaces(*multicast_group);
			if (udp_rtp_cfg.address.empty()) {
				throw invalid_argument("udp_rtp.multicast_group must not be empty when set");
			}
		}
		if (auto val = (*udp_rtp)["port"].value<int64_t>(); val) {
			if (*val <= 0 || *val > std::numeric_limits<uint16_t>::max()) {
				throw invalid_argument("udp_rtp.port must be in range [1, 65535]");
			}
			udp_rtp_cfg.port = static_cast<uint16_t>(*val);
		}
		if (auto val = (*udp_rtp)["payload_type"].value<int64_t>(); val) {
			if (*val < 0 || *val > std::numeric_limits<uint8_t>::max()) {
				throw invalid_argument("udp_rtp.payload_type must be in range [0, 255]");
			}
			udp_rtp_cfg.payload_type = static_cast<uint8_t>(*val);
		}
		if (auto val = (*udp_rtp)["auto_multicast"].value<bool>(); val) {
			udp_rtp_cfg.auto_multicast = *val;
		}
		if (auto val = (*udp_rtp)["codec"].value<std::string>(); val) {
			udp_rtp_cfg.codec = normalize_ascii_lower(trim_ascii_spaces(*val));
		}
		if (udp_rtp_cfg.codec != "h264" && udp_rtp_cfg.codec != "h265") {
			throw invalid_argument("udp_rtp.codec must be one of: h264, h265");
		}
		if (auto val = (*udp_rtp)["decoder"].value<std::string>(); val) {
			udp_rtp_cfg.decoder = normalize_ascii_lower(trim_ascii_spaces(*val));
		}
		if (udp_rtp_cfg.codec == "h264") {
			if (udp_rtp_cfg.decoder != "auto" &&
				udp_rtp_cfg.decoder != "nvh264dec" &&
				udp_rtp_cfg.decoder != "avdec_h264") {
				throw invalid_argument("udp_rtp.decoder must be one of: auto, nvh264dec, avdec_h264 when udp_rtp.codec=h264");
			}
		} else {
			if (udp_rtp_cfg.decoder != "auto" &&
				udp_rtp_cfg.decoder != "nvh265dec" &&
				udp_rtp_cfg.decoder != "avdec_h265") {
				throw invalid_argument("udp_rtp.decoder must be one of: auto, nvh265dec, avdec_h265 when udp_rtp.codec=h265");
			}
		}
		config.udp_rtp = udp_rtp_cfg;
	}

		if (auto dummy = tbl["dummy"].as_table(); dummy) {
			DummyConfig dummy_cfg{};

			dummy_cfg.width = (*dummy)["width"].value_or(dummy_cfg.width);
			dummy_cfg.height = (*dummy)["height"].value_or(dummy_cfg.height);
			dummy_cfg.fps = (*dummy)["fps"].value_or(dummy_cfg.fps);
			dummy_cfg.frames = (*dummy)["frames"].value_or(dummy_cfg.frames);
			dummy_cfg.startup_delay_ms = (*dummy)["startup_delay_ms"].value_or(dummy_cfg.startup_delay_ms);
			if (auto val = (*dummy)["timestamp_overlay_font_path"].value<std::string>(); val) {
				auto trimmed = trim_ascii_spaces(*val);
				if (trimmed.empty()) {
					throw invalid_argument("dummy.timestamp_overlay_font_path must not be empty");
				}
				dummy_cfg.timestamp_overlay_font_path = std::move(trimmed);
			}

			if (dummy_cfg.width <= 0) {
				throw invalid_argument("dummy.width must be positive");
			}
			if (dummy_cfg.height <= 0) {
			throw invalid_argument("dummy.height must be positive");
		}
		if (dummy_cfg.fps <= 0) {
			throw invalid_argument("dummy.fps must be positive");
		}
		if (dummy_cfg.startup_delay_ms < 0) {
			throw invalid_argument("dummy.startup_delay_ms must be non-negative");
		}

		const auto buffer_size = static_cast<uint64_t>(dummy_cfg.width) *
								 static_cast<uint64_t>(dummy_cfg.height) * 3ull;
		if (buffer_size == 0 || buffer_size > std::numeric_limits<uint32_t>::max()) {
			throw invalid_argument("dummy frame buffer size exceeds ABI limits");
		}

		config.dummy = dummy_cfg;
	}

	if (auto mcap = tbl["mcap"].as_table(); mcap) {
		McapConfig mcap_cfg{};

		if (auto val = (*mcap)["path"].value<std::string>(); val) {
			mcap_cfg.path = trim_ascii_spaces(*val);
		}
		if ((*mcap)["playlist"]) {
			auto playlist = (*mcap)["playlist"].as_table();
			if (!playlist) {
				throw invalid_argument("mcap.playlist must be a table");
			}
			FilePlaylistConfig playlist_cfg{};
			if (auto paths = (*playlist)["paths"]; paths) {
				playlist_cfg.paths = parse_playlist_paths(paths, "mcap.playlist.paths");
			} else {
				throw invalid_argument("mcap.playlist.paths is required when [mcap.playlist] exists");
			}
			playlist_cfg.sort_by_recording_time = parse_bool_field(
				(*playlist)["sort_by_recording_time"],
				"mcap.playlist.sort_by_recording_time",
				false);
			mcap_cfg.playlist = std::move(playlist_cfg);
		}

		if (auto val = (*mcap)["video_topic"].value<std::string>(); val) {
			mcap_cfg.video_topic = trim_ascii_spaces(*val);
		}
		if (auto val = (*mcap)["depth_topic"].value<std::string>(); val) {
			mcap_cfg.depth_topic = trim_ascii_spaces(*val);
		}
		if (auto val = (*mcap)["body_topic"].value<std::string>(); val) {
			mcap_cfg.body_topic = trim_ascii_spaces(*val);
		}
		if (auto val = (*mcap)["timestamp_domain"].value<std::string>(); val) {
			mcap_cfg.timestamp_domain = timestamp_domain_from_string(trim_ascii_spaces(*val));
		}

		const auto has_path = !mcap_cfg.path.empty();
		const auto has_playlist = mcap_cfg.playlist.has_value();
		if (has_path == has_playlist) {
			throw invalid_argument("exactly one of mcap.path or mcap.playlist.paths must be configured");
		}

		config.mcap = std::move(mcap_cfg);
	}

	if (auto preprocess = tbl["preprocess"].as_table(); preprocess) {
		PreprocessConfig preprocess_cfg{};
		if (auto undistort = (*preprocess)["undistort"].as_table(); undistort) {
			UndistortConfig undistort_cfg{};
			undistort_cfg.enabled = (*undistort)["enabled"].value_or(false);
			if (auto model = (*undistort)["model"].value<std::string>(); model) {
				undistort_cfg.model = undistort_model_from_string(*model);
			}
			if (auto camera_matrix = (*undistort)["camera_matrix"]; camera_matrix) {
				undistort_cfg.camera_matrix = parse_camera_matrix(camera_matrix);
			}
			if (auto dist_coeffs = (*undistort)["dist_coeffs"]; dist_coeffs) {
				undistort_cfg.dist_coeffs = parse_dist_coeffs(dist_coeffs);
			}
			undistort_cfg.use_optimal_new_camera_matrix = (*undistort)["use_optimal_new_camera_matrix"].value_or(true);
			undistort_cfg.alpha                         = (*undistort)["alpha"].value_or(0.0);
			undistort_cfg.crop_to_valid_roi             = (*undistort)["crop_to_valid_roi"].value_or(false);
			undistort_cfg.strict_startup                = (*undistort)["strict_startup"].value_or(false);

			if (undistort_cfg.enabled) {
				try {
					validate_undistort_config(undistort_cfg);
				} catch (const std::exception &e) {
					if (undistort_cfg.strict_startup) {
						throw;
					}
					spdlog::warn("invalid undistort config; disabling pass: {}", e.what());
					undistort_cfg.enabled = false;
				}
			}
			preprocess_cfg.undistort = std::move(undistort_cfg);
		}
		config.preprocess = std::move(preprocess_cfg);
	}

	if (auto nats = tbl["nats"].as_table(); nats) {
		NatsConfig nats_cfg{};
		nats_cfg.enabled = (*nats)["enabled"].value_or(true);
		if (auto val = (*nats)["url"].value<std::string>(); val) {
			nats_cfg.url = trim_ascii_spaces(*val);
		}
		config.nats = std::move(nats_cfg);
	}

	if (auto zed = tbl["zed"].as_table(); zed) {
		ZedConfig zed_cfg{};

		if (auto val = (*zed)["stream_mode"].value<std::string>(); val) {
			zed_cfg.stream_mode = normalize_ascii_lower(trim_ascii_spaces(*val));
		} else {
			zed_cfg.stream_mode = "local";
		}

		if (!is_valid_zed_stream_mode(zed_cfg.stream_mode)) {
			throw invalid_argument("zed.stream_mode must be one of: local, usb, device, auto, network, ethernet, stream, svo");
		}

		if (auto val = (*zed)["serial"]; val) {
			if (auto serial = val.value<int>(); serial) {
				if (*serial < 0) {
					throw invalid_argument("zed.serial must be non-negative");
				}
				zed_cfg.serial = *serial;
			} else {
				throw invalid_argument("zed.serial must be integer");
			}
		}

		if (auto val = (*zed)["index"]; val) {
			if (auto index = val.value<int>(); index) {
				if (*index < 0) {
					throw invalid_argument("zed.index must be non-negative");
				}
				zed_cfg.index = *index;
			} else {
				throw invalid_argument("zed.index must be integer");
			}
		}

		if (auto val = (*zed)["ip_address"]; val) {
			if (auto ip = val.value<std::string>(); ip) {
				auto trimmed_ip = trim_ascii_spaces(*ip);
				if (trimmed_ip.empty()) {
					throw invalid_argument("zed.ip_address must not be empty when provided");
				}
				zed_cfg.ip_address = std::move(trimmed_ip);
			} else {
				throw invalid_argument("zed.ip_address must be string");
			}
		}

		if (auto val = (*zed)["port"]; val) {
			if (auto port = val.value<int>(); port) {
				if (*port <= 0 || *port > 65535) {
					throw invalid_argument("zed.port must be in range 1..65535");
				}
				zed_cfg.port = *port;
			} else {
				throw invalid_argument("zed.port must be integer");
			}
		}

		if (auto val = (*zed)["svo_path"]; val) {
			if (auto path = val.value<std::string>(); path) {
				auto trimmed_path = trim_ascii_spaces(*path);
				if (trimmed_path.empty()) {
					throw invalid_argument("zed.svo_path must not be empty when provided");
				}
				zed_cfg.svo_path = std::move(trimmed_path);
			} else {
				throw invalid_argument("zed.svo_path must be string");
			}
		}
		if ((*zed)["playlist"]) {
			auto playlist = (*zed)["playlist"].as_table();
			if (!playlist) {
				throw invalid_argument("zed.playlist must be a table");
			}
			FilePlaylistConfig playlist_cfg{};
			if (auto paths = (*playlist)["paths"]; paths) {
				playlist_cfg.paths = parse_playlist_paths(paths, "zed.playlist.paths");
			} else {
				throw invalid_argument("zed.playlist.paths is required when [zed.playlist] exists");
			}
			playlist_cfg.sort_by_recording_time = parse_bool_field(
				(*playlist)["sort_by_recording_time"],
				"zed.playlist.sort_by_recording_time",
				false);
			zed_cfg.playlist = std::move(playlist_cfg);
		}

		if (auto val = (*zed)["resolution"].value<std::string>(); val) {
			zed_cfg.resolution = validate_and_canonicalize_zed_resolution(*val);
		} else if (is_zed_svo_stream_mode(zed_cfg.stream_mode)) {
			zed_cfg.resolution = "AUTO";
		} else {
			throw invalid_argument("zed.resolution is required when [zed] section exists");
		}

		if (auto val = (*zed)["fps"].value<int>(); val) {
			zed_cfg.fps = *val;
		} else if (is_zed_svo_stream_mode(zed_cfg.stream_mode)) {
			zed_cfg.fps = 0;
		} else {
			throw invalid_argument("zed.fps is required when [zed] section exists and must be integer");
		}

		if (auto val = (*zed)["depth_mode"].value<std::string>(); val) {
			zed_cfg.depth_mode = validate_and_canonicalize_zed_depth_mode(*val);
		} else {
			throw invalid_argument("zed.depth_mode is required when [zed] section exists");
		}

		zed_cfg.publish_confidence = parse_bool_field(
			(*zed)["publish_confidence"],
			"zed.publish_confidence",
			true);
		zed_cfg.svo_real_time_mode = parse_bool_field(
			(*zed)["svo_real_time_mode"],
			"zed.svo_real_time_mode",
			true);

		if (auto val = (*zed)["depth_max_fps"]; val) {
			if (auto v = val.value<int>(); v) {
				zed_cfg.depth_max_fps = *v;
			} else {
				throw invalid_argument("zed.depth_max_fps must be integer");
			}
		}

		if (auto val = (*zed)["depth_stabilization"]; val) {
			if (auto v = val.value<int>(); v) {
				zed_cfg.depth_stabilization = *v;
			} else {
				throw invalid_argument("zed.depth_stabilization must be integer");
			}
		}

		if (auto val = (*zed)["open_timeout_ms"]; val) {
			if (auto v = val.value<int>(); v) {
				zed_cfg.open_timeout_ms = *v;
			} else {
				throw invalid_argument("zed.open_timeout_ms must be integer");
			}
		}

		if (auto val = (*zed)["max_consecutive_failures"]; val) {
			if (auto v = val.value<int>(); v) {
				zed_cfg.max_consecutive_failures = *v;
			} else {
				throw invalid_argument("zed.max_consecutive_failures must be integer");
			}
		}

		if (auto val = (*zed)["left_pixel_format"].value<std::string>(); val) {
			zed_cfg.left_pixel_format = normalize_ascii_lower(*val);
		} else {
			zed_cfg.left_pixel_format = "bgr8";
		}

		if (auto val = (*zed)["coordinate_system"].value<std::string>(); val) {
			zed_cfg.coordinate_system = canonicalize_zed_coordinate_system(*val);
		} else {
			zed_cfg.coordinate_system = "IMAGE";
		}

		if (auto recording = (*zed)["recording"].as_table(); recording) {
			if (auto val = (*recording)["compression_mode"].value<std::string>(); val) {
				zed_cfg.recording.compression_mode =
					canonicalize_zed_recording_compression_mode(*val);
			}

			if (auto val = (*recording)["bitrate"]; val) {
				if (auto bitrate = val.value<int64_t>(); bitrate) {
					if (*bitrate < 0 || *bitrate > std::numeric_limits<unsigned int>::max()) {
						throw invalid_argument("zed.recording.bitrate must be in range 0..4294967295");
					}
					zed_cfg.recording.bitrate = static_cast<unsigned int>(*bitrate);
				} else {
					throw invalid_argument("zed.recording.bitrate must be integer");
				}
			}

			if (auto val = (*recording)["target_framerate"]; val) {
				if (auto target_framerate = val.value<int64_t>(); target_framerate) {
					if (*target_framerate < 0 || *target_framerate > std::numeric_limits<unsigned int>::max()) {
						throw invalid_argument("zed.recording.target_framerate must be in range 0..4294967295");
					}
					zed_cfg.recording.target_framerate = static_cast<unsigned int>(*target_framerate);
				} else {
					throw invalid_argument("zed.recording.target_framerate must be integer");
				}
			}

			if (auto val = (*recording)["transcode_streaming_input"]; val) {
				if (auto transcode = val.value<bool>(); transcode) {
					zed_cfg.recording.transcode_streaming_input = *transcode;
				} else {
					throw invalid_argument("zed.recording.transcode_streaming_input must be boolean");
				}
			}
		}

		if (auto body_tracking = (*zed)["body_tracking"].as_table(); body_tracking) {
			ZedConfig::BodyTrackingConfig body_tracking_cfg{};
			body_tracking_cfg.enabled = (*body_tracking)["enabled"].value_or(false);

			if (auto val = (*body_tracking)["detection_model"].value<std::string>(); val) {
				body_tracking_cfg.detection_model = canonicalize_zed_body_tracking_model(*val);
			}
			if (auto val = (*body_tracking)["body_format"].value<std::string>(); val) {
				body_tracking_cfg.body_format = canonicalize_zed_body_format(*val);
			}
			if (auto val = (*body_tracking)["body_selection"].value<std::string>(); val) {
				body_tracking_cfg.body_selection = canonicalize_zed_body_selection(*val);
			}
			if (auto val = (*body_tracking)["reference_frame"].value<std::string>(); val) {
				body_tracking_cfg.reference_frame = canonicalize_zed_body_reference_frame(*val);
			}

			body_tracking_cfg.set_floor_as_origin =
				(*body_tracking)["set_floor_as_origin"].value_or(
					body_tracking_cfg.set_floor_as_origin);
			body_tracking_cfg.enable_body_fitting =
				(*body_tracking)["enable_body_fitting"].value_or(body_tracking_cfg.enable_body_fitting);
			body_tracking_cfg.allow_reduced_precision_inference =
				(*body_tracking)["allow_reduced_precision_inference"].value_or(
					body_tracking_cfg.allow_reduced_precision_inference);
			body_tracking_cfg.max_range =
				(*body_tracking)["max_range"].value_or(body_tracking_cfg.max_range);
			body_tracking_cfg.prediction_timeout_s =
				(*body_tracking)["prediction_timeout_s"].value_or(
					body_tracking_cfg.prediction_timeout_s);
			body_tracking_cfg.detection_confidence_threshold =
				(*body_tracking)["detection_confidence_threshold"].value_or(
					body_tracking_cfg.detection_confidence_threshold);
			body_tracking_cfg.minimum_keypoints_threshold =
				(*body_tracking)["minimum_keypoints_threshold"].value_or(
					body_tracking_cfg.minimum_keypoints_threshold);
			body_tracking_cfg.skeleton_smoothing =
				(*body_tracking)["skeleton_smoothing"].value_or(
					body_tracking_cfg.skeleton_smoothing);

			validate_zed_body_tracking_config(body_tracking_cfg);
			zed_cfg.body_tracking = std::move(body_tracking_cfg);
		}

		validate_zed_runtime_config(zed_cfg);
		validate_zed_recording_config(zed_cfg.recording);

		if (zed_cfg.serial && zed_cfg.index) {
			throw invalid_argument("zed.serial and zed.index are mutually exclusive");
		}

		if (is_zed_svo_stream_mode(zed_cfg.stream_mode)) {
			const auto has_svo_path = zed_cfg.svo_path.has_value();
			const auto has_playlist = zed_cfg.playlist.has_value();
			if (has_svo_path == has_playlist) {
				throw invalid_argument("exactly one of zed.svo_path or zed.playlist.paths must be configured when zed.stream_mode is svo");
			}
			if (zed_cfg.serial || zed_cfg.index) {
				throw invalid_argument("zed.serial and zed.index must not be set when zed.stream_mode is svo");
			}
			if (zed_cfg.ip_address) {
				throw invalid_argument("zed.ip_address must not be set when zed.stream_mode is svo");
			}
			if (zed_cfg.port) {
				throw invalid_argument("zed.port must not be set when zed.stream_mode is svo");
			}
		} else if (is_zed_network_stream_mode(zed_cfg.stream_mode)) {
			if (zed_cfg.playlist) {
				throw invalid_argument("zed.playlist is only valid when zed.stream_mode is svo");
			}
			if (!zed_cfg.ip_address || zed_cfg.ip_address->empty()) {
				throw invalid_argument("zed.ip_address is required when zed.stream_mode is network/ethernet/stream");
			}
			if (zed_cfg.serial || zed_cfg.index) {
				throw invalid_argument("zed.serial and zed.index must not be set when zed.stream_mode is network/ethernet/stream");
			}
			if (zed_cfg.svo_path) {
				throw invalid_argument("zed.svo_path is only valid when zed.stream_mode is svo");
			}
		} else {
			if (zed_cfg.playlist) {
				throw invalid_argument("zed.playlist is only valid when zed.stream_mode is svo");
			}
			if (zed_cfg.ip_address) {
				throw invalid_argument("zed.ip_address is only valid when zed.stream_mode is network/ethernet/stream");
			}
			if (zed_cfg.port) {
				throw invalid_argument("zed.port is only valid when zed.stream_mode is network/ethernet/stream");
			}
			if (zed_cfg.svo_path) {
				throw invalid_argument("zed.svo_path is only valid when zed.stream_mode is svo");
			}
		}

		zed_cfg.stream_mode = canonical_zed_stream_mode(zed_cfg.stream_mode);

		config.zed = zed_cfg;
	}

	// Validate: ensure the selected backend has its config
	if (config.video.backend == BackendType::OpenCV && !config.opencv) {
		throw invalid_argument("[opencv] section is required when backend is 'opencv'");
	}
	if (config.video.backend == BackendType::Dummy && !config.dummy) {
		throw invalid_argument("[dummy] section is required when backend is 'dummy'");
	}
	if (config.video.backend == BackendType::GStreamer && !config.gstreamer) {
		throw invalid_argument("[gstreamer] section is required when backend is 'gstreamer'");
	}
	if (config.video.backend == BackendType::UdpRtp && !config.udp_rtp) {
		throw invalid_argument("[udp_rtp] section is required when backend is 'udp_rtp'");
	}
	if (config.video.backend == BackendType::MCAP && !config.mcap) {
		throw invalid_argument("[mcap] section is required when backend is 'mcap'");
	}
	if (config.video.backend == BackendType::ZED && !config.zed) {
		throw invalid_argument("[zed] section is required when backend is 'zed'");
	}

	validate_ipc_config(config);

	return config;
}

ActiveBackendSourceSnapshot Config::SnapshotActiveBackendSource() const {
	switch (video.backend) {
	case BackendType::MCAP:
		if (mcap) {
			return ActiveBackendSourceSnapshot{.path = mcap->path};
		}
		break;
	case BackendType::ZED:
		if (zed) {
			return ActiveBackendSourceSnapshot{.path = zed->svo_path};
		}
		break;
	default:
		break;
	}

	return {};
}

void Config::RestoreActiveBackendSource(const ActiveBackendSourceSnapshot &snapshot) {
	switch (video.backend) {
	case BackendType::MCAP:
		if (mcap) {
			mcap->path = snapshot.path.value_or("");
		}
		break;
	case BackendType::ZED:
		if (zed) {
			zed->svo_path = snapshot.path;
		}
		break;
	default:
		break;
	}
}
} // namespace app
