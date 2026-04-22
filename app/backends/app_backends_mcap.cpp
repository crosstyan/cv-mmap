#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <errno.h>
#include <cvmmap/compat/expected.hpp>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#define MCAP_IMPLEMENTATION

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <google/protobuf/stubs/common.h>
#include <mcap/reader.hpp>
#include <rvl/rvl.hpp>
#include <spdlog/spdlog.h>

#include <cvmmap/parser.hpp>

#include "app_backends_facade.hpp"
#include "app_backends_mcap.hpp"
#include "app_config.hpp"
#include "app_enum_models.hpp"
#include "proto/cvmmap_streamer/DepthMap.pb.h"
#include "proto/foxglove/CompressedVideo.pb.h"

namespace app::backends {

namespace {

	using clock_t = std::chrono::steady_clock;

	constexpr auto kBodyEncoding = "cvmmap.body_tracking.v1";

	struct VideoSample {
		uint64_t timestamp_ns{0};
		std::string format{};
		std::vector<uint8_t> bytes{};
		bool keyframe{false};
	};

	struct DepthSample {
		uint64_t timestamp_ns{0};
		uint32_t width{0};
		uint32_t height{0};
		cvmmap_streamer::DepthMap::DepthUnit source_unit{
			cvmmap_streamer::DepthMap::DEPTH_UNIT_UNKNOWN};
		cvmmap_streamer::DepthMap::StorageUnit storage_unit{
			cvmmap_streamer::DepthMap::STORAGE_UNIT_UNKNOWN};
		cvmmap_streamer::DepthMap::Encoding encoding{
			cvmmap_streamer::DepthMap::ENCODING_UNKNOWN};
		std::vector<uint8_t> bytes{};
	};

	struct BodySample {
		uint64_t timestamp_ns{0};
		cvmmap::body_tracking_frame_t frame{};
	};

	struct PublishPacket {
		frame_metadata_t metadata{};
		std::vector<uint8_t> payload{};
		std::vector<cvmmap::body_tracking_frame_t> body_frames{};
	};

	std::string ffmpeg_error_string(const int errnum) {
		std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
		av_strerror(errnum, buffer.data(), buffer.size());
		return std::string(buffer.data());
	}

	uint64_t proto_timestamp_to_ns(const google::protobuf::Timestamp &timestamp) {
		if (timestamp.seconds() < 0 || timestamp.nanos() < 0) {
			return 0;
		}
		return static_cast<uint64_t>(timestamp.seconds()) * 1000000000ull +
			   static_cast<uint64_t>(timestamp.nanos());
	}

	AVCodecID codec_id_from_format(const std::string_view format) {
		if (format == "h264") {
			return AV_CODEC_ID_H264;
		}
		if (format == "h265" || format == "hevc") {
			return AV_CODEC_ID_HEVC;
		}
		if (format == "vp9") {
			return AV_CODEC_ID_VP9;
		}
		if (format == "av1") {
			return AV_CODEC_ID_AV1;
		}
		return AV_CODEC_ID_NONE;
	}

	cvmmap::expected<uint64_t, std::string> probe_mcap_start_timestamp_ns(
		const std::string &path,
		const std::string &video_topic) {
		mcap::McapReader reader{};
		const auto open_status = reader.open(path);
		if (!open_status.ok()) {
			return cvmmap::unexpected("open MCAP failed: " + open_status.message);
		}

		mcap::ReadMessageOptions options{};
		options.readOrder = mcap::ReadMessageOptions::ReadOrder::FileOrder;
		options.topicFilter = [&video_topic](const std::string_view topic) {
			return topic == video_topic;
		};

		uint64_t min_timestamp_ns = std::numeric_limits<uint64_t>::max();
		auto messages = reader.readMessages(
			[](const mcap::Status &) {},
			options);
		for (auto it = messages.begin(); it != messages.end(); ++it) {
			if (it->channel == nullptr || it->channel->topic != video_topic) {
				continue;
			}
			foxglove::CompressedVideo video{};
			if (!video.ParseFromArray(
					it->message.data,
					static_cast<int>(it->message.dataSize))) {
				reader.close();
				return cvmmap::unexpected("failed to parse foxglove.CompressedVideo payload");
			}
			auto timestamp_ns = proto_timestamp_to_ns(video.timestamp());
			if (timestamp_ns == 0) {
				timestamp_ns = it->message.logTime;
			}
			min_timestamp_ns = std::min(min_timestamp_ns, timestamp_ns);
		}
		reader.close();

		if (min_timestamp_ns == std::numeric_limits<uint64_t>::max()) {
			return cvmmap::unexpected(
				"MCAP file does not contain any video messages on topic '" + video_topic + "'");
		}
		return min_timestamp_ns;
	}

