#include <cvmmap/nats_service.hpp>
#include <cvmmap/nats_subjects.hpp>

#include <nats.h>
#include <spdlog/spdlog.h>

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <cvmmap/compat/format.hpp>
#include <string>
#include <vector>

#include <control.pb.h>

#include <cvmmap/ipc.hpp>

namespace cvmmap {

namespace pb = ::cvmmap::proto;

namespace {

pb::ErrorCode map_posix_error(const int error_code) {
	switch (error_code) {
	case 0:
		return pb::ERROR_CODE_OK;
	case -EOPNOTSUPP:
		return pb::ERROR_CODE_UNSUPPORTED;
	case -ERANGE:
	case -EINVAL:
		return pb::ERROR_CODE_OUT_OF_RANGE;
	default:
		return pb::ERROR_CODE_ERROR;
	}
}

pb::ErrorCode map_control_error_code(const int error_code) {
	switch (error_code) {
	case CONTROL_RESPONSE_OK:
		return pb::ERROR_CODE_OK;
	case CONTROL_RESPONSE_UNKNOWN_CMD:
	case CONTROL_RESPONSE_UNSUPPORTED:
	case -EOPNOTSUPP:
		return pb::ERROR_CODE_UNSUPPORTED;
	case CONTROL_RESPONSE_INVALID_PAYLOAD:
		return pb::ERROR_CODE_INVALID_PAYLOAD;
	case CONTROL_RESPONSE_OUT_OF_RANGE:
	case -ERANGE:
	case -EINVAL:
		return pb::ERROR_CODE_OUT_OF_RANGE;
	case CONTROL_RESPONSE_TIMEOUT:
		return pb::ERROR_CODE_TIMEOUT;
	default:
		return pb::ERROR_CODE_ERROR;
	}
}

pb::SourceKind to_proto_source_kind(const cvmmap::SourceKind source_kind) {
	switch (source_kind) {
	case cvmmap::SourceKind::Live:
		return pb::SOURCE_KIND_LIVE;
	case cvmmap::SourceKind::Finite:
		return pb::SOURCE_KIND_FINITE;
	default:
		return pb::SOURCE_KIND_UNKNOWN;
	}
}

pb::TimestampDomain to_proto_timestamp_domain(
	const cvmmap::TimestampDomain timestamp_domain) {
	switch (timestamp_domain) {
	case cvmmap::TimestampDomain::UnixEpochNs:
		return pb::TIMESTAMP_DOMAIN_UNIX_EPOCH_NS;
	case cvmmap::TimestampDomain::MediaTimeNs:
		return pb::TIMESTAMP_DOMAIN_MEDIA_TIME_NS;
	default:
		return pb::TIMESTAMP_DOMAIN_UNKNOWN;
	}
}

pb::RecordingFormat to_proto_recording_format(
	const cvmmap::RecordingFormat recording_format) {
	switch (recording_format) {
	case cvmmap::RecordingFormat::Svo:
		return pb::RECORDING_FORMAT_SVO;
	case cvmmap::RecordingFormat::Mcap:
		return pb::RECORDING_FORMAT_MCAP;
	default:
		return pb::RECORDING_FORMAT_UNKNOWN;
	}
}

pb::ModuleStatusCode to_proto_module_status(const int32_t status_code) {
	if (status_code == MODULE_STATUS_ONLINE) {
		return pb::MODULE_STATUS_CODE_ONLINE;
	}
	if (status_code == MODULE_STATUS_OFFLINE) {
		return pb::MODULE_STATUS_CODE_OFFLINE;
	}
	if (status_code == MODULE_STATUS_STREAM_RESET) {
		return pb::MODULE_STATUS_CODE_STREAM_RESET;
	}
	return pb::MODULE_STATUS_CODE_UNKNOWN;
}

void fill_recording_status_response(
	pb::RecordingStatusResponse &response,
	const RecordingStatus &status) {
	response.set_error(pb::ERROR_CODE_OK);
	response.set_format(to_proto_recording_format(status.format));
	response.set_can_record(status.can_record);
	response.set_is_recording(status.is_recording);
	response.set_is_paused(status.is_paused);
	response.set_last_frame_ok(status.last_frame_ok);
	response.set_frames_ingested(status.frames_ingested);
	response.set_frames_encoded(status.frames_encoded);
	response.set_active_path(status.active_path);
}

void fill_capabilities_response(
	pb::CapabilitiesResponse &response,
	const bool can_seek,
	const std::initializer_list<RecordingFormat> available_formats) {
	response.set_error(pb::ERROR_CODE_OK);
	response.set_can_seek(can_seek);
	for (const auto format : available_formats) {
		response.add_available_recording_formats(
			to_proto_recording_format(format));
	}
}

void fill_playlist_info(
	pb::PlaylistInfo &wire_info,
	const PlaylistInfo &info) {
	wire_info.set_has_playlist(info.has_playlist);
	wire_info.set_sort_by_recording_time(info.sort_by_recording_time);
	wire_info.set_current_index(info.current_index);
	wire_info.set_current_path(info.current_path);
	for (const auto &path : info.paths) {
		wire_info.add_paths(path);
	}
}

cvmmap::expected<PlaylistRequest, ControlError> parse_playlist_request(
	const pb::ApplyPlaylistRequest &request) {
	PlaylistRequest parsed{};
	parsed.sort_by_recording_time = request.sort_by_recording_time();
	parsed.paths.reserve(static_cast<size_t>(request.paths_size()));
	for (const auto &path : request.paths()) {
		if (path.empty()) {
			return cvmmap::unexpected(ControlError{
				.code = CONTROL_RESPONSE_INVALID_PAYLOAD,
				.message = "playlist paths must not be empty",
			});
		}
		parsed.paths.push_back(path);
	}
	if (parsed.paths.empty()) {
		return cvmmap::unexpected(ControlError{
			.code = CONTROL_RESPONSE_INVALID_PAYLOAD,
			.message = "playlist paths must not be empty",
		});
	}
	return parsed;
}

cvmmap::expected<RecordingRequest, ControlError> parse_recording_request(
	const pb::RecordingStartRequest &request,
	const RecordingFormat format) {
	if (request.output_path().empty()) {
		return cvmmap::unexpected(ControlError{
			.code = CONTROL_RESPONSE_INVALID_PAYLOAD,
			.message = "recording path is empty",
		});
	}

	RecordingRequest parsed{
		.format = format,
		.output_path = request.output_path(),
	};

	switch (format) {
	case RecordingFormat::Svo: {
		if (request.has_mcap_options()) {
			return cvmmap::unexpected(ControlError{
				.code = CONTROL_RESPONSE_INVALID_PAYLOAD,
				.message = "MCAP options are invalid for SVO recording",
			});
		}
		if (request.has_svo_options()) {
			SvoRecordingOptions options{};
			const auto &wire_options = request.svo_options();
			if (wire_options.has_compression_mode()) {
				options.compression_mode = wire_options.compression_mode();
			}
			if (wire_options.has_bitrate()) {
				options.bitrate = wire_options.bitrate();
			}
			if (wire_options.has_target_framerate()) {
				options.target_framerate = wire_options.target_framerate();
			}
			if (wire_options.has_transcode_streaming_input()) {
				options.transcode_streaming_input =
					wire_options.transcode_streaming_input();
			}
			parsed.svo_options = std::move(options);
		}
		break;
	}
	case RecordingFormat::Mcap: {
		if (request.has_svo_options()) {
			return cvmmap::unexpected(ControlError{
				.code = CONTROL_RESPONSE_INVALID_PAYLOAD,
				.message = "SVO options are invalid for MCAP recording",
			});
		}
		if (request.has_mcap_options()) {
			McapRecordingOptions options{};
			const auto &wire_options = request.mcap_options();
			if (wire_options.has_compression()) {
				options.compression = wire_options.compression();
			}
			if (wire_options.has_topic()) {
				options.topic = wire_options.topic();
			}
			if (wire_options.has_depth_topic()) {
				options.depth_topic = wire_options.depth_topic();
			}
			if (wire_options.has_body_topic()) {
				options.body_topic = wire_options.body_topic();
			}
			if (wire_options.has_frame_id()) {
				options.frame_id = wire_options.frame_id();
			}
			parsed.mcap_options = std::move(options);
		}
		break;
	}
	default:
		return cvmmap::unexpected(ControlError{
			.code = CONTROL_RESPONSE_INVALID_PAYLOAD,
			.message = "recording format is required",
		});
	}

	return parsed;
}

uint64_t now_ns() {
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::system_clock::now().time_since_epoch())
			.count());
}

} // namespace

