#include "app_config_internal.hpp"

#include <sstream>
#include <variant>

namespace app {

std::string Config::to_toml() const {
	std::ostringstream ss;
	ss << "name = \"" << name << "\"\n\n";
	ss << "[ipc]\n";
	ss << "namespace = \"" << ipc.name_space << "\"\n";
	ss << "prefix = \"" << ipc.prefix << "\"\n\n";

	ss << "[video]\n";
	ss << "backend = \"" << to_string(video.backend) << "\"\n";
	ss << "finite_stream_ending_behavior = \"" << to_string(video.finite_stream_ending_behavior) << "\"\n\n";

	if (opencv) {
		ss << "[opencv]\n";
		ss << "parameter = ";
		if (std::holds_alternative<std::string>(opencv->parameter)) {
			ss << "\"" << std::get<std::string>(opencv->parameter) << "\"";
		} else {
			ss << std::get<int>(opencv->parameter);
		}
		ss << "\n";
		ss << "api = \"" << to_string(opencv->api_preference) << "\"\n\n";
	}

	if (gstreamer) {
		ss << "[gstreamer]\n";
		ss << "pipeline = \"" << gstreamer->pipeline << "\"\n";
	}

	if (udp_rtp) {
		ss << "\n[udp_rtp]\n";
		ss << "multicast_group = \"" << udp_rtp->multicast_group << "\"\n";
		ss << "port = " << udp_rtp->port << "\n";
		ss << "payload_type = " << static_cast<unsigned>(udp_rtp->payload_type) << "\n";
		ss << "auto_multicast = " << (udp_rtp->auto_multicast ? "true" : "false") << "\n";
		ss << "codec = \"" << udp_rtp->codec << "\"\n";
		ss << "decoder = \"" << udp_rtp->decoder << "\"\n";
	}

	if (dummy) {
		ss << "[dummy]\n";
		ss << "width = " << dummy->width << "\n";
		ss << "height = " << dummy->height << "\n";
		ss << "fps = " << dummy->fps << "\n";
		ss << "frames = " << dummy->frames << "\n";
		ss << "startup_delay_ms = " << dummy->startup_delay_ms << "\n";
		if (dummy->timestamp_overlay_font_path) {
			ss << "timestamp_overlay_font_path = \"" << *dummy->timestamp_overlay_font_path << "\"\n";
		}
	}

	if (mcap) {
		ss << "\n[mcap]\n";
		if (!mcap->path.empty()) {
			ss << "path = \"" << mcap->path << "\"\n";
		}
		ss << "video_topic = \"" << mcap->video_topic << "\"\n";
		ss << "depth_topic = \"" << mcap->depth_topic << "\"\n";
		ss << "body_topic = \"" << mcap->body_topic << "\"\n";
		ss << "timestamp_domain = \"" << to_string(mcap->timestamp_domain) << "\"\n";
		if (mcap->playlist) {
			ss << "\n[mcap.playlist]\n";
			ss << "paths = [";
			for (size_t i = 0; i < mcap->playlist->paths.size(); ++i) {
				if (i != 0) {
					ss << ", ";
				}
				ss << "\"" << mcap->playlist->paths[i] << "\"";
			}
			ss << "]\n";
			ss << "sort_by_recording_time = " << (mcap->playlist->sort_by_recording_time ? "true" : "false") << "\n";
		}
	}

	if (preprocess && preprocess->undistort) {
		const auto &undistort = *preprocess->undistort;
		ss << "\n[preprocess.undistort]\n";
		ss << "enabled = " << (undistort.enabled ? "true" : "false") << "\n";
		ss << "model = \"" << to_string(undistort.model) << "\"\n";
		ss << "camera_matrix = [";
		for (size_t i = 0; i < undistort.camera_matrix.size(); ++i) {
			if (i != 0) {
				ss << ", ";
			}
			ss << undistort.camera_matrix[i];
		}
		ss << "]\n";
		ss << "dist_coeffs = [";
		for (size_t i = 0; i < undistort.dist_coeffs.size(); ++i) {
			if (i != 0) {
				ss << ", ";
			}
			ss << undistort.dist_coeffs[i];
		}
		ss << "]\n";
		ss << "use_optimal_new_camera_matrix = " << (undistort.use_optimal_new_camera_matrix ? "true" : "false") << "\n";
		ss << "alpha = " << undistort.alpha << "\n";
		ss << "crop_to_valid_roi = " << (undistort.crop_to_valid_roi ? "true" : "false") << "\n";
		ss << "strict_startup = " << (undistort.strict_startup ? "true" : "false") << "\n";
	}

	ss << "\n[nats]\n";
	ss << "enabled = " << (nats.enabled ? "true" : "false") << "\n";
	ss << "url = \"" << nats.url << "\"\n";

	if (zed) {
		ss << "\n[zed]\n";
		ss << "stream_mode = \"" << config_detail::canonical_zed_stream_mode(zed->stream_mode) << "\"\n";
		if (zed->svo_path) {
			ss << "svo_path = \"" << *zed->svo_path << "\"\n";
		}
		if (zed->ip_address) {
			ss << "ip_address = \"" << *zed->ip_address << "\"\n";
		}
		if (zed->port) {
			ss << "port = " << *zed->port << "\n";
		}
		if (zed->serial) {
			ss << "serial = " << *zed->serial << "\n";
		}
		if (zed->index) {
			ss << "index = " << *zed->index << "\n";
		}
		ss << "resolution = \"" << zed->resolution << "\"\n";
		ss << "fps = " << zed->fps << "\n";
		ss << "depth_mode = \"" << zed->depth_mode << "\"\n";
		ss << "publish_confidence = " << (zed->publish_confidence ? "true" : "false") << "\n";
		ss << "svo_real_time_mode = " << (zed->svo_real_time_mode ? "true" : "false") << "\n";
		ss << "depth_max_fps = " << zed->depth_max_fps << "\n";
		ss << "depth_stabilization = " << zed->depth_stabilization << "\n";
		ss << "open_timeout_ms = " << zed->open_timeout_ms << "\n";
		ss << "max_consecutive_failures = " << zed->max_consecutive_failures << "\n";
		ss << "left_pixel_format = \"" << zed->left_pixel_format << "\"\n";
		ss << "coordinate_system = \"" << zed->coordinate_system << "\"\n";
		if (zed->playlist) {
			ss << "\n[zed.playlist]\n";
			ss << "paths = [";
			for (size_t i = 0; i < zed->playlist->paths.size(); ++i) {
				if (i != 0) {
					ss << ", ";
				}
				ss << "\"" << zed->playlist->paths[i] << "\"";
			}
			ss << "]\n";
			ss << "sort_by_recording_time = " << (zed->playlist->sort_by_recording_time ? "true" : "false") << "\n";
		}
		ss << "\n[zed.recording]\n";
		ss << "compression_mode = \"" << zed->recording.compression_mode << "\"\n";
		ss << "bitrate = " << zed->recording.bitrate << "\n";
		ss << "target_framerate = " << zed->recording.target_framerate << "\n";
		ss << "transcode_streaming_input = "
		   << (zed->recording.transcode_streaming_input ? "true" : "false") << "\n";

		if (zed->body_tracking) {
			const auto &body_tracking = *zed->body_tracking;
			ss << "\n[zed.body_tracking]\n";
			ss << "enabled = " << (body_tracking.enabled ? "true" : "false") << "\n";
			ss << "detection_model = \"" << body_tracking.detection_model << "\"\n";
			ss << "body_format = \"" << body_tracking.body_format << "\"\n";
			ss << "body_selection = \"" << body_tracking.body_selection << "\"\n";
			ss << "reference_frame = \"" << body_tracking.reference_frame << "\"\n";
			ss << "set_floor_as_origin = " << (body_tracking.set_floor_as_origin ? "true" : "false") << "\n";
			ss << "enable_body_fitting = " << (body_tracking.enable_body_fitting ? "true" : "false") << "\n";
			ss << "allow_reduced_precision_inference = "
			   << (body_tracking.allow_reduced_precision_inference ? "true" : "false") << "\n";
			ss << "max_range = " << body_tracking.max_range << "\n";
			ss << "prediction_timeout_s = " << body_tracking.prediction_timeout_s << "\n";
			ss << "detection_confidence_threshold = " << body_tracking.detection_confidence_threshold << "\n";
			ss << "minimum_keypoints_threshold = " << body_tracking.minimum_keypoints_threshold << "\n";
			ss << "skeleton_smoothing = " << body_tracking.skeleton_smoothing << "\n";
		}
	}

	return ss.str();
}

} // namespace app
