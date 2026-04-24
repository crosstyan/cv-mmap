#include "nats_service_protocol.hpp"

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
using namespace nats_service_detail;

constexpr std::string_view kNatsMicroServiceName = "cvmmap_producer";
constexpr std::string_view kNatsMicroServiceVersion = "0.1.0";
constexpr std::string_view kNatsMicroServiceDescription =
	"cv-mmap producer discovery and control service";

std::string micro_error_to_string(microError *error) {
	if (!error) {
		return {};
	}
	char buffer[512];
	return std::string(microError_String(error, buffer, sizeof(buffer)));
}

struct NatsControlService::impl {
	NatsControlServiceOptions options;
	NatsControlHandlers handlers;
	bool started{false};
	natsConnection *conn{nullptr};
	microService *service{nullptr};

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
	microError *reply(
		microRequest *request,
		const Response &response) {
		const auto size = response.ByteSizeLong();
		std::vector<uint8_t> bytes(size);
		if (!response.SerializeToArray(bytes.data(), static_cast<int>(size))) {
			return micro_Errorf("protobuf response serialization error");
		}
		return microRequest_Respond(
			request,
			reinterpret_cast<const char *>(bytes.data()),
			bytes.size());
	}

	static impl *from_request(microRequest *request) {
		return static_cast<impl *>(microRequest_GetServiceState(request));
	}

	template <typename Request>
	static bool parse_request(microRequest *request, Request *message) {
		auto *wire_message = microRequest_GetMsg(request);
		if (!wire_message) {
			return false;
		}
		return message->ParseFromArray(
			natsMsg_GetData(wire_message),
			natsMsg_GetDataLength(wire_message));
	}

	std::vector<std::string> build_metadata_storage() const {
		std::vector<std::string> metadata;
		metadata.reserve(28);
		auto append = [&metadata](std::string key, std::string value) {
			metadata.push_back(std::move(key));
			metadata.push_back(std::move(value));
		};

		append("instance_name", options.instance_name);
		append("namespace", options.namespace_name);
		append("ipc_prefix", options.ipc_prefix);
		append("base_name", options.base_name);
		append("nats_target_key", options.target_key);
		append("shm_name", options.shm_name);
		append("zmq_addr", options.zmq_addr);
		append("body_subject", nats::subject_body(options.target_key));
		append("status_subject", nats::subject_status(options.target_key));
		append("producer_subject_prefix", nats::subject_producer_prefix(options.target_key));
		append("backend", options.backend);
		append("build_revision", options.build_revision);
		append("build_tag", options.build_tag);
		append("build_branch", options.build_branch);
		append("build_timestamp_utc", options.build_timestamp_utc);
		return metadata;
	}

	static void on_micro_error(
		microService *,
		microEndpoint *,
		natsStatus status) {
		spdlog::error(
			"nats micro service internal error: {}",
			natsStatus_GetText(status));
	}

	static void on_micro_done(microService *service) {
		if (auto *self = static_cast<impl *>(microService_GetState(service))) {
			spdlog::info(
				"nats micro service stopped for target '{}'",
				self->options.target_key);
		}
	}

	static microError *on_source_reset_req(microRequest *request) {
		auto *self = from_request(request);
		pb::ResetFrameCountResponse response;
		if (self->handlers.on_reset_frame_count) {
			response.set_error(map_control_error_code(
				self->handlers.on_reset_frame_count()));
		} else {
			response.set_error(pb::ERROR_CODE_UNSUPPORTED);
		}
		return self->reply(request, response);
	}

	static microError *on_source_info_req(microRequest *request) {
		auto *self = from_request(request);
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
		return self->reply(request, response);
	}