struct NatsControlService::impl {
	std::string instance_name;
	std::string target_key;
	std::string nats_url;
	NatsControlHandlers handlers;
	bool started{false};
	natsConnection *conn{nullptr};
	natsSubscription *sub_source_reset{nullptr};
	natsSubscription *sub_source_info{nullptr};
	natsSubscription *sub_source_seek{nullptr};
	natsSubscription *sub_source_capabilities{nullptr};
	natsSubscription *sub_source_playlist_apply{nullptr};
	natsSubscription *sub_source_playlist_info{nullptr};
	natsSubscription *sub_svo_capabilities{nullptr};
	natsSubscription *sub_svo_start{nullptr};
	natsSubscription *sub_svo_stop{nullptr};
	natsSubscription *sub_svo_status{nullptr};
	natsSubscription *sub_mcap_capabilities{nullptr};
	natsSubscription *sub_mcap_start{nullptr};
	natsSubscription *sub_mcap_stop{nullptr};
	natsSubscription *sub_mcap_status{nullptr};

	void publish(const std::string &subject, const void *data, const int size) {
		if (!conn) {
			return;
		}
		const auto status =
			natsConnection_Publish(conn, subject.c_str(), data, size);
		if (status != NATS_OK) {
			spdlog::error(
				"nats publish to '{}': {}",
				subject,
				natsStatus_GetText(status));
		}
	}

