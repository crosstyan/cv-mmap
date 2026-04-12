#pragma once

#include <string>
#include <string_view>

namespace cvmmap::nats {

inline std::string make_prefix(std::string_view target_key) {
	return std::string("cvmmap.") + std::string(target_key);
}

inline std::string subject_producer_prefix(std::string_view target_key) {
	return make_prefix(target_key) + ".producer";
}

// Source control request-reply subjects
inline std::string subject_producer_source_reset(std::string_view target_key) {
	return subject_producer_prefix(target_key) + ".source.reset";
}

inline std::string subject_producer_source_info(std::string_view target_key) {
	return subject_producer_prefix(target_key) + ".source.info";
}

inline std::string subject_producer_source_seek(std::string_view target_key) {
	return subject_producer_prefix(target_key) + ".source.seek";
}

inline std::string subject_producer_source_capabilities(std::string_view target_key) {
	return subject_producer_prefix(target_key) + ".source.capabilities";
}

inline std::string subject_producer_source_playlist_apply(std::string_view target_key) {
	return subject_producer_prefix(target_key) + ".source.playlist.apply";
}

inline std::string subject_producer_source_playlist_info(std::string_view target_key) {
	return subject_producer_prefix(target_key) + ".source.playlist.info";
}

// SVO recorder request-reply subjects
inline std::string subject_producer_svo_recorder_capabilities(std::string_view target_key) {
	return subject_producer_prefix(target_key) + ".recorder.svo.capabilities";
}

inline std::string subject_producer_svo_recorder_start(std::string_view target_key) {
	return subject_producer_prefix(target_key) + ".recorder.svo.start";
}

inline std::string subject_producer_svo_recorder_stop(std::string_view target_key) {
	return subject_producer_prefix(target_key) + ".recorder.svo.stop";
}

inline std::string subject_producer_svo_recorder_status(std::string_view target_key) {
	return subject_producer_prefix(target_key) + ".recorder.svo.status";
}

// Pub/sub subjects
inline std::string subject_body(std::string_view target_key) {
	return make_prefix(target_key) + ".body";
}

inline std::string subject_status(std::string_view target_key) {
	return make_prefix(target_key) + ".status";
}

inline std::string subject_producer_wildcard(std::string_view target_key) {
	return subject_producer_prefix(target_key) + ".>";
}

} // namespace cvmmap::nats