	static microError *on_source_playlist_apply_req(microRequest *request) {
		auto *self = from_request(request);
		pb::ApplyPlaylistResponse response;
		if (!self->handlers.on_apply_playlist) {
			response.set_error(pb::ERROR_CODE_UNSUPPORTED);
			response.set_error_message("playlist apply is not supported by the active producer");
			return self->reply(request, response);
		}

		pb::ApplyPlaylistRequest wire_request;
		if (!parse_request(request, &wire_request)) {
			response.set_error(pb::ERROR_CODE_INVALID_PAYLOAD);
			response.set_error_message("invalid playlist apply payload");
			return self->reply(request, response);
		}

		auto parsed_request = parse_playlist_request(wire_request);
		if (!parsed_request) {
			response.set_error(map_control_error_code(parsed_request.error().code));
			response.set_error_message(parsed_request.error().message);
			return self->reply(request, response);
		}

		auto result = self->handlers.on_apply_playlist(*parsed_request);
		if (!result) {
			response.set_error(map_control_error_code(result.error().code));
			response.set_error_message(result.error().message);
			return self->reply(request, response);
		}

		response.set_error(pb::ERROR_CODE_OK);
		fill_playlist_info(*response.mutable_playlist_info(), *result);
		return self->reply(request, response);
	}

	static microError *on_source_playlist_info_req(microRequest *request) {
		auto *self = from_request(request);
		pb::GetPlaylistInfoResponse response;
		if (!self->handlers.on_get_playlist_info) {
			response.set_error(pb::ERROR_CODE_UNSUPPORTED);
			response.set_error_message("playlist query is not supported by the active producer");
			return self->reply(request, response);
		}

		pb::GetPlaylistInfoRequest wire_request;
		if (!parse_request(request, &wire_request)) {
			response.set_error(pb::ERROR_CODE_INVALID_PAYLOAD);
			response.set_error_message("invalid playlist info payload");
			return self->reply(request, response);
		}

		auto result = self->handlers.on_get_playlist_info();
		if (!result) {
			response.set_error(map_control_error_code(result.error().code));
			response.set_error_message(result.error().message);
			return self->reply(request, response);
		}

		response.set_error(pb::ERROR_CODE_OK);
		fill_playlist_info(*response.mutable_playlist_info(), *result);
		return self->reply(request, response);
	}

static microError *on_camera_control_capabilities_req(microRequest *request) {
	auto *self = from_request(request);
	pb::GetCameraControlCapabilitiesResponse response;
	if (!self->handlers.on_get_camera_control_capabilities) {
		response.set_error(pb::ERROR_CODE_UNSUPPORTED);
		response.set_error_message("camera control is not supported by the active producer");
		return self->reply(request, response);
	}

	pb::GetCameraControlCapabilitiesRequest wire_request;
	if (!parse_request(request, &wire_request)) {
		response.set_error(pb::ERROR_CODE_INVALID_PAYLOAD);
		response.set_error_message("invalid camera control capabilities payload");
		return self->reply(request, response);
	}

	auto result = self->handlers.on_get_camera_control_capabilities();
	if (!result) {
		response.set_error(map_control_error_code(result.error().code));
		response.set_error_message(result.error().message);
		return self->reply(request, response);
	}

	fill_camera_control_capabilities_response(response, *result);
	return self->reply(request, response);
}

static microError *on_camera_control_get_req(microRequest *request) {
	auto *self = from_request(request);
	pb::GetCameraControlResponse response;
	if (!self->handlers.on_get_camera_control) {
		response.set_error(pb::ERROR_CODE_UNSUPPORTED);
		response.set_error_message("camera control is not supported by the active producer");
		return self->reply(request, response);
	}

	pb::GetCameraControlRequest wire_request;
	if (!parse_request(request, &wire_request)) {
		response.set_error(pb::ERROR_CODE_INVALID_PAYLOAD);
		response.set_error_message("invalid camera control get payload");
		return self->reply(request, response);
	}

	auto setting = parse_camera_control_setting(wire_request.setting());
	if (!setting) {
		response.set_error(map_control_error_code(setting.error().code));
		response.set_error_message(setting.error().message);
		return self->reply(request, response);
	}

	auto result = self->handlers.on_get_camera_control(*setting);
	if (!result) {
		response.set_error(map_control_error_code(result.error().code));
		response.set_error_message(result.error().message);
		return self->reply(request, response);
	}

	response.set_error(pb::ERROR_CODE_OK);
	fill_camera_control_state(*response.mutable_control(), *result);
	return self->reply(request, response);
}

static microError *on_camera_control_set_req(microRequest *request) {
	auto *self = from_request(request);
	pb::SetCameraControlResponse response;
	if (!self->handlers.on_set_camera_control) {
		response.set_error(pb::ERROR_CODE_UNSUPPORTED);
		response.set_error_message("camera control is not supported by the active producer");
		return self->reply(request, response);
	}

	pb::SetCameraControlRequest wire_request;
	if (!parse_request(request, &wire_request)) {
		response.set_error(pb::ERROR_CODE_INVALID_PAYLOAD);
		response.set_error_message("invalid camera control set payload");
		return self->reply(request, response);
	}

	auto parsed_request = parse_camera_control_request(wire_request);
	if (!parsed_request) {
		response.set_error(map_control_error_code(parsed_request.error().code));
		response.set_error_message(parsed_request.error().message);
		return self->reply(request, response);
	}

	auto result = self->handlers.on_set_camera_control(*parsed_request);
	if (!result) {
		response.set_error(map_control_error_code(result.error().code));
		response.set_error_message(result.error().message);
		return self->reply(request, response);
	}

	response.set_error(pb::ERROR_CODE_OK);
	fill_camera_control_state(*response.mutable_control(), *result);
	return self->reply(request, response);
}

static microError *on_camera_control_set_range_req(microRequest *request) {
	auto *self = from_request(request);
	pb::SetCameraControlRangeResponse response;
	if (!self->handlers.on_set_camera_control_range) {
		response.set_error(pb::ERROR_CODE_UNSUPPORTED);
		response.set_error_message("camera control is not supported by the active producer");
		return self->reply(request, response);
	}

	pb::SetCameraControlRangeRequest wire_request;
	if (!parse_request(request, &wire_request)) {
		response.set_error(pb::ERROR_CODE_INVALID_PAYLOAD);
		response.set_error_message("invalid camera control set_range payload");
		return self->reply(request, response);
	}

	auto parsed_request = parse_camera_control_range_request(wire_request);
	if (!parsed_request) {
		response.set_error(map_control_error_code(parsed_request.error().code));
		response.set_error_message(parsed_request.error().message);
		return self->reply(request, response);
	}

	auto result = self->handlers.on_set_camera_control_range(*parsed_request);
	if (!result) {
		response.set_error(map_control_error_code(result.error().code));
		response.set_error_message(result.error().message);
		return self->reply(request, response);
	}

	response.set_error(pb::ERROR_CODE_OK);
	fill_camera_control_state(*response.mutable_control(), *result);
	return self->reply(request, response);
}