	size_t find_start_code(std::span<const uint8_t> bytes, size_t offset) {
		for (size_t i = offset; i + 3 < bytes.size(); ++i) {
			if (bytes[i] == 0x00 && bytes[i + 1] == 0x00) {
				if (bytes[i + 2] == 0x01) {
					return i;
				}
				if (i + 3 < bytes.size() && bytes[i + 2] == 0x00 &&
					bytes[i + 3] == 0x01) {
					return i;
				}
			}
		}
		return bytes.size();
	}

	size_t start_code_size(std::span<const uint8_t> bytes, size_t offset) {
		if (offset + 3 < bytes.size() && bytes[offset] == 0x00 &&
			bytes[offset + 1] == 0x00 && bytes[offset + 2] == 0x01) {
			return 3;
		}
		if (offset + 4 < bytes.size() && bytes[offset] == 0x00 &&
			bytes[offset + 1] == 0x00 && bytes[offset + 2] == 0x00 &&
			bytes[offset + 3] == 0x01) {
			return 4;
		}
		return 0;
	}

	bool looks_like_keyframe_h264(std::span<const uint8_t> bytes) {
		for (size_t offset = find_start_code(bytes, 0); offset < bytes.size();
			 offset        = find_start_code(bytes, offset + 1)) {
			const auto prefix_size = start_code_size(bytes, offset);
			if (prefix_size == 0) {
				break;
			}
			const auto nal_offset = offset + prefix_size;
			if (nal_offset >= bytes.size()) {
				break;
			}
			const auto nal_type = static_cast<uint8_t>(bytes[nal_offset] & 0x1F);
			if (nal_type == 5) {
				return true;
			}
		}
		return false;
	}

	bool looks_like_keyframe_h265(std::span<const uint8_t> bytes) {
		for (size_t offset = find_start_code(bytes, 0); offset < bytes.size();
			 offset        = find_start_code(bytes, offset + 1)) {
			const auto prefix_size = start_code_size(bytes, offset);
			if (prefix_size == 0) {
				break;
			}
			const auto nal_offset = offset + prefix_size;
			if (nal_offset >= bytes.size()) {
				break;
			}
			const auto nal_type =
				static_cast<uint8_t>((bytes[nal_offset] >> 1) & 0x3F);
			if (nal_type >= 16 && nal_type <= 23) {
				return true;
			}
		}
		return false;
	}

	bool detect_keyframe(const std::string_view format,
						 std::span<const uint8_t> bytes,
						 size_t index) {
		if (index == 0) {
			return true;
		}
		if (format == "h264") {
			return looks_like_keyframe_h264(bytes);
		}
		if (format == "h265" || format == "hevc") {
			return looks_like_keyframe_h265(bytes);
		}
		return false;
	}

	cvmmap::DepthUnit depth_unit_from_proto(
		const cvmmap_streamer::DepthMap::DepthUnit unit) {
		switch (unit) {
		case cvmmap_streamer::DepthMap::DEPTH_UNIT_MILLIMETER:
			return cvmmap::DepthUnit::Millimeter;
		case cvmmap_streamer::DepthMap::DEPTH_UNIT_METER:
			return cvmmap::DepthUnit::Meter;
		case cvmmap_streamer::DepthMap::DEPTH_UNIT_UNKNOWN:
		default:
			return cvmmap::DepthUnit::Unknown;
		}
	}

	float convert_depth_sample(float value,
							   cvmmap_streamer::DepthMap::StorageUnit storage_unit,
							   cvmmap_streamer::DepthMap::DepthUnit source_unit) {
		if (!std::isfinite(value) || value <= 0.0f) {
			return std::numeric_limits<float>::quiet_NaN();
		}

		if (storage_unit == cvmmap_streamer::DepthMap::STORAGE_UNIT_MILLIMETER &&
			source_unit == cvmmap_streamer::DepthMap::DEPTH_UNIT_METER) {
			return value / 1000.0f;
		}
		if (storage_unit == cvmmap_streamer::DepthMap::STORAGE_UNIT_METER &&
			source_unit == cvmmap_streamer::DepthMap::DEPTH_UNIT_MILLIMETER) {
			return value * 1000.0f;
		}
		return value;
	}

} // namespace

struct McapBackendImpl {
	struct DecoderState {
		const AVCodec *codec{nullptr};
		AVCodecContext *context{nullptr};
		AVFrame *frame{nullptr};
		AVPacket *packet{nullptr};
		SwsContext *scaler{nullptr};
		AVCodecID codec_id{AV_CODEC_ID_NONE};
		int source_width{0};
		int source_height{0};
		AVPixelFormat source_format{AV_PIX_FMT_NONE};
		std::vector<uint8_t> bgr_buffer{};