	void publish_proto(
		const std::string &subject,
		const google::protobuf::MessageLite &message) {
		const auto size = message.ByteSizeLong();
		std::vector<uint8_t> bytes(size);
		message.SerializeToArray(bytes.data(), static_cast<int>(size));
		publish(subject, bytes.data(), static_cast<int>(bytes.size()));
	}

	template <typename Response>
	void reply(
		natsMsg *message,
		const Response &response) {
		const auto *reply_subject = natsMsg_GetReply(message);
		if (reply_subject && reply_subject[0]) {
			const auto size = response.ByteSizeLong();
			std::vector<uint8_t> bytes(size);
			response.SerializeToArray(bytes.data(), static_cast<int>(size));
			natsConnection_Publish(
				conn,
				reply_subject,
				bytes.data(),
				static_cast<int>(bytes.size()));
		}
		natsMsg_Destroy(message);
	}

	static void on_source_reset_msg(
		natsConnection *,
		natsSubscription *,
		natsMsg *message,
		void *closure) {
		auto *self = static_cast<impl *>(closure);
		pb::ResetFrameCountResponse response;
		if (self->handlers.on_reset_frame_count) {
			response.set_error(
				self->handlers.on_reset_frame_count() == 0 ?
					pb::ERROR_CODE_OK :
					pb::ERROR_CODE_ERROR);
		} else {
			response.set_error(pb::ERROR_CODE_UNSUPPORTED);
		}
		self->reply(message, response);
	}