	static microError *on_svo_recording_capabilities_req(microRequest *request) {
		auto *self = from_request(request);
		pb::CapabilitiesResponse response;
		const auto capabilities =
			self->handlers.on_get_svo_recording_capabilities ?
				self->handlers.on_get_svo_recording_capabilities() :
				SvoRecordingCapabilities{};
		fill_recording_capabilities_response(
			response,
			capabilities.can_record ? std::initializer_list<RecordingFormat>{RecordingFormat::Svo}
									: std::initializer_list<RecordingFormat>{});
		return self->reply(request, response);
	}

	static microError *on_svo_recording_start_req(microRequest *request) {
		auto *self = from_request(request);
		pb::RecordingStatusResponse response;
		if (!self->handlers.on_start_svo_recording) {
			response.set_error(pb::ERROR_CODE_UNSUPPORTED);
			return self->reply(request, response);
		}

		pb::RecordingStartRequest wire_request;
		if (!parse_request(request, &wire_request)) {
			response.set_error(pb::ERROR_CODE_INVALID_PAYLOAD);
			return self->reply(request, response);
		}

		auto parsed_request = parse_svo_recording_request(wire_request);
		if (!parsed_request) {
			response.set_error(map_control_error_code(parsed_request.error().code));
			response.set_error_message(parsed_request.error().message);
			return self->reply(request, response);
		}

		try {
			auto result = self->handlers.on_start_svo_recording(*parsed_request);
			if (!result) {
				response.set_error(map_control_error_code(result.error().code));
				response.set_error_message(result.error().message);
				return self->reply(request, response);
			}
			fill_svo_recording_status_response(response, *result);
			return self->reply(request, response);
		} catch (const std::exception &e) {
			response.set_error(pb::ERROR_CODE_ERROR);
			response.set_error_message(
				cvmmap::format("unexpected recording start failure: {}", e.what()));
			return self->reply(request, response);
		} catch (...) {
			response.set_error(pb::ERROR_CODE_ERROR);
			response.set_error_message("unexpected recording start failure");
			return self->reply(request, response);
		}
	}