		void close() {
			if (packet != nullptr) {
				av_packet_free(&packet);
			}
			if (frame != nullptr) {
				av_frame_free(&frame);
			}
			if (context != nullptr) {
				avcodec_free_context(&context);
			}
			if (scaler != nullptr) {
				sws_freeContext(scaler);
				scaler = nullptr;
			}
			codec         = nullptr;
			codec_id      = AV_CODEC_ID_NONE;
			source_width  = 0;
			source_height = 0;
			source_format = AV_PIX_FMT_NONE;
			bgr_buffer.clear();
		}
	};

	app::McapConfig mcap_config;
	app::VideoConfig video_config;
	std::jthread worker_thread;
	on_metadata_fn_t _on_metadata{nullptr};
	on_frame_fn_t _on_frame{nullptr};
	on_body_tracking_fn_t _on_body_tracking{nullptr};
	on_error_fn_t _on_error{nullptr};
	std::mutex state_mutex{};
	std::condition_variable_any state_cv{};
	frame_metadata_t metadata{};
	std::vector<VideoSample> video_samples{};
	std::unordered_map<uint64_t, DepthSample> depth_by_timestamp{};
	std::vector<BodySample> body_samples{};
	std::vector<size_t> keyframe_indices{};
	size_t current_video_index{0};
	size_t next_video_index{0};
	size_t next_body_index{0};
	cvmmap::DepthUnit depth_unit_hint{cvmmap::DepthUnit::Unknown};
	DecoderState decoder{};
	bool metadata_emitted{false};
	bool position_changed{false};

	McapBackendImpl(app::McapConfig cfg, app::VideoConfig video_cfg)
		: mcap_config(std::move(cfg)), video_config(video_cfg) {}

	~McapBackendImpl() {
		decoder.close();
	}

	void on_metadata(const frame_metadata_t &metadata_) {
		if (_on_metadata) {
			_on_metadata(metadata_);
		}
	}

	void on_frame(std::span<uint8_t> frame_buffer_, const frame_metadata_t &metadata_) {
		if (_on_frame) {
			_on_frame(frame_buffer_, metadata_);
		}
	}

	void on_body_tracking(const cvmmap::body_tracking_frame_t &frame) {
		if (_on_body_tracking) {
			_on_body_tracking(frame);
		}
	}

	void on_error(error_t error_code, std::string_view message) {
		if (_on_error) {
			_on_error(error_code, message);
		}
	}

	void publish_packet(PublishPacket packet) {
		if (!metadata_emitted) {
			on_metadata(packet.metadata);
			metadata_emitted = true;
		}
		on_frame(
			std::span<uint8_t>(packet.payload.data(), packet.payload.size()),
			packet.metadata);
		for (const auto &body_frame : packet.body_frames) {
			on_body_tracking(body_frame);
		}
	}

	source_info_t GetSourceInfo() {
		std::lock_guard lock(state_mutex);
		source_info_t info{};
		info.source_kind      = cvmmap::SourceKind::Finite;
		info.timestamp_domain = mcap_config.timestamp_domain;
		if (video_config.finite_source_can_seek()) {
			info.flags |= cvmmap::SOURCE_INFO_FLAG_CAN_SEEK;
		}
		if (video_config.finite_source_auto_loops()) {
			info.flags |= cvmmap::SOURCE_INFO_FLAG_AUTO_LOOP;
		}
		if (video_config.finite_source_loop_emits_reset()) {
			info.flags |= cvmmap::SOURCE_INFO_FLAG_LOOP_EMITS_RESET;
		}
		if (!depth_by_timestamp.empty()) {
			info.flags |= cvmmap::SOURCE_INFO_FLAG_HAS_DEPTH;
		}
		if (!body_samples.empty()) {
			info.flags |= cvmmap::SOURCE_INFO_FLAG_HAS_BODY;
		}
		if (!video_samples.empty()) {
			info.timeline_start_ns = video_samples.front().timestamp_ns;
			info.timeline_end_ns   = video_samples.back().timestamp_ns;
			info.duration_ns       = info.timeline_end_ns >= info.timeline_start_ns
										 ? info.timeline_end_ns - info.timeline_start_ns
										 : 0;
		}
		info.current_timestamp_ns = metadata.timestamp_ns;
		info.current_frame_count  = metadata.frame_count;
		return info;
	}