	static void on_source_info_msg(
		natsConnection *,
		natsSubscription *,
		natsMsg *message,
		void *closure) {
		auto *self = static_cast<impl *>(closure);
		pb::GetSourceInfoResponse response;
		if (self->handlers.on_get_source_info) {
			const auto info = self->handlers.on_get_source_info();
			response.set_error(pb::ERROR_CODE_OK);
			response.set_source_kind(to_proto_source_kind(info.source_kind));
			response.set_timestamp_domain(
				to_proto_timestamp_domain(info.timestamp_domain));
			response.set_flags(info.flags);
			response.set_timeline_start_ns(info.timeline_start_ns);
			response.set_timeline_end_ns(info.timeline_end_ns);
			response.set_duration_ns(info.duration_ns);
			response.set_current_timestamp_ns(info.current_timestamp_ns);
			response.set_current_frame_count(info.current_frame_count);
		} else {
			response.set_error(pb::ERROR_CODE_UNSUPPORTED);
		}
		self->reply(message, response);
	}

	static void on_source_seek_msg(
		natsConnection *,
		natsSubscription *,
		natsMsg *message,
		void *closure) {
		auto *self = static_cast<impl *>(closure);
		pb::SeekTimestampResponse response;
		if (!self->handlers.on_seek_timestamp) {
			response.set_error(pb::ERROR_CODE_UNSUPPORTED);
			self->reply(message, response);
			return;
		}

		pb::SeekTimestampRequest request;
		if (!request.ParseFromArray(
				natsMsg_GetData(message),
				natsMsg_GetDataLength(message))) {
			response.set_error(pb::ERROR_CODE_INVALID_PAYLOAD);
			self->reply(message, response);
			return;
		}

		auto result = self->handlers.on_seek_timestamp(
			request.target_timestamp_ns());
		if (!result) {
			response.set_error(map_posix_error(result.error()));
			self->reply(message, response);
			return;
		}

		response.set_error(pb::ERROR_CODE_OK);
		response.set_requested_timestamp_ns(result->requested_timestamp_ns);
		response.set_landed_timestamp_ns(result->landed_timestamp_ns);
		response.set_landed_frame_count(result->landed_frame_count);
		response.set_exact_match(result->exact_match);
		self->reply(message, response);
	}

	static void on_source_playlist_apply_msg(
		natsConnection *,
		natsSubscription *,
		natsMsg *message,
		void *closure) {
		auto *self = static_cast<impl *>(closure);
		pb::ApplyPlaylistResponse response;
		if (!self->handlers.on_apply_playlist) {
			response.set_error(pb::ERROR_CODE_UNSUPPORTED);
			response.set_error_message("playlist apply is not supported by the active producer");
			self->reply(message, response);
			return;
		}

		pb::ApplyPlaylistRequest request;
		if (!request.ParseFromArray(
				natsMsg_GetData(message),
				natsMsg_GetDataLength(message))) {
			response.set_error(pb::ERROR_CODE_INVALID_PAYLOAD);
			response.set_error_message("invalid playlist apply payload");
			self->reply(message, response);
			return;
		}

		auto parsed_request = parse_playlist_request(request);
		if (!parsed_request) {
			response.set_error(map_control_error_code(parsed_request.error().code));
			response.set_error_message(parsed_request.error().message);
			self->reply(message, response);
			return;
		}

		auto result = self->handlers.on_apply_playlist(*parsed_request);
		if (!result) {
			response.set_error(map_control_error_code(result.error().code));
			response.set_error_message(result.error().message);
			self->reply(message, response);
			return;
		}

		response.set_error(pb::ERROR_CODE_OK);
		fill_playlist_info(*response.mutable_playlist_info(), *result);
		self->reply(message, response);
	}

