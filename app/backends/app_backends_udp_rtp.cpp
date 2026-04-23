#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <errno.h>
#include <spdlog/spdlog.h>

#include "app_backends_udp_rtp.hpp"
#include "app_config.hpp"

namespace app::backends {

namespace {

	constexpr const char *RAW_SINK_NAME       = "raw_sink";
	constexpr const char *ENCODED_SINK_NAME   = "encoded_sink";
	constexpr std::size_t MAX_PENDING_MATCHES = 16;

	enum class UdpRtpCodec {
		H264,
		H265,
	};

	struct UdpRtpCodecTraits {
		const char *encoding_name;
		const char *depay_factory;
		const char *parser_factory;
		const char *encoded_caps;
		const char *preferred_hw_decoder;
		const char *preferred_sw_decoder;
		cvmmap::EncodedCodec encoded_codec;
	};

	struct RawSample {
		frame_metadata_t metadata{};
		std::vector<uint8_t> bytes{};
	};

	struct EncodedSample {
		encoded_access_unit_t access_unit{};
	};

	uint64_t now_ns() {
		return static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::system_clock::now().time_since_epoch())
				.count());
	}

	std::optional<PixelFormat> gst_format_to_pixel_format(const GstVideoFormat format) {
		switch (format) {
		case GST_VIDEO_FORMAT_BGR:
			return PixelFormat::BGR;
		case GST_VIDEO_FORMAT_RGB:
			return PixelFormat::RGB;
		case GST_VIDEO_FORMAT_BGRA:
			return PixelFormat::BGRA;
		case GST_VIDEO_FORMAT_RGBA:
			return PixelFormat::RGBA;
		case GST_VIDEO_FORMAT_GRAY8:
			return PixelFormat::GRAY;
		default:
			return std::nullopt;
		}
	}

	uint8_t channels_for_pixel_format(const PixelFormat format) {
		switch (format) {
		case PixelFormat::RGB:
		case PixelFormat::BGR:
			return 3;
		case PixelFormat::RGBA:
		case PixelFormat::BGRA:
			return 4;
		case PixelFormat::GRAY:
			return 1;
		default:
			return 0;
		}
	}

	uint64_t timestamp_from_buffer(const GstBuffer *buffer) {
		if (buffer == nullptr) {
			return now_ns();
		}
		if (GST_BUFFER_PTS_IS_VALID(buffer)) {
			return static_cast<uint64_t>(GST_BUFFER_PTS(buffer));
		}
		if (GST_BUFFER_DTS_IS_VALID(buffer)) {
			return static_cast<uint64_t>(GST_BUFFER_DTS(buffer));
		}
		return now_ns();
	}

	bool is_element_available(const char *factory_name) {
		GstElementFactory *factory = gst_element_factory_find(factory_name);
		if (factory == nullptr) {
			return false;
		}
		gst_object_unref(factory);
		return true;
	}

	std::optional<UdpRtpCodec> codec_from_config(std::string_view configured) {
		if (configured == "h264") {
			return UdpRtpCodec::H264;
		}
		if (configured == "h265") {
			return UdpRtpCodec::H265;
		}
		return std::nullopt;
	}

	UdpRtpCodecTraits codec_traits(const UdpRtpCodec codec) {
		switch (codec) {
		case UdpRtpCodec::H264:
			return UdpRtpCodecTraits{
				.encoding_name        = "H264",
				.depay_factory        = "rtph264depay",
				.parser_factory       = "h264parse",
				.encoded_caps         = "video/x-h264,stream-format=byte-stream,alignment=au",
				.preferred_hw_decoder = "nvh264dec",
				.preferred_sw_decoder = "avdec_h264",
				.encoded_codec        = cvmmap::EncodedCodec::H264,
			};
		case UdpRtpCodec::H265:
		default:
			return UdpRtpCodecTraits{
				.encoding_name        = "H265",
				.depay_factory        = "rtph265depay",
				.parser_factory       = "h265parse",
				.encoded_caps         = "video/x-h265,stream-format=byte-stream,alignment=au",
				.preferred_hw_decoder = "nvh265dec",
				.preferred_sw_decoder = "avdec_h265",
				.encoded_codec        = cvmmap::EncodedCodec::H265,
			};
		}
	}

	std::string resolve_decoder_name(const UdpRtpCodec codec, const std::string &configured) {
		const auto traits = codec_traits(codec);
		if (configured == "auto") {
			if (is_element_available(traits.preferred_hw_decoder)) {
				return traits.preferred_hw_decoder;
			}
			if (is_element_available(traits.preferred_sw_decoder)) {
				return traits.preferred_sw_decoder;
			}
			return {};
		}
		return configured;
	}

	std::string make_pipeline_string(const app::UdpRtpConfig &config,
									 const UdpRtpCodec codec,
									 const std::string &decoder_name) {
		const auto traits = codec_traits(codec);
		return cvmmap::format(
			"udpsrc auto-multicast={} multicast-group={} port={} caps=\"application/x-rtp,media=video,clock-rate=90000,encoding-name={},payload={}\" ! "
			"{} ! {} config-interval=-1 disable-passthrough=true ! tee name=parsed_tee "
			"parsed_tee. ! queue ! {} ! appsink name={} emit-signals=true sync=false max-buffers=8 drop=true "
			"parsed_tee. ! queue ! {} ! videoconvert ! video/x-raw,format=BGR ! appsink name={} emit-signals=true sync=false max-buffers=2 drop=true",
			config.auto_multicast ? "true" : "false",
			config.multicast_group,
			config.port,
			traits.encoding_name,
			static_cast<unsigned>(config.payload_type),
			traits.depay_factory,
			traits.parser_factory,
			traits.encoded_caps,
			ENCODED_SINK_NAME,
			decoder_name,
			RAW_SINK_NAME);
	}

} // namespace