	cvmmap::expected<void, std::string> load_file() {
		mcap::McapReader reader{};
		const auto open_status = reader.open(mcap_config.path);
		if (!open_status.ok()) {
			return cvmmap::unexpected("open MCAP failed: " + open_status.message);
		}

		mcap::ReadMessageOptions options{};
		options.readOrder   = mcap::ReadMessageOptions::ReadOrder::LogTimeOrder;
		options.topicFilter = [this](const std::string_view topic) {
			return topic == mcap_config.video_topic ||
				   topic == mcap_config.depth_topic ||
				   topic == mcap_config.body_topic;
		};

		auto messages = reader.readMessages(
			[](const mcap::Status &) {},
			options);
		for (auto it = messages.begin(); it != messages.end(); ++it) {
			if (it->channel == nullptr) {
				continue;
			}

			const auto payload = std::span<const uint8_t>(
				reinterpret_cast<const uint8_t *>(it->message.data),
				it->message.dataSize);

			if (it->channel->topic == mcap_config.video_topic) {
				foxglove::CompressedVideo video{};
				if (!video.ParseFromArray(it->message.data,
										  static_cast<int>(it->message.dataSize))) {
					reader.close();
					return cvmmap::unexpected("failed to parse foxglove.CompressedVideo payload");
				}
				auto timestamp_ns = proto_timestamp_to_ns(video.timestamp());
				if (timestamp_ns == 0) {
					timestamp_ns = it->message.logTime;
				}
				VideoSample sample{};
				sample.timestamp_ns = timestamp_ns;
				sample.format       = video.format();
				sample.bytes.assign(
					reinterpret_cast<const uint8_t *>(video.data().data()),
					reinterpret_cast<const uint8_t *>(video.data().data()) + video.data().size());
				sample.keyframe =
					detect_keyframe(sample.format, sample.bytes, video_samples.size());
				video_samples.push_back(std::move(sample));
				continue;
			}

			if (it->channel->topic == mcap_config.depth_topic) {
				cvmmap_streamer::DepthMap depth{};
				if (!depth.ParseFromArray(it->message.data,
										  static_cast<int>(it->message.dataSize))) {
					spdlog::warn("failed to parse depth payload at logTime={}", it->message.logTime);
					continue;
				}
				auto timestamp_ns = proto_timestamp_to_ns(depth.timestamp());
				if (timestamp_ns == 0) {
					timestamp_ns = it->message.logTime;
				}
				DepthSample sample{};
				sample.timestamp_ns = timestamp_ns;
				sample.width        = depth.width();
				sample.height       = depth.height();
				sample.source_unit  = depth.source_unit();
				sample.storage_unit = depth.storage_unit();
				sample.encoding     = depth.encoding();
				sample.bytes.assign(
					reinterpret_cast<const uint8_t *>(depth.data().data()),
					reinterpret_cast<const uint8_t *>(depth.data().data()) + depth.data().size());
				depth_by_timestamp[sample.timestamp_ns] = std::move(sample);
				continue;
			}

			if (it->channel->topic == mcap_config.body_topic) {
				if (it->channel->messageEncoding != kBodyEncoding) {
					spdlog::warn(
						"skipping body message with unexpected encoding '{}' on topic '{}'",
						it->channel->messageEncoding,
						it->channel->topic);
					continue;
				}
				auto parsed = cvmmap::parse_body_tracking_message(payload);
				if (!parsed) {
					spdlog::warn("failed to parse body payload at logTime={}: {}",
								 it->message.logTime,
								 parsed.error());
					continue;
				}
				BodySample sample{};
				sample.timestamp_ns = parsed->header.timestamp_ns != 0
										  ? parsed->header.timestamp_ns
										  : parsed->header.sdk_timestamp_ns;
				sample.frame        = std::move(*parsed);
				body_samples.push_back(std::move(sample));
			}
		}
		reader.close();

		std::sort(video_samples.begin(), video_samples.end(),
				  [](const auto &lhs, const auto &rhs) {
					  return lhs.timestamp_ns < rhs.timestamp_ns;
				  });
		std::sort(body_samples.begin(), body_samples.end(),
				  [](const auto &lhs, const auto &rhs) {
					  return lhs.timestamp_ns < rhs.timestamp_ns;
				  });

		if (video_samples.empty()) {
			return cvmmap::unexpected("MCAP file does not contain any video messages on topic '" +
									  mcap_config.video_topic + "'");
		}

		for (size_t index = 0; index < video_samples.size(); ++index) {
			if (video_samples[index].keyframe) {
				keyframe_indices.push_back(index);
			}
		}
		if (keyframe_indices.empty()) {
			keyframe_indices.push_back(0);
			video_samples.front().keyframe = true;
		}

		depth_unit_hint = cvmmap::DepthUnit::Unknown;
		for (const auto &[timestamp, depth] : depth_by_timestamp) {
			(void)timestamp;
			depth_unit_hint = depth_unit_from_proto(depth.source_unit);
			if (depth_unit_hint != cvmmap::DepthUnit::Unknown) {
				break;
			}
		}

		return {};
	}