	static void on_source_playlist_info_msg(
		natsConnection *,
		natsSubscription *,
		natsMsg *message,
		void *closure) {
		auto *self = static_cast<impl *>(closure);
		pb::GetPlaylistInfoResponse response;
		if (!self->handlers.on_get_playlist_info) {
			response.set_error(pb::ERROR_CODE_UNSUPPORTED);
			response.set_error_message("playlist query is not supported by the active producer");
			self->reply(message, response);
			return;
		}

		pb::GetPlaylistInfoRequest request;
		if (!request.ParseFromArray(
				natsMsg_GetData(message),
				natsMsg_GetDataLength(message))) {
			response.set_error(pb::ERROR_CODE_INVALID_PAYLOAD);
			response.set_error_message("invalid playlist info payload");
			self->reply(message, response);
			return;
		}

		auto result = self->handlers.on_get_playlist_info();
		if (!result) {
			response.set_error(map_control_error_code(result.error().code));
			response.set_error_message(result.error().message);
			self->reply(message, response);
			return;
		}

		response.set_error(pb::ERROR_CODE_OK);
		fill_playlist_info(*response.mutable_playlist_info(), *result);
		self->reply(message, response);
	}

static void on_source_capabilities_msg(
	natsConnection *,
	natsSubscription *,
	natsMsg *message,
	void *closure) {
	auto *self = static_cast<impl *>(closure);
	pb::CapabilitiesResponse response;
	if (self->handlers.on_get_source_info) {
		const auto info = self->handlers.on_get_source_info();
		fill_capabilities_response(
			response,
			(info.flags & SOURCE_INFO_FLAG_CAN_SEEK) != 0,
			{});
	} else {
		response.set_error(pb::ERROR_CODE_UNSUPPORTED);
	}
	self->reply(message, response);
	}

	static void on_recording_capabilities_msg(
		natsConnection *,
		natsSubscription *,
		natsMsg *message,
		void *closure,
		const RecordingFormat format) {
		auto *self = static_cast<impl *>(closure);
		pb::CapabilitiesResponse response;
		if (self->handlers.on_recording_available &&
			self->handlers.on_recording_available(format)) {
			fill_capabilities_response(response, false, {format});
		} else {
			fill_capabilities_response(response, false, {});
		}
		self->reply(message, response);
	}

	static void on_recording_start_msg(
		natsConnection *,
		natsSubscription *,
		natsMsg *message,
		void *closure,
		const RecordingFormat format) {
		auto *self = static_cast<impl *>(closure);
		pb::RecordingStatusResponse response;
		if (!self->handlers.on_start_recording) {
			response.set_error(pb::ERROR_CODE_UNSUPPORTED);
			self->reply(message, response);
			return;
		}

		pb::RecordingStartRequest request;
		if (!request.ParseFromArray(
				natsMsg_GetData(message),
				natsMsg_GetDataLength(message))) {
			response.set_error(pb::ERROR_CODE_INVALID_PAYLOAD);
			self->reply(message, response);
			return;
		}

		auto parsed_request = parse_recording_request(request, format);
		if (!parsed_request) {
			response.set_error(map_control_error_code(parsed_request.error().code));
			response.set_error_message(parsed_request.error().message);
			self->reply(message, response);
			return;
		}

		try {
			auto result = self->handlers.on_start_recording(*parsed_request);
			if (!result) {
				response.set_error(map_control_error_code(result.error().code));
				response.set_error_message(result.error().message);
				self->reply(message, response);
				return;
			}

			fill_recording_status_response(response, *result);
		} catch (const std::exception &e) {
			response.set_error(pb::ERROR_CODE_ERROR);
			response.set_error_message(
				cvmmap::format("unexpected recording start failure: {}", e.what()));
			self->reply(message, response);
			return;
		} catch (...) {
			response.set_error(pb::ERROR_CODE_ERROR);
			response.set_error_message("unexpected recording start failure");
			self->reply(message, response);
			return;
		}
		self->reply(message, response);
	}

	static void on_recording_stop_msg(
		natsConnection *,
		natsSubscription *,
		natsMsg *message,
		void *closure,
		const RecordingFormat format) {
		auto *self = static_cast<impl *>(closure);
		pb::RecordingStatusResponse response;
		if (!self->handlers.on_stop_recording) {
			response.set_error(pb::ERROR_CODE_UNSUPPORTED);
			self->reply(message, response);
			return;
		}

		pb::RecordingStopRequest request;
		if (!request.ParseFromArray(
				natsMsg_GetData(message),
				natsMsg_GetDataLength(message))) {
			response.set_error(pb::ERROR_CODE_INVALID_PAYLOAD);
			self->reply(message, response);
			return;
		}

		auto result = self->handlers.on_stop_recording(format);
		if (!result) {
			response.set_error(map_control_error_code(result.error().code));
			response.set_error_message(result.error().message);
			self->reply(message, response);
			return;
		}

		fill_recording_status_response(response, *result);
		self->reply(message, response);
	}