struct UdpRtpBackendImpl {
	app::UdpRtpConfig config{};
	app::VideoConfig video_config{};

	GstElement *pipeline{nullptr};
	GstElement *raw_sink{nullptr};
	GstElement *encoded_sink{nullptr};
	GstBus *bus{nullptr};
	std::jthread bus_thread{};
	std::atomic<bool> initialized{false};

	on_metadata_fn_t on_metadata{};
	on_frame_fn_t on_frame{};
	on_error_fn_t on_error{};
	on_encoded_access_unit_fn_t on_encoded_access_unit{};

	frame_metadata_t last_metadata{};
	bool metadata_sent{false};
	uint32_t source_frame_index{0};
	uint16_t frame_rate_num{0};
	uint16_t frame_rate_den{0};
	UdpRtpCodec codec{UdpRtpCodec::H265};
	std::mutex mutex{};
	std::map<uint64_t, RawSample> pending_raw{};
	std::map<uint64_t, EncodedSample> pending_encoded{};

	void emit_error(error_t error_code, std::string_view message) {
		if (on_error) {
			on_error(error_code, message);
		}
	}

	void shutdown_pipeline() {
		if (pipeline != nullptr) {
			gst_element_set_state(pipeline, GST_STATE_NULL);
		}
		if (bus_thread.joinable()) {
			bus_thread.request_stop();
			bus_thread.join();
		}
		if (bus != nullptr) {
			gst_object_unref(bus);
			bus = nullptr;
		}
		if (raw_sink != nullptr) {
			gst_object_unref(raw_sink);
			raw_sink = nullptr;
		}
		if (encoded_sink != nullptr) {
			gst_object_unref(encoded_sink);
			encoded_sink = nullptr;
		}
		if (pipeline != nullptr) {
			gst_object_unref(pipeline);
			pipeline = nullptr;
		}
		initialized.store(false, std::memory_order_release);
	}

	void prune_pending_locked() {
		while (pending_raw.size() > MAX_PENDING_MATCHES) {
			spdlog::warn("udp_rtp dropping unmatched raw frame pts={}", pending_raw.begin()->first);
			pending_raw.erase(pending_raw.begin());
		}
		while (pending_encoded.size() > MAX_PENDING_MATCHES) {
			spdlog::warn("udp_rtp dropping unmatched encoded AU pts={}", pending_encoded.begin()->first);
			pending_encoded.erase(pending_encoded.begin());
		}
	}