	cvmmap::expected<void, std::string> open_decoder_for_format(
		const std::string_view format) {
		decoder.close();

		decoder.codec_id = codec_id_from_format(format);
		if (decoder.codec_id == AV_CODEC_ID_NONE) {
			return cvmmap::unexpected("unsupported compressed video format '" +
									  std::string(format) + "'");
		}

		decoder.codec = avcodec_find_decoder(decoder.codec_id);
		if (decoder.codec == nullptr) {
			return cvmmap::unexpected("ffmpeg decoder unavailable for format '" +
									  std::string(format) + "'");
		}

		decoder.context = avcodec_alloc_context3(decoder.codec);
		if (decoder.context == nullptr) {
			return cvmmap::unexpected("failed to allocate ffmpeg decoder context");
		}
		if (avcodec_open2(decoder.context, decoder.codec, nullptr) < 0) {
			return cvmmap::unexpected("failed to open ffmpeg decoder");
		}

		decoder.packet = av_packet_alloc();
		decoder.frame  = av_frame_alloc();
		if (decoder.packet == nullptr || decoder.frame == nullptr) {
			return cvmmap::unexpected("failed to allocate ffmpeg frame/packet");
		}
		return {};
	}

	cvmmap::expected<std::vector<uint8_t>, std::string> decode_packet_locked(
		const VideoSample &sample) {
		if (decoder.context == nullptr) {
			auto opened = open_decoder_for_format(sample.format);
			if (!opened) {
				return cvmmap::unexpected(opened.error());
			}
		}

		av_packet_unref(decoder.packet);
		const auto packet_alloc =
			av_new_packet(decoder.packet, static_cast<int>(sample.bytes.size()));
		if (packet_alloc < 0) {
			return cvmmap::unexpected("av_new_packet failed: " +
									  ffmpeg_error_string(packet_alloc));
		}
		std::memcpy(decoder.packet->data, sample.bytes.data(), sample.bytes.size());

		const auto send_result = avcodec_send_packet(decoder.context, decoder.packet);
		if (send_result < 0) {
			return cvmmap::unexpected("avcodec_send_packet failed: " +
									  ffmpeg_error_string(send_result));
		}

		std::vector<uint8_t> decoded{};
		while (true) {
			const auto receive_result =
				avcodec_receive_frame(decoder.context, decoder.frame);
			if (receive_result == AVERROR(EAGAIN) ||
				receive_result == AVERROR_EOF) {
				break;
			}
			if (receive_result < 0) {
				return cvmmap::unexpected("avcodec_receive_frame failed: " +
										  ffmpeg_error_string(receive_result));
			}

			const auto width  = decoder.frame->width;
			const auto height = decoder.frame->height;
			const auto format = static_cast<AVPixelFormat>(decoder.frame->format);
			if (width <= 0 || height <= 0) {
				return cvmmap::unexpected("decoded frame has invalid dimensions");
			}

			if (decoder.scaler == nullptr ||
				decoder.source_width != width ||
				decoder.source_height != height ||
				decoder.source_format != format) {
				decoder.scaler = sws_getCachedContext(
					decoder.scaler,
					width,
					height,
					format,
					width,
					height,
					AV_PIX_FMT_BGR24,
					SWS_BILINEAR,
					nullptr,
					nullptr,
					nullptr);
				if (decoder.scaler == nullptr) {
					return cvmmap::unexpected("failed to create ffmpeg scaler");
				}
				decoder.source_width  = width;
				decoder.source_height = height;
				decoder.source_format = format;
			}

			const auto buffer_size =
				static_cast<size_t>(width) * static_cast<size_t>(height) * 3ull;
			decoder.bgr_buffer.resize(buffer_size);

			uint8_t *dst_data[4] = {decoder.bgr_buffer.data(), nullptr, nullptr, nullptr};
			int dst_linesize[4]  = {width * 3, 0, 0, 0};
			sws_scale(
				decoder.scaler,
				decoder.frame->data,
				decoder.frame->linesize,
				0,
				height,
				dst_data,
				dst_linesize);

			decoded = decoder.bgr_buffer;
		}

		if (decoded.empty()) {
			return cvmmap::unexpected("decoder produced no output frame");
		}
		return decoded;
	}