	static void on_recording_status_msg(
		natsConnection *,
		natsSubscription *,
		natsMsg *message,
		void *closure,
		const RecordingFormat format) {
		auto *self = static_cast<impl *>(closure);
		pb::RecordingStatusResponse response;
		if (!self->handlers.on_get_recording_status) {
			response.set_error(pb::ERROR_CODE_UNSUPPORTED);
			self->reply(message, response);
			return;
		}

		pb::RecordingStatusRequest request;
		if (!request.ParseFromArray(
				natsMsg_GetData(message),
				natsMsg_GetDataLength(message))) {
			response.set_error(pb::ERROR_CODE_INVALID_PAYLOAD);
			self->reply(message, response);
			return;
		}

		auto result = self->handlers.on_get_recording_status(format);
		if (!result) {
			response.set_error(map_control_error_code(result.error().code));
			response.set_error_message(result.error().message);
			self->reply(message, response);
			return;
		}

		fill_recording_status_response(response, *result);
		self->reply(message, response);
	}

	static void on_svo_capabilities_msg(
		natsConnection *conn,
		natsSubscription *sub,
		natsMsg *message,
		void *closure) {
		on_recording_capabilities_msg(
			conn,
			sub,
			message,
			closure,
			RecordingFormat::Svo);
	}

	static void on_mcap_capabilities_msg(
		natsConnection *conn,
		natsSubscription *sub,
		natsMsg *message,
		void *closure) {
		on_recording_capabilities_msg(
			conn,
			sub,
			message,
			closure,
			RecordingFormat::Mcap);
	}

	static void on_svo_start_msg(
		natsConnection *conn,
		natsSubscription *sub,
		natsMsg *message,
		void *closure) {
		on_recording_start_msg(
			conn,
			sub,
			message,
			closure,
			RecordingFormat::Svo);
	}

	static void on_mcap_start_msg(
		natsConnection *conn,
		natsSubscription *sub,
		natsMsg *message,
		void *closure) {
		on_recording_start_msg(
			conn,
			sub,
			message,
			closure,
			RecordingFormat::Mcap);
	}

	static void on_svo_stop_msg(
		natsConnection *conn,
		natsSubscription *sub,
		natsMsg *message,
		void *closure) {
		on_recording_stop_msg(
			conn,
			sub,
			message,
			closure,
			RecordingFormat::Svo);
	}

	static void on_mcap_stop_msg(
		natsConnection *conn,
		natsSubscription *sub,
		natsMsg *message,
		void *closure) {
		on_recording_stop_msg(
			conn,
			sub,
			message,
			closure,
			RecordingFormat::Mcap);
	}

	static void on_svo_status_msg(
		natsConnection *conn,
		natsSubscription *sub,
		natsMsg *message,
		void *closure) {
		on_recording_status_msg(
			conn,
			sub,
			message,
			closure,
			RecordingFormat::Svo);
	}

	static void on_mcap_status_msg(
		natsConnection *conn,
		natsSubscription *sub,
		natsMsg *message,
		void *closure) {
		on_recording_status_msg(
			conn,
			sub,
			message,
			closure,
			RecordingFormat::Mcap);
	}
};

NatsControlService::NatsControlService(
	std::string instance_name,
	std::string target_key,
	std::string nats_url)
	: pimpl_(std::make_unique<impl>()) {
	pimpl_->instance_name = std::move(instance_name);
	pimpl_->target_key = std::move(target_key);
	pimpl_->nats_url = std::move(nats_url);
}

NatsControlService::~NatsControlService() {
	Stop();
}

void NatsControlService::SetHandlers(NatsControlHandlers handlers) {
	assert(!pimpl_->started && "SetHandlers must be called before Start()");
	pimpl_->handlers = std::move(handlers);
}