	void try_emit_matched(uint64_t pts_ns) {
		std::optional<RawSample> raw{};
		std::optional<EncodedSample> encoded{};
		bool should_emit_metadata{false};
		{
			std::lock_guard lock(mutex);
			auto raw_it     = pending_raw.find(pts_ns);
			auto encoded_it = pending_encoded.find(pts_ns);
			if (raw_it == pending_raw.end() || encoded_it == pending_encoded.end()) {
				prune_pending_locked();
				return;
			}
			raw.emplace(std::move(raw_it->second));
			encoded.emplace(std::move(encoded_it->second));
			pending_raw.erase(raw_it);
			pending_encoded.erase(encoded_it);
			should_emit_metadata = !metadata_sent;
			if (should_emit_metadata) {
				last_metadata = raw->metadata;
				metadata_sent = true;
			}
		}

		if (should_emit_metadata && on_metadata) {
			on_metadata(raw->metadata);
		}
		if (on_encoded_access_unit) {
			on_encoded_access_unit(encoded->access_unit);
		}
		if (on_frame) {
			on_frame(std::span<uint8_t>(raw->bytes.data(), raw->bytes.size()), raw->metadata);
		}
	}

	bool ingest_raw_sample(GstSample *sample) {
		if (sample == nullptr) {
			return false;
		}

		GstCaps *caps = gst_sample_get_caps(sample);
		if (caps == nullptr) {
			gst_sample_unref(sample);
			emit_error(-EINVAL, "udp_rtp raw sample missing caps");
			return false;
		}

		GstVideoInfo video_info{};
		if (!gst_video_info_from_caps(&video_info, caps)) {
			gst_sample_unref(sample);
			emit_error(-EINVAL, "udp_rtp raw sample caps are not video/x-raw");
			return false;
		}

		const auto pixel_format = gst_format_to_pixel_format(
			GST_VIDEO_INFO_FORMAT(&video_info));
		if (!pixel_format) {
			gst_sample_unref(sample);
			emit_error(-EINVAL, "udp_rtp raw sample format is unsupported");
			return false;
		}

		GstBuffer *buffer = gst_sample_get_buffer(sample);
		if (buffer == nullptr) {
			gst_sample_unref(sample);
			return false;
		}

		GstMapInfo map{};
		if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
			gst_sample_unref(sample);
			emit_error(-EIO, "udp_rtp failed to map raw sample buffer");
			return false;
		}

		RawSample raw{};
		raw.bytes.assign(map.data, map.data + map.size);
		raw.metadata.versions_major = cvmmap::FRAME_METADATA_V2_MAJOR;
		raw.metadata.versions_minor = cvmmap::FRAME_METADATA_V2_MINOR_ENCODED_AU;
		std::copy(
			frame_metadata_t::CV_MMAP_MAGIC.begin(),
			frame_metadata_t::CV_MMAP_MAGIC.end(),
			raw.metadata.magic);
		raw.metadata.frame_count       = ++source_frame_index;
		raw.metadata.timestamp_ns      = timestamp_from_buffer(buffer);
		raw.metadata.info.width        = static_cast<uint16_t>(GST_VIDEO_INFO_WIDTH(&video_info));
		raw.metadata.info.height       = static_cast<uint16_t>(GST_VIDEO_INFO_HEIGHT(&video_info));
		raw.metadata.info.channels     = channels_for_pixel_format(*pixel_format);
		raw.metadata.info.depth        = Depth::U8;
		raw.metadata.info.pixel_format = *pixel_format;
		raw.metadata.info.buffer_size  = static_cast<uint32_t>(raw.bytes.size());

		if (GST_VIDEO_INFO_FPS_N(&video_info) > 0 && GST_VIDEO_INFO_FPS_D(&video_info) > 0) {
			frame_rate_num = static_cast<uint16_t>(GST_VIDEO_INFO_FPS_N(&video_info));
			frame_rate_den = static_cast<uint16_t>(GST_VIDEO_INFO_FPS_D(&video_info));
		}

