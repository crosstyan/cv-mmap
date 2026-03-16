#pragma once

#include <string>
#include <string_view>

namespace cvmmap::nats {

inline std::string make_prefix(std::string_view target_key) {
	return std::string("cvmmap.") + std::string(target_key);
}

// Source control request-reply subjects
inline std::string subject_control_source_reset(std::string_view target_key) {
	return make_prefix(target_key) + ".control.source.reset";
}

inline std::string subject_control_source_info(std::string_view target_key) {
	return make_prefix(target_key) + ".control.source.info";
}

inline std::string subject_control_source_seek(std::string_view target_key) {
	return make_prefix(target_key) + ".control.source.seek";
}

inline std::string subject_control_source_capabilities(std::string_view target_key) {
	return make_prefix(target_key) + ".control.source.capabilities";
}

// Recorder request-reply subjects
inline std::string subject_control_recorder_svo_capabilities(std::string_view target_key) {
	return make_prefix(target_key) + ".control.recorder.svo.capabilities";
}

inline std::string subject_control_recorder_svo_start(std::string_view target_key) {
	return make_prefix(target_key) + ".control.recorder.svo.start";
}

inline std::string subject_control_recorder_svo_stop(std::string_view target_key) {
	return make_prefix(target_key) + ".control.recorder.svo.stop";
}

inline std::string subject_control_recorder_svo_status(std::string_view target_key) {
	return make_prefix(target_key) + ".control.recorder.svo.status";
}

inline std::string subject_control_recorder_mcap_capabilities(std::string_view target_key) {
	return make_prefix(target_key) + ".control.recorder.mcap.capabilities";
}

inline std::string subject_control_recorder_mcap_start(std::string_view target_key) {
	return make_prefix(target_key) + ".control.recorder.mcap.start";
}

inline std::string subject_control_recorder_mcap_stop(std::string_view target_key) {
	return make_prefix(target_key) + ".control.recorder.mcap.stop";
}

inline std::string subject_control_recorder_mcap_status(std::string_view target_key) {
	return make_prefix(target_key) + ".control.recorder.mcap.status";
}

// Pub/sub subjects
inline std::string subject_body(std::string_view target_key) {
	return make_prefix(target_key) + ".body";
}

inline std::string subject_status(std::string_view target_key) {
	return make_prefix(target_key) + ".status";
}

inline std::string subject_control_wildcard(std::string_view target_key) {
	return make_prefix(target_key) + ".control.>";
}

} // namespace cvmmap::nats