bool NatsControlService::Start() {
	if (pimpl_->started) {
		return true;
	}

	natsOptions *options = nullptr;
	natsOptions_Create(&options);
	natsOptions_SetURL(options, pimpl_->nats_url.c_str());

	const auto status = natsConnection_Connect(&pimpl_->conn, options);
	natsOptions_Destroy(options);
	if (status != NATS_OK) {
		spdlog::error(
			"nats connect to '{}': {}",
			pimpl_->nats_url,
			natsStatus_GetText(status));
		return false;
	}

	pimpl_->started = true;
	const auto &target_key = pimpl_->target_key;

	if (pimpl_->handlers.on_reset_frame_count) {
		natsConnection_Subscribe(
			&pimpl_->sub_source_reset,
			pimpl_->conn,
			nats::subject_control_source_reset(target_key).c_str(),
			impl::on_source_reset_msg,
			pimpl_.get());
	}
	if (pimpl_->handlers.on_get_source_info) {
		natsConnection_Subscribe(
			&pimpl_->sub_source_info,
			pimpl_->conn,
			nats::subject_control_source_info(target_key).c_str(),
			impl::on_source_info_msg,
			pimpl_.get());
		natsConnection_Subscribe(
			&pimpl_->sub_source_capabilities,
			pimpl_->conn,
			nats::subject_control_source_capabilities(target_key).c_str(),
			impl::on_source_capabilities_msg,
			pimpl_.get());
	}
	if (pimpl_->handlers.on_seek_timestamp) {
		natsConnection_Subscribe(
			&pimpl_->sub_source_seek,
			pimpl_->conn,
			nats::subject_control_source_seek(target_key).c_str(),
			impl::on_source_seek_msg,
			pimpl_.get());
	}
	if (pimpl_->handlers.on_apply_playlist) {
		natsConnection_Subscribe(
			&pimpl_->sub_source_playlist_apply,
			pimpl_->conn,
			nats::subject_control_source_playlist_apply(target_key).c_str(),
			impl::on_source_playlist_apply_msg,
			pimpl_.get());
	}
	if (pimpl_->handlers.on_get_playlist_info) {
		natsConnection_Subscribe(
			&pimpl_->sub_source_playlist_info,
			pimpl_->conn,
			nats::subject_control_source_playlist_info(target_key).c_str(),
			impl::on_source_playlist_info_msg,
			pimpl_.get());
	}

	auto *service_impl = pimpl_.get();
	const auto subscribe_recorder_subjects =
		[service_impl, &target_key](RecordingFormat format,
								 natsSubscription **capability_sub,
								 natsMsgHandler capability_handler,
								 natsSubscription **start_sub,
								 natsMsgHandler start_handler,
								 natsSubscription **stop_sub,
								 natsMsgHandler stop_handler,
								 natsSubscription **status_sub,
								 natsMsgHandler status_handler) {
			auto capability_subject = std::string{};
			auto start_subject = std::string{};
			auto stop_subject = std::string{};
			auto status_subject = std::string{};
			switch (format) {
			case RecordingFormat::Svo:
				capability_subject = nats::subject_control_recorder_svo_capabilities(target_key);
				start_subject = nats::subject_control_recorder_svo_start(target_key);
				stop_subject = nats::subject_control_recorder_svo_stop(target_key);
				status_subject = nats::subject_control_recorder_svo_status(target_key);
				break;
			case RecordingFormat::Mcap:
				capability_subject = nats::subject_control_recorder_mcap_capabilities(target_key);
				start_subject = nats::subject_control_recorder_mcap_start(target_key);
				stop_subject = nats::subject_control_recorder_mcap_stop(target_key);
				status_subject = nats::subject_control_recorder_mcap_status(target_key);
				break;
			default:
				return;
			}

			const bool owns_format =
				service_impl->handlers.on_recording_available &&
				service_impl->handlers.on_recording_available(format);
			if (!owns_format) {
				return;
			}

			if (service_impl->handlers.on_recording_available) {
				natsConnection_Subscribe(
					capability_sub,
					service_impl->conn,
					capability_subject.c_str(),
					capability_handler,
					service_impl);
			}
			if (service_impl->handlers.on_start_recording) {
				natsConnection_Subscribe(
					start_sub,
					service_impl->conn,
					start_subject.c_str(),
					start_handler,
					service_impl);
			}
			if (service_impl->handlers.on_stop_recording) {
				natsConnection_Subscribe(
					stop_sub,
					service_impl->conn,
					stop_subject.c_str(),
					stop_handler,
					service_impl);
			}
			if (service_impl->handlers.on_get_recording_status) {
				natsConnection_Subscribe(
					status_sub,
					service_impl->conn,
					status_subject.c_str(),
					status_handler,
					service_impl);
			}
		};

	subscribe_recorder_subjects(
		RecordingFormat::Svo,
		&pimpl_->sub_svo_capabilities,
		impl::on_svo_capabilities_msg,
		&pimpl_->sub_svo_start,
		impl::on_svo_start_msg,
		&pimpl_->sub_svo_stop,
		impl::on_svo_stop_msg,
		&pimpl_->sub_svo_status,
		impl::on_svo_status_msg);
	subscribe_recorder_subjects(
		RecordingFormat::Mcap,
		&pimpl_->sub_mcap_capabilities,
		impl::on_mcap_capabilities_msg,
		&pimpl_->sub_mcap_start,
		impl::on_mcap_start_msg,
		&pimpl_->sub_mcap_stop,
		impl::on_mcap_stop_msg,
		&pimpl_->sub_mcap_status,
		impl::on_mcap_status_msg);

	spdlog::info("nats control service started for target '{}'", target_key);
	return true;
}