	cvmmap::expected<std::vector<float>, std::string> decode_depth_locked(
		const DepthSample &sample) {
		if (sample.encoding == cvmmap_streamer::DepthMap::RVL_U16_LOSSLESS) {
			auto decoded = rvl::try_decompress_image(sample.bytes);
			if (!decoded) {
				return cvmmap::unexpected(decoded.error());
			}
			if (decoded->rows != sample.height || decoded->cols != sample.width) {
				return cvmmap::unexpected("decoded uint16 depth image size mismatch");
			}
			std::vector<float> pixels(decoded->pixels.size());
			for (size_t index = 0; index < decoded->pixels.size(); ++index) {
				pixels[index] = convert_depth_sample(
					static_cast<float>(decoded->pixels[index]),
					sample.storage_unit,
					sample.source_unit);
			}
			return pixels;
		}

		if (sample.encoding == cvmmap_streamer::DepthMap::RVL_F32) {
			auto decoded = rvl::try_decompress_float_image(sample.bytes);
			if (!decoded) {
				return cvmmap::unexpected(decoded.error());
			}
			if (decoded->rows != sample.height || decoded->cols != sample.width) {
				return cvmmap::unexpected("decoded float depth image size mismatch");
			}
			std::vector<float> pixels(decoded->pixels.size());
			for (size_t index = 0; index < decoded->pixels.size(); ++index) {
				pixels[index] = convert_depth_sample(
					decoded->pixels[index],
					sample.storage_unit,
					sample.source_unit);
			}
			return pixels;
		}

		return cvmmap::unexpected("unsupported depth encoding");
	}

	size_t seek_decode_start_index(size_t target_index) const {
		auto it = std::upper_bound(keyframe_indices.begin(), keyframe_indices.end(), target_index);
		if (it == keyframe_indices.begin()) {
			return 0;
		}
		--it;
		return *it;
	}

	std::vector<cvmmap::body_tracking_frame_t> latest_body_for_timestamp_locked(
		const uint64_t timestamp_ns,
		const uint32_t frame_count) {
		std::vector<cvmmap::body_tracking_frame_t> out{};
		const auto it = std::upper_bound(
			body_samples.begin(),
			body_samples.end(),
			timestamp_ns,
			[](const uint64_t ts, const BodySample &sample) {
				return ts < sample.timestamp_ns;
			});
		next_body_index = static_cast<size_t>(std::distance(body_samples.begin(), it));
		if (it == body_samples.begin()) {
			return out;
		}
		auto frame               = std::prev(it)->frame;
		frame.header.frame_count = frame_count;
		out.push_back(std::move(frame));
		return out;
	}

	std::vector<cvmmap::body_tracking_frame_t> pending_bodies_for_timestamp_locked(
		const uint64_t timestamp_ns,
		const uint32_t frame_count) {
		std::vector<cvmmap::body_tracking_frame_t> out{};
		while (next_body_index < body_samples.size() &&
			   body_samples[next_body_index].timestamp_ns <= timestamp_ns) {
			auto frame               = body_samples[next_body_index].frame;
			frame.header.frame_count = frame_count;
			out.push_back(std::move(frame));
			++next_body_index;
		}
		return out;
	}

	cvmmap::expected<PublishPacket, std::string> build_publish_packet_locked(
		const size_t video_index,
		const uint32_t local_frame_count,
		const bool republish_latest_body) {
		if (video_index >= video_samples.size()) {
			return cvmmap::unexpected("video index out of range");
		}

		auto decoded_frame = decode_packet_locked(video_samples[video_index]);
		if (!decoded_frame) {
			return cvmmap::unexpected(decoded_frame.error());
		}

		PublishPacket packet{};
		packet.metadata.ensure_magic();
		packet.metadata.frame_count  = local_frame_count;
		packet.metadata.timestamp_ns = video_samples[video_index].timestamp_ns;
		packet.metadata.info         = frame_info_t{
			.width        = static_cast<uint16_t>(decoder.source_width),
			.height       = static_cast<uint16_t>(decoder.source_height),
			.channels     = 3,
			.depth        = Depth::U8,
			.pixel_format = PixelFormat::BGR,
			.buffer_size  = static_cast<uint32_t>(decoded_frame->size()),
		};
		packet.payload = std::move(*decoded_frame);

		auto depth_it = depth_by_timestamp.find(packet.metadata.timestamp_ns);
		if (depth_it != depth_by_timestamp.end()) {
			auto depth_pixels = decode_depth_locked(depth_it->second);
			if (!depth_pixels) {
				spdlog::warn("failed to decode MCAP depth sample at {}: {}",
							 packet.metadata.timestamp_ns,
							 depth_pixels.error());
			} else if (
				depth_it->second.width == packet.metadata.info.width &&
				depth_it->second.height == packet.metadata.info.height) {
				const auto *depth_bytes =
					reinterpret_cast<const uint8_t *>(depth_pixels->data());
				packet.payload.insert(
					packet.payload.end(),
					depth_bytes,
					depth_bytes + depth_pixels->size() * sizeof(float));
				const auto depth_unit = depth_unit_from_proto(depth_it->second.source_unit);
				if (depth_unit != cvmmap::DepthUnit::Unknown) {
					depth_unit_hint = depth_unit;
				}
			} else {
				spdlog::warn(
					"ignoring MCAP depth sample at {} because dimensions {}x{} do not match video {}x{}",
					packet.metadata.timestamp_ns,
					depth_it->second.width,
					depth_it->second.height,
					packet.metadata.info.width,
					packet.metadata.info.height);
			}
		}

		if (republish_latest_body) {
			packet.body_frames = latest_body_for_timestamp_locked(
				packet.metadata.timestamp_ns,
				local_frame_count);
		} else {
			packet.body_frames = pending_bodies_for_timestamp_locked(
				packet.metadata.timestamp_ns,
				local_frame_count);
		}

		metadata            = packet.metadata;
		current_video_index = video_index;
		next_video_index    = video_index + 1;
		return packet;
	}