		gst_buffer_unmap(buffer, &map);
		gst_sample_unref(sample);

		{
			std::lock_guard lock(mutex);
			pending_raw[raw.metadata.timestamp_ns] = std::move(raw);
			prune_pending_locked();
		}
		try_emit_matched(raw.metadata.timestamp_ns);
		return true;
	}

	bool ingest_encoded_sample(GstSample *sample) {
		if (sample == nullptr) {
			return false;
		}

		GstBuffer *buffer = gst_sample_get_buffer(sample);
		if (buffer == nullptr) {
			gst_sample_unref(sample);
			return false;
		}

		GstMapInfo map{};
		if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
			gst_sample_unref(sample);
			emit_error(-EIO, "udp_rtp failed to map encoded sample buffer");
			return false;
		}

		EncodedSample encoded{};
		encoded.access_unit.codec            = codec_traits(codec).encoded_codec;
		encoded.access_unit.bitstream_format = cvmmap::EncodedBitstreamFormat::AnnexB;
		encoded.access_unit.flags =
			(GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT) ? 0
																		: FRAME_METADATA_V2_ENCODED_FLAG_KEYFRAME);
		encoded.access_unit.frame_rate_num      = frame_rate_num;
		encoded.access_unit.frame_rate_den      = frame_rate_den;
		encoded.access_unit.stream_pts_ns       = timestamp_from_buffer(buffer);
		encoded.access_unit.source_timestamp_ns = encoded.access_unit.stream_pts_ns;
		encoded.access_unit.bytes.assign(map.data, map.data + map.size);

		gst_buffer_unmap(buffer, &map);
		gst_sample_unref(sample);

		{
			std::lock_guard lock(mutex);
			pending_encoded[encoded.access_unit.source_timestamp_ns] = std::move(encoded);
			prune_pending_locked();
		}
		try_emit_matched(encoded.access_unit.source_timestamp_ns);
		return true;
	}

	void run_bus_loop(std::stop_token stop_token) {
		while (!stop_token.stop_requested() && bus != nullptr) {
			GstMessage *message = gst_bus_timed_pop_filtered(
				bus,
				200 * GST_MSECOND,
				static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
			if (message == nullptr) {
				continue;
			}

			switch (GST_MESSAGE_TYPE(message)) {
			case GST_MESSAGE_ERROR: {
				GError *error = nullptr;
				gchar *debug  = nullptr;
				gst_message_parse_error(message, &error, &debug);
				std::string message_text = error != nullptr ? error->message : "udp_rtp GStreamer pipeline error";
				emit_error(-EIO, message_text);
				if (error != nullptr) {
					g_error_free(error);
				}
				if (debug != nullptr) {
					g_free(debug);
				}
				break;
			}
			case GST_MESSAGE_EOS:
				emit_error(ERR_EOS, "udp_rtp pipeline reached EOS");
				break;
			default:
				break;
			}

			gst_message_unref(message);
		}
	}

	static GstFlowReturn on_new_raw_sample_static(GstAppSink *sink, gpointer user_data) {
		auto *self        = static_cast<UdpRtpBackendImpl *>(user_data);
		GstSample *sample = gst_app_sink_pull_sample(sink);
		return self->ingest_raw_sample(sample) ? GST_FLOW_OK : GST_FLOW_ERROR;
	}

	static GstFlowReturn on_new_encoded_sample_static(GstAppSink *sink, gpointer user_data) {
		auto *self        = static_cast<UdpRtpBackendImpl *>(user_data);
		GstSample *sample = gst_app_sink_pull_sample(sink);
		return self->ingest_encoded_sample(sample) ? GST_FLOW_OK : GST_FLOW_ERROR;
	}
};

UdpRtpBackend::UdpRtpBackend(app::UdpRtpConfig config, const app::VideoConfig &video_config)
	: impl(std::make_unique<UdpRtpBackendImpl>()) {
	impl->config       = std::move(config);
	impl->video_config = video_config;
}

UdpRtpBackend::~UdpRtpBackend() = default;

