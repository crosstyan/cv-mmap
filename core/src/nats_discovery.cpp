#include <cvmmap/client.hpp>
#include <cvmmap/nats_subjects.hpp>

#include <cvmmap/compat/expected.hpp>
#include <cvmmap/compat/format.hpp>
#include <google/protobuf/util/json_util.h>
#include <nats.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <set>
#include <string>
#include <vector>

#include <control.pb.h>

namespace cvmmap {

namespace pb = ::cvmmap::proto;
namespace json = ::google::protobuf::util;

namespace {

constexpr std::string_view kNatsMicroServiceName = "cvmmap_producer";

DiscoveryError make_discovery_error(
	const DiscoveryErrorCode code,
	std::string message) {
	return DiscoveryError{
		.code = code,
		.message = std::move(message),
	};
}

template <typename StatusLike>
std::string status_message_text(const StatusLike &status) {
	const auto message = status.message();
	if constexpr (requires { message.as_string(); }) {
		return std::string(message.as_string());
	} else {
		return std::string(message);
	}
}

DiscoveryError make_nats_discovery_error(
	const natsStatus status,
	std::string subject) {
	const auto code =
		status == NATS_TIMEOUT ?
			DiscoveryErrorCode::Timeout :
			DiscoveryErrorCode::NatsError;
	return make_discovery_error(
		code,
		cvmmap::format(
			"nats discovery request to '{}': {}",
			subject,
			natsStatus_GetText(status)));
}

template <typename Message>
cvmmap::expected<Message, DiscoveryError> parse_discovery_json(
	std::span<const uint8_t> data,
	std::string_view context) {
	Message message;
	const auto json_payload = std::string(
		reinterpret_cast<const char *>(data.data()),
		data.size());
	auto status = json::JsonStringToMessage(json_payload, &message);
	if (!status.ok()) {
		return cvmmap::unexpected(make_discovery_error(
			DiscoveryErrorCode::InvalidPayload,
			cvmmap::format(
				"invalid {} discovery payload: {}",
				context,
				status_message_text(status))));
	}
	return message;
}

std::optional<std::string> metadata_value(
	const google::protobuf::Map<std::string, std::string> &metadata,
	std::string_view key) {
	const auto it = metadata.find(std::string(key));
	if (it == metadata.end()) {
		return std::nullopt;
	}
	return it->second;
}

cvmmap::expected<DiscoveredProducer, DiscoveryError> to_discovered_producer(
	const pb::NatsMicroInfoResponse &info) {
	DiscoveredProducer producer{
		.service_id = info.id(),
		.service_name = info.name(),
		.service_version = info.version(),
	};

	if (producer.service_name != kNatsMicroServiceName) {
		return cvmmap::unexpected(make_discovery_error(
			DiscoveryErrorCode::InvalidPayload,
			cvmmap::format(
				"unexpected discovery service name '{}'",
				producer.service_name)));
	}

	const auto assign_required =
		[&](std::string_view key, std::string *field) -> bool {
		auto value = metadata_value(info.metadata(), key);
		if (!value || value->empty()) {
			return false;
		}
		*field = std::move(*value);
		return true;
	};

	if (!assign_required("instance_name", &producer.instance_name) ||
		!assign_required("nats_target_key", &producer.nats_target_key) ||
		!assign_required("shm_name", &producer.shm_name) ||
		!assign_required("zmq_addr", &producer.zmq_addr)) {
		return cvmmap::unexpected(make_discovery_error(
			DiscoveryErrorCode::InvalidPayload,
			cvmmap::format(
				"service '{}' is missing required cvmmap transport metadata",
				producer.service_id)));
	}

	if (auto value = metadata_value(info.metadata(), "namespace")) {
		producer.namespace_name = std::move(*value);
	}
	if (auto value = metadata_value(info.metadata(), "ipc_prefix")) {
		producer.ipc_prefix = std::move(*value);
	}
	if (auto value = metadata_value(info.metadata(), "base_name")) {
		producer.base_name = std::move(*value);
	}
	if (auto value = metadata_value(info.metadata(), "body_subject")) {
		producer.body_subject = std::move(*value);
	} else {
		producer.body_subject = nats::subject_body(producer.nats_target_key);
	}
	if (auto value = metadata_value(info.metadata(), "status_subject")) {
		producer.status_subject = std::move(*value);
	} else {
		producer.status_subject = nats::subject_status(producer.nats_target_key);
	}
	if (auto value = metadata_value(info.metadata(), "producer_subject_prefix")) {
		producer.producer_subject_prefix = std::move(*value);
	} else {
		producer.producer_subject_prefix =
			nats::subject_producer_prefix(producer.nats_target_key);
	}
	if (auto value = metadata_value(info.metadata(), "backend")) {
		producer.backend = std::move(*value);
	}

	for (const auto &endpoint : info.endpoints()) {
		if (!endpoint.subject().empty()) {
			producer.producer_subjects.push_back(endpoint.subject());
		}
	}
	std::sort(producer.producer_subjects.begin(), producer.producer_subjects.end());
	producer.producer_subjects.erase(
		std::unique(
			producer.producer_subjects.begin(),
			producer.producer_subjects.end()),
		producer.producer_subjects.end());

	return producer;
}

bool matches_query(
	const DiscoveredProducer &producer,
	const DiscoveryQuery &query) {
	if (query.instance_name && producer.instance_name != *query.instance_name) {
		return false;
	}
	if (query.nats_target_key &&
		producer.nats_target_key != *query.nats_target_key) {
		return false;
	}
	if (query.backend && producer.backend != *query.backend) {
		return false;
	}
	return true;
}

} // namespace

cvmmap::expected<std::vector<DiscoveredProducer>, DiscoveryError>
DiscoverCvMmapProducers(
	const DiscoveryRequest &request,
	const std::chrono::milliseconds timeout) {
	auto nats_url = request.nats_url.value_or(
		std::string(CvMmapClient::DEFAULT_NATS_URL));

	natsOptions *options = nullptr;
	natsConnection *conn = nullptr;
	natsInbox *inbox = nullptr;
	natsSubscription *sub = nullptr;

	const auto cleanup = [&]() {
		if (sub) {
			natsSubscription_Unsubscribe(sub);
			natsSubscription_Destroy(sub);
			sub = nullptr;
		}
		if (inbox) {
			natsInbox_Destroy(inbox);
			inbox = nullptr;
		}
		if (conn) {
			natsConnection_Close(conn);
			natsConnection_Destroy(conn);
			conn = nullptr;
		}
		if (options) {
			natsOptions_Destroy(options);
			options = nullptr;
		}
	};

	auto fail = [&](DiscoveryError error)
		-> cvmmap::expected<std::vector<DiscoveredProducer>, DiscoveryError> {
		cleanup();
		return cvmmap::unexpected(std::move(error));
	};

	natsOptions_Create(&options);
	natsOptions_SetURL(options, nats_url.c_str());
	auto status = natsConnection_Connect(&conn, options);
	if (status != NATS_OK) {
		return fail(make_nats_discovery_error(status, nats_url));
	}

	status = natsInbox_Create(&inbox);
	if (status != NATS_OK) {
		return fail(make_nats_discovery_error(status, "$SRV.PING"));
	}

	status = natsConnection_SubscribeSync(
		&sub,
		conn,
		reinterpret_cast<const char *>(inbox));
	if (status != NATS_OK) {
		return fail(make_nats_discovery_error(status, "$SRV.PING"));
	}

	const auto ping_subject =
		cvmmap::format("$SRV.PING.{}", kNatsMicroServiceName);
	status = natsConnection_PublishRequest(
		conn,
		ping_subject.c_str(),
		reinterpret_cast<const char *>(inbox),
		nullptr,
		0);
	if (status != NATS_OK) {
		return fail(make_nats_discovery_error(status, ping_subject));
	}

	std::set<std::string> service_ids;
	size_t malformed_ping_count = 0;
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (true) {
		const auto now = std::chrono::steady_clock::now();
		if (now >= deadline) {
			break;
		}

		const auto remaining_ms =
			std::max<int64_t>(
				1,
				std::chrono::duration_cast<std::chrono::milliseconds>(
					deadline - now)
					.count());
		natsMsg *message = nullptr;
		status = natsSubscription_NextMsg(
			&message,
			sub,
			static_cast<int64_t>(remaining_ms));
		if (status == NATS_TIMEOUT) {
			break;
		}
		if (status != NATS_OK) {
			return fail(make_nats_discovery_error(status, ping_subject));
		}

		const auto payload = std::span<const uint8_t>(
			reinterpret_cast<const uint8_t *>(natsMsg_GetData(message)),
			static_cast<size_t>(natsMsg_GetDataLength(message)));
		auto parsed = parse_discovery_json<pb::NatsMicroPingResponse>(
			payload,
			"ping");
		natsMsg_Destroy(message);
		if (!parsed) {
			++malformed_ping_count;
			spdlog::warn("{}", parsed.error().message);
			continue;
		}
		if (parsed->name() == kNatsMicroServiceName && !parsed->id().empty()) {
			service_ids.insert(parsed->id());
		}
	}

	std::vector<DiscoveredProducer> producers;
	size_t malformed_info_count = 0;
	for (const auto &service_id : service_ids) {
		const auto info_subject =
			cvmmap::format("$SRV.INFO.{}.{}", kNatsMicroServiceName, service_id);
		natsMsg *reply = nullptr;
		status = natsConnection_Request(
			&reply,
			conn,
			info_subject.c_str(),
			nullptr,
			0,
			timeout.count());
		if (status != NATS_OK) {
			cleanup();
			return cvmmap::unexpected(make_nats_discovery_error(status, info_subject));
		}

		const auto payload = std::span<const uint8_t>(
			reinterpret_cast<const uint8_t *>(natsMsg_GetData(reply)),
			static_cast<size_t>(natsMsg_GetDataLength(reply)));
		auto parsed_info = parse_discovery_json<pb::NatsMicroInfoResponse>(
			payload,
			"info");
		natsMsg_Destroy(reply);
		if (!parsed_info) {
			++malformed_info_count;
			spdlog::warn("{}", parsed_info.error().message);
			continue;
		}

		auto producer = to_discovered_producer(*parsed_info);
		if (!producer) {
			++malformed_info_count;
			spdlog::warn("{}", producer.error().message);
			continue;
		}
		if (matches_query(*producer, request.query)) {
			producers.push_back(std::move(*producer));
		}
	}

	if (producers.empty() &&
		(service_ids.empty() ? malformed_ping_count : malformed_info_count) > 0) {
		return fail(make_discovery_error(
			DiscoveryErrorCode::InvalidPayload,
			"service discovery received malformed cvmmap discovery payloads"));
	}

	cleanup();
	return producers;
}

} // namespace cvmmap