	static microError *on_svo_recording_stop_req(microRequest *request) {
		auto *self = from_request(request);
		pb::RecordingStatusResponse response;
		if (!self->handlers.on_stop_svo_recording) {
			response.set_error(pb::ERROR_CODE_UNSUPPORTED);
			return self->reply(request, response);
		}

		pb::RecordingStopRequest wire_request;
		if (!parse_request(request, &wire_request)) {
			response.set_error(pb::ERROR_CODE_INVALID_PAYLOAD);
			return self->reply(request, response);
		}

		auto result = self->handlers.on_stop_svo_recording();
		if (!result) {
			response.set_error(map_control_error_code(result.error().code));
			response.set_error_message(result.error().message);
			return self->reply(request, response);
		}

		fill_svo_recording_status_response(response, *result);
		return self->reply(request, response);
	}

	static microError *on_svo_recording_status_req(microRequest *request) {
		auto *self = from_request(request);
		pb::RecordingStatusResponse response;
		if (!self->handlers.on_get_svo_recording_status) {
			response.set_error(pb::ERROR_CODE_UNSUPPORTED);
			return self->reply(request, response);
		}

		pb::RecordingStatusRequest wire_request;
		if (!parse_request(request, &wire_request)) {
			response.set_error(pb::ERROR_CODE_INVALID_PAYLOAD);
			return self->reply(request, response);
		}

		auto result = self->handlers.on_get_svo_recording_status();
		if (!result) {
			response.set_error(map_control_error_code(result.error().code));
			response.set_error_message(result.error().message);
			return self->reply(request, response);
		}

		fill_svo_recording_status_response(response, *result);
		return self->reply(request, response);
	}
};