	cvmmap::expected<PublishPacket, std::string> seek_to_index_locked(
		const size_t target_index,
		const uint32_t local_frame_count) {
		const auto start_index = seek_decode_start_index(target_index);
		auto reopen            = open_decoder_for_format(video_samples[target_index].format);
		if (!reopen) {
			return cvmmap::unexpected(reopen.error());
		}

		PublishPacket packet{};
		for (size_t index = start_index; index <= target_index; ++index) {
			auto built = build_publish_packet_locked(
				index,
				index == target_index ? local_frame_count : local_frame_count,
				index == target_index);
			if (!built) {
				return cvmmap::unexpected(built.error());
			}
			if (index == target_index) {
				packet = std::move(*built);
			}
		}
		return packet;
	}

	cvmmap::expected<seek_result_t, error_t> seek_timestamp_locked(
		const uint64_t timestamp_ns,
		const bool notify_worker,
		PublishPacket *packet_out) {
		if (!video_config.finite_source_can_seek()) {
			return cvmmap::unexpected(-EOPNOTSUPP);
		}
		if (video_samples.empty()) {
			return cvmmap::unexpected(-EINVAL);
		}
		if (timestamp_ns < video_samples.front().timestamp_ns ||
			timestamp_ns > video_samples.back().timestamp_ns) {
			return cvmmap::unexpected(-ERANGE);
		}

		const auto it = std::lower_bound(
			video_samples.begin(),
			video_samples.end(),
			timestamp_ns,
			[](const VideoSample &sample, const uint64_t ts) {
				return sample.timestamp_ns < ts;
			});
		if (it == video_samples.end()) {
			return cvmmap::unexpected(-ERANGE);
		}

		const auto target_index =
			static_cast<size_t>(std::distance(video_samples.begin(), it));
		auto packet = seek_to_index_locked(target_index, 0);
		if (!packet) {
			spdlog::error("bad MCAP seek decode: {}", packet.error());
			return cvmmap::unexpected(-EIO);
		}

		if (notify_worker) {
			position_changed = true;
		}
		if (packet_out != nullptr) {
			*packet_out = std::move(*packet);
		}
		return seek_result_t{
			.requested_timestamp_ns = timestamp_ns,
			.landed_timestamp_ns    = packet->metadata.timestamp_ns,
			.landed_frame_count     = packet->metadata.frame_count,
			.exact_match            = (packet->metadata.timestamp_ns == timestamp_ns),
		};
	}

	void Init() {
		auto loaded = load_file();
		if (!loaded) {
			spdlog::error("initialize MCAP backend: {}", loaded.error());
			on_error(-ENOENT, loaded.error());
			return;
		}

		metadata.ensure_magic();
		metadata.frame_count  = 0;
		metadata.timestamp_ns = video_samples.front().timestamp_ns;

		PublishPacket initial_packet{};
		{
			std::lock_guard lock(state_mutex);
			auto packet = seek_to_index_locked(0, 0);
			if (!packet) {
				on_error(-EIO, "failed to decode initial MCAP frame");
				return;
			}
			initial_packet = std::move(*packet);
		}

		spdlog::info(
			"MCAP backend initialized: path='{}' video_messages={} depth_messages={} body_messages={}",
			mcap_config.path,
			video_samples.size(),
			depth_by_timestamp.size(),
			body_samples.size());

		publish_packet(initial_packet);

		worker_thread = std::jthread([this](std::stop_token stop_token) {
			worker_loop(stop_token);
		});
	}