void NatsControlService::Stop() {
	auto destroy_subscription = [](natsSubscription *&subscription) {
		if (!subscription) {
			return;
		}
		natsSubscription_Unsubscribe(subscription);
		natsSubscription_Destroy(subscription);
		subscription = nullptr;
	};

	destroy_subscription(pimpl_->sub_source_reset);
	destroy_subscription(pimpl_->sub_source_info);
	destroy_subscription(pimpl_->sub_source_seek);
	destroy_subscription(pimpl_->sub_source_capabilities);
	destroy_subscription(pimpl_->sub_source_playlist_apply);
	destroy_subscription(pimpl_->sub_source_playlist_info);
	destroy_subscription(pimpl_->sub_svo_capabilities);
	destroy_subscription(pimpl_->sub_svo_start);
	destroy_subscription(pimpl_->sub_svo_stop);
	destroy_subscription(pimpl_->sub_svo_status);
	destroy_subscription(pimpl_->sub_mcap_capabilities);
	destroy_subscription(pimpl_->sub_mcap_start);
	destroy_subscription(pimpl_->sub_mcap_stop);
	destroy_subscription(pimpl_->sub_mcap_status);

	if (pimpl_->conn) {
		natsConnection_Close(pimpl_->conn);
		natsConnection_Destroy(pimpl_->conn);
		pimpl_->conn = nullptr;
	}
	pimpl_->started = false;
}

void NatsControlService::PublishModuleStatus(const int32_t status_code) {
	if (!pimpl_->conn) {
		return;
	}

	pb::ModuleStatusEvent event;
	event.set_status(to_proto_module_status(status_code));
	event.set_instance(pimpl_->instance_name);
	event.set_timestamp_ns(now_ns());

	pimpl_->publish_proto(
		nats::subject_status(pimpl_->target_key),
		event);
}

void NatsControlService::PublishBodyTracking(
	const std::span<const uint8_t> raw_bytes) {
	if (!pimpl_->conn) {
		return;
	}

	pimpl_->publish(
		nats::subject_body(pimpl_->target_key),
		raw_bytes.data(),
		static_cast<int>(raw_bytes.size()));
}

} // namespace cvmmap