NatsControlService::NatsControlService(NatsControlServiceOptions options)
	: pimpl_(std::make_unique<impl>()) {
	pimpl_->options = std::move(options);
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
	natsOptions_SetURL(options, pimpl_->options.nats_url.c_str());

	const auto status = natsConnection_Connect(&pimpl_->conn, options);
	natsOptions_Destroy(options);
	if (status != NATS_OK) {
		spdlog::error(
			"nats connect to '{}': {}",
			pimpl_->options.nats_url,
			natsStatus_GetText(status));
		return false;
	}
	spdlog::info("nats connected to '{}'", pimpl_->options.nats_url);

	auto metadata_storage = pimpl_->build_metadata_storage();
	std::vector<const char *> metadata_list;
	metadata_list.reserve(metadata_storage.size());
	for (const auto &entry : metadata_storage) {
		metadata_list.push_back(entry.c_str());
	}

	const auto &target_key = pimpl_->options.target_key;
	const auto default_subject = nats::subject_producer_source_info(target_key);
	microEndpointConfig default_endpoint{};
	default_endpoint.Name = "source_info";
	default_endpoint.Subject = default_subject.c_str();
	default_endpoint.Handler = impl::on_source_info_req;

	microServiceConfig service_config{};
	service_config.Name = kNatsMicroServiceName.data();
	service_config.Version = kNatsMicroServiceVersion.data();
	service_config.Description = kNatsMicroServiceDescription.data();
	service_config.Metadata = natsMetadata{
		.List = metadata_list.data(),
		.Count = static_cast<int>(metadata_storage.size() / 2),
	};
	service_config.Endpoint = &default_endpoint;
	service_config.ErrHandler = impl::on_micro_error;
	service_config.DoneHandler = impl::on_micro_done;
	service_config.State = pimpl_.get();

	if (auto *error = micro_AddService(&pimpl_->service, pimpl_->conn, &service_config)) {
		spdlog::error(
			"nats micro service start error '{}': {}",
			kNatsMicroServiceName,
			micro_error_to_string(error));
		microError_Destroy(error);
		natsConnection_Close(pimpl_->conn);
		natsConnection_Destroy(pimpl_->conn);
		pimpl_->conn = nullptr;
		return false;
	}
	const auto add_endpoint =
		[this](const char *name, const std::string &subject, microRequestHandler handler) -> bool {
			microEndpointConfig endpoint_config{};
			endpoint_config.Name = name;
			endpoint_config.Subject = subject.c_str();
			endpoint_config.Handler = handler;
			if (auto *error = microService_AddEndpoint(pimpl_->service, &endpoint_config)) {
				spdlog::error(
					"nats micro endpoint registration error '{}' on '{}': {}",
					name,
					subject,
					micro_error_to_string(error));
				microError_Destroy(error);
				return false;
			}
			return true;
		};

	const auto all_added =
		add_endpoint("source_reset", nats::subject_producer_source_reset(target_key), impl::on_source_reset_req) &&
		add_endpoint("source_playlist_apply", nats::subject_producer_source_playlist_apply(target_key), impl::on_source_playlist_apply_req) &&
		add_endpoint("source_playlist_info", nats::subject_producer_source_playlist_info(target_key), impl::on_source_playlist_info_req) &&
		add_endpoint("camera_control_capabilities", nats::subject_producer_camera_control_capabilities(target_key), impl::on_camera_control_capabilities_req) &&
		add_endpoint("camera_control_get", nats::subject_producer_camera_control_get(target_key), impl::on_camera_control_get_req) &&
		add_endpoint("camera_control_set", nats::subject_producer_camera_control_set(target_key), impl::on_camera_control_set_req) &&
		add_endpoint("camera_control_set_range", nats::subject_producer_camera_control_set_range(target_key), impl::on_camera_control_set_range_req) &&
		add_endpoint("recorder_svo_capabilities", nats::subject_producer_svo_recorder_capabilities(target_key), impl::on_svo_recording_capabilities_req) &&
		add_endpoint("recorder_svo_start", nats::subject_producer_svo_recorder_start(target_key), impl::on_svo_recording_start_req) &&
		add_endpoint("recorder_svo_stop", nats::subject_producer_svo_recorder_stop(target_key), impl::on_svo_recording_stop_req) &&
		add_endpoint("recorder_svo_status", nats::subject_producer_svo_recorder_status(target_key), impl::on_svo_recording_status_req);

	if (!all_added) {
		if (pimpl_->service) {
			microError_Ignore(microService_Destroy(pimpl_->service));
			pimpl_->service = nullptr;
		}
		natsConnection_Close(pimpl_->conn);
		natsConnection_Destroy(pimpl_->conn);
		pimpl_->conn = nullptr;
		return false;
	}

	if (pimpl_->handlers.on_get_svo_recording_capabilities &&
		!pimpl_->handlers.on_get_svo_recording_capabilities().can_record) {
		spdlog::info(
			"nats SVO recorder unavailable for target '{}'; responding to control requests with unavailable/unsupported status",
			target_key);
	}

	pimpl_->started = true;
	spdlog::info(
		"nats micro service '{}' started for target '{}' (service discovery enabled)",
		kNatsMicroServiceName,
		target_key);
	spdlog::info("nats control service started for target '{}'", target_key);
	return true;
}

void NatsControlService::Stop() {
	if (pimpl_->service) {
		if (auto *error = microService_Destroy(pimpl_->service)) {
			spdlog::error(
				"nats micro service stop error '{}': {}",
				kNatsMicroServiceName,
				micro_error_to_string(error));
			microError_Destroy(error);
		}
		pimpl_->service = nullptr;
	}

	if (pimpl_->conn) {
		natsConnection_Close(pimpl_->conn);
		natsConnection_Destroy(pimpl_->conn);
		pimpl_->conn = nullptr;
	}
	pimpl_->started = false;
}

void NatsControlService::PublishModuleStatus(const ModuleStatus status) {
	if (!pimpl_->conn) {
		return;
	}

	pb::ModuleStatusEvent event;
	event.set_status(to_proto_module_status(status));
	event.set_instance(pimpl_->options.instance_name);
	event.set_timestamp_ns(now_ns());

	pimpl_->publish_proto(
		nats::subject_status(pimpl_->options.target_key),
		event);
}

void NatsControlService::PublishBodyTracking(
	const std::span<const uint8_t> raw_bytes) {
	if (!pimpl_->conn) {
		return;
	}

	pimpl_->publish(
		nats::subject_body(pimpl_->options.target_key),
		raw_bytes.data(),
		static_cast<int>(raw_bytes.size()));
}

} // namespace cvmmap