void UdpRtpBackend::Init() {
	static std::once_flag gst_init_flag;
	std::call_once(gst_init_flag, []() {
		gst_init(nullptr, nullptr);
		spdlog::info("GStreamer initialized: {}", gst_version_string());
	});

	const auto parsed_codec = codec_from_config(impl->config.codec);
	if (!parsed_codec) {
		impl->emit_error(-EINVAL, "udp_rtp codec must be h264 or h265");
		return;
	}
	impl->codec = *parsed_codec;

	const auto decoder_name = resolve_decoder_name(impl->codec, impl->config.decoder);
	if (decoder_name.empty()) {
		impl->emit_error(-ENOENT, "udp_rtp decoder auto resolution failed");
		return;
	}

	const auto pipeline_string = make_pipeline_string(impl->config, impl->codec, decoder_name);
	spdlog::info("udp_rtp pipeline: {}", pipeline_string);

	GError *error  = nullptr;
	impl->pipeline = gst_parse_launch(pipeline_string.c_str(), &error);
	if (error != nullptr) {
		const auto message = std::string(error->message);
		g_error_free(error);
		impl->emit_error(-EINVAL, message);
		return;
	}
	if (impl->pipeline == nullptr) {
		impl->emit_error(-ENODEV, "udp_rtp pipeline creation failed");
		return;
	}

	impl->raw_sink     = gst_bin_get_by_name(GST_BIN(impl->pipeline), RAW_SINK_NAME);
	impl->encoded_sink = gst_bin_get_by_name(GST_BIN(impl->pipeline), ENCODED_SINK_NAME);
	if (impl->raw_sink == nullptr || impl->encoded_sink == nullptr) {
		impl->emit_error(-ENOENT, "udp_rtp appsinks were not found in the pipeline");
		impl->shutdown_pipeline();
		return;
	}

	gst_app_sink_set_emit_signals(GST_APP_SINK(impl->raw_sink), TRUE);
	gst_app_sink_set_emit_signals(GST_APP_SINK(impl->encoded_sink), TRUE);
	g_signal_connect(impl->raw_sink, "new-sample", G_CALLBACK(UdpRtpBackendImpl::on_new_raw_sample_static), impl.get());
	g_signal_connect(impl->encoded_sink, "new-sample", G_CALLBACK(UdpRtpBackendImpl::on_new_encoded_sample_static), impl.get());

	impl->bus               = gst_element_get_bus(impl->pipeline);
	const auto state_change = gst_element_set_state(impl->pipeline, GST_STATE_PLAYING);
	if (state_change == GST_STATE_CHANGE_FAILURE) {
		impl->emit_error(-EIO, "udp_rtp pipeline failed to enter PLAYING state");
		impl->shutdown_pipeline();
		return;
	}

	impl->bus_thread = std::jthread([this](std::stop_token stop_token) {
		impl->run_bus_loop(stop_token);
	});
	impl->initialized.store(true, std::memory_order_release);
}

void UdpRtpBackend::Shutdown() {
	if (!impl) {
		return;
	}
	impl->shutdown_pipeline();
}

void UdpRtpBackend::SetOnMetadata(on_metadata_fn_t on_metadata) {
	impl->on_metadata = std::move(on_metadata);
}

void UdpRtpBackend::SetOnFrame(on_frame_fn_t on_frame) {
	impl->on_frame = std::move(on_frame);
}

void UdpRtpBackend::SetOnError(on_error_fn_t on_error) {
	impl->on_error = std::move(on_error);
}

void UdpRtpBackend::SetOnEncodedAccessUnit(on_encoded_access_unit_fn_t on_encoded_access_unit) {
	impl->on_encoded_access_unit = std::move(on_encoded_access_unit);
}

source_info_t UdpRtpBackend::GetSourceInfo() {
	source_info_t info{};
	info.source_kind          = cvmmap::SourceKind::Live;
	info.timestamp_domain     = cvmmap::TimestampDomain::MediaTimeNs;
	info.current_timestamp_ns = impl->last_metadata.timestamp_ns;
	info.current_frame_count  = impl->last_metadata.frame_count;
	return info;
}
error_t UdpRtpBackend::ResetFrameCount() {
	return -ENOTSUP;
}

} // namespace app::backends