	void worker_loop(std::stop_token stop_token) {
		while (!stop_token.stop_requested()) {
			PublishPacket packet{};
			{
				std::unique_lock lock(state_mutex);
				if (next_video_index >= video_samples.size()) {
					if (video_config.finite_source_loops_silently()) {
						auto looped = seek_to_index_locked(0, 0);
						if (!looped) {
							lock.unlock();
							on_error(-EIO, "failed to loop MCAP replay");
							break;
						}
						packet = std::move(*looped);
						lock.unlock();
						publish_packet(packet);
						continue;
					}

					lock.unlock();
					on_error(ERR_EOS, "EOF");
					break;
				}

				const auto previous_timestamp =
					video_samples[current_video_index].timestamp_ns;
				const auto next_timestamp =
					video_samples[next_video_index].timestamp_ns;
				const auto sleep_ns = next_timestamp > previous_timestamp
										  ? (next_timestamp - previous_timestamp)
										  : 0;
				if (sleep_ns > 0) {
					const auto interrupted = state_cv.wait_for(
						lock,
						std::chrono::nanoseconds(sleep_ns),
						[this, &stop_token] {
							return position_changed || stop_token.stop_requested();
						});
					if (stop_token.stop_requested()) {
						break;
					}
					if (interrupted && position_changed) {
						position_changed = false;
						continue;
					}
				}

				auto built = build_publish_packet_locked(
					next_video_index,
					metadata.frame_count + 1,
					false);
				if (!built) {
					lock.unlock();
					on_error(-EIO, built.error());
					break;
				}
				packet = std::move(*built);
			}

			publish_packet(packet);
		}
	}

	void Shutdown() {
		{
			std::lock_guard lock(state_mutex);
			position_changed = true;
		}
		state_cv.notify_all();
		if (worker_thread.joinable()) {
			worker_thread.request_stop();
			worker_thread.join();
		}
	}

	void SetOnMetadata(on_metadata_fn_t on_metadata_) {
		_on_metadata = std::move(on_metadata_);
	}

	void SetOnFrame(on_frame_fn_t on_frame_) {
		_on_frame = std::move(on_frame_);
	}

	void SetOnBodyTracking(on_body_tracking_fn_t on_body_tracking_) {
		_on_body_tracking = std::move(on_body_tracking_);
	}

	void SetOnError(on_error_fn_t on_error_) {
		_on_error = std::move(on_error_);
	}

	cvmmap::expected<seek_result_t, error_t> SeekTimestampNs(uint64_t timestamp_ns) {
		PublishPacket packet{};
		cvmmap::expected<seek_result_t, error_t> result = cvmmap::unexpected(-EIO);
		{
			std::lock_guard lock(state_mutex);
			result = seek_timestamp_locked(timestamp_ns, true, &packet);
		}
		if (result) {
			publish_packet(packet);
			state_cv.notify_all();
		}
		return result;
	}

	error_t ResetFrameCount() {
		PublishPacket packet{};
		{
			std::lock_guard lock(state_mutex);
			auto result = seek_to_index_locked(0, 0);
			if (!result) {
				spdlog::error("bad MCAP replay reset: {}", result.error());
				return -EIO;
			}
			packet           = std::move(*result);
			position_changed = true;
		}
		publish_packet(packet);
		state_cv.notify_all();
		return ERR_OK;
	}
};

McapBackend::McapBackend(app::McapConfig mcap_config,
						 const app::VideoConfig &video_config)
	: impl(std::make_unique<McapBackendImpl>(
		  std::move(mcap_config), video_config)) {}

McapBackend::~McapBackend() = default;

void McapBackend::Init() {
	impl->Init();
}

void McapBackend::Shutdown() {
	impl->Shutdown();
}

void McapBackend::SetOnMetadata(on_metadata_fn_t on_metadata) {
	impl->SetOnMetadata(std::move(on_metadata));
}

void McapBackend::SetOnFrame(on_frame_fn_t on_frame) {
	impl->SetOnFrame(std::move(on_frame));
}

void McapBackend::SetOnBodyTracking(on_body_tracking_fn_t on_body_tracking) {
	impl->SetOnBodyTracking(std::move(on_body_tracking));
}

void McapBackend::SetOnError(on_error_fn_t on_error) {
	impl->SetOnError(std::move(on_error));
}

source_info_t McapBackend::GetSourceInfo() {
	return impl->GetSourceInfo();
}

cvmmap::expected<seek_result_t, error_t> McapBackend::SeekTimestampNs(
	uint64_t timestamp_ns) {
	return impl->SeekTimestampNs(timestamp_ns);
}

error_t McapBackend::ResetFrameCount() {
	return impl->ResetFrameCount();
}

cvmmap::expected<uint64_t, std::string> ProbeMcapStartTimestampNs(
	const app::McapConfig &mcap_config) {
	return probe_mcap_start_timestamp_ns(mcap_config.path, mcap_config.video_topic);
}

} // namespace app::backends
