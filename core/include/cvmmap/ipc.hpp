#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace cvmmap {

constexpr size_t LABEL_LEN_MAX = 24;
constexpr size_t SHM_PAYLOAD_OFFSET = 256;

constexpr uint8_t FRAME_TOPIC_MAGIC = 0x7d;
constexpr uint8_t BODY_TRACKING_MAGIC = 0x62;

constexpr uint8_t VERSION_MAJOR = 1;
constexpr uint8_t VERSION_MINOR = 0;

constexpr uint8_t FRAME_METADATA_V1_MAJOR = 1;
constexpr uint8_t FRAME_METADATA_V2_MAJOR = 2;
constexpr uint8_t FRAME_METADATA_V2_MINOR_BASE = 0;
constexpr uint8_t FRAME_METADATA_V2_MINOR_ENCODED_AU = 1;

enum class PixelFormat : uint8_t {
	RGB = 0,
	BGR,
	RGBA,
	BGRA,
	GRAY,
	YUV,
	YUYV,
};

enum class Depth : uint8_t {
	U8 = 0,
	S8 = 1,
	U16 = 2,
	S16 = 3,
	S32 = 4,
	F32 = 5,
	F64 = 6,
	F16 = 7,
};

enum class FramePlaneType : uint8_t {
	Left = 0,
	Depth = 1,
	Confidence = 2,
	EncodedAccessUnit = 3,
};

enum class EncodedCodec : uint8_t {
	Unknown = 0,
	H264 = 1,
	H265 = 2,
};

enum class EncodedBitstreamFormat : uint8_t {
	Unknown = 0,
	AnnexB = 1,
};

constexpr uint16_t FRAME_METADATA_V2_ENCODED_FLAG_KEYFRAME = 0x0001u;

enum class DepthUnit : uint8_t {
	Unknown = 0,
	Millimeter = 1,
	Meter = 2,
};

enum class ControlErrorCode : uint8_t {
	Ok = 0,
	UnknownCmd = 1,
	Error = 2,
	Unsupported = 3,
	InvalidPayload = 4,
	OutOfRange = 5,
	Timeout = 6,
};

enum class ModuleStatus : uint8_t {
	Unknown = 0,
	Online = 1,
	Offline = 2,
	StreamReset = 3,
};

enum class SourceKind : uint8_t {
	Unknown = 0,
	Live = 1,
	Finite = 2,
};

enum class TimestampDomain : uint8_t {
	Unknown = 0,
	UnixEpochNs = 1,
	MediaTimeNs = 2,
};

constexpr uint32_t SOURCE_INFO_FLAG_AUTO_LOOP = 0x00000001u;
constexpr uint32_t SOURCE_INFO_FLAG_HAS_DEPTH = 0x00000002u;
constexpr uint32_t SOURCE_INFO_FLAG_HAS_BODY = 0x00000004u;
constexpr uint32_t SOURCE_INFO_FLAG_CAN_RECORD = 0x00000008u;
constexpr uint32_t SOURCE_INFO_FLAG_LOOP_EMITS_RESET = 0x00000010u;

enum class RecordingFormat : uint8_t {
	Unknown = 0,
	Svo = 1,
	Mcap = 2,
};

enum class CameraControlSetting : uint8_t {
	Unknown = 0,
	Exposure = 1,
	Gain = 2,
	AecAgc = 3,
	WhitebalanceTemperature = 4,
	WhitebalanceAuto = 5,
	LedStatus = 6,
	ExposureTime = 7,
	AnalogGain = 8,
	DigitalGain = 9,
	AutoExposureTimeRange = 10,
	AutoAnalogGainRange = 11,
	AutoDigitalGainRange = 12,
};

enum class CameraControlValueKind : uint8_t {
	Unknown = 0,
	Single = 1,
	Range = 2,
};

enum class CameraControlWriteMode : uint8_t {
	Unknown = 0,
	Manual = 1,
	Auto = 2,
};

constexpr uint16_t RECORDING_STATUS_FLAG_CAN_RECORD = 0x0001u;
constexpr uint16_t RECORDING_STATUS_FLAG_IS_RECORDING = 0x0002u;
constexpr uint16_t RECORDING_STATUS_FLAG_IS_PAUSED = 0x0004u;
constexpr uint16_t RECORDING_STATUS_FLAG_LAST_FRAME_OK = 0x0008u;

enum class BodyTrackingModel : uint8_t {
	HumanBodyFast = 0,
	HumanBodyMedium = 1,
	HumanBodyAccurate = 2,
};

enum class BodyFormat : uint8_t {
	Body18 = 0,
	Body34 = 1,
	Body38 = 2,
};

enum class BodyKeypointSelection : uint8_t {
	Full = 0,
	UpperBody = 1,
};

enum class InferencePrecision : uint8_t {
	FP32 = 0,
	FP16 = 1,
	INT8 = 2,
};

enum class BodyCoordinateSystem : uint8_t {
	Unknown = 0,
	Image = 1,
	RightHandedYUp = 2,
};

enum class BodyReferenceFrame : uint8_t {
	Unknown = 0,
	Camera = 1,
	World = 2,
};

enum class ObjectTrackingState : uint8_t {
	Off = 0,
	Ok = 1,
	Searching = 2,
	Terminate = 3,
};

enum class ObjectActionState : uint8_t {
	Idle = 0,
	Moving = 1,
};

constexpr uint16_t BODY_TRACKING_FLAG_IS_NEW = 0x0001;
constexpr uint16_t BODY_TRACKING_FLAG_IS_TRACKED = 0x0002;
constexpr uint16_t BODY_TRACKING_FLAG_BODY_FITTING_ENABLED = 0x0004;
constexpr uint16_t BODY_TRACKING_FLAG_REDUCED_PRECISION_REQUESTED = 0x0008;
constexpr uint16_t BODY_TRACKING_FLAG_FLOOR_AS_ORIGIN = 0x0010;

constexpr uint16_t BODY_TRACKING_BODY_FLAG_HAS_LOCAL_JOINTS = 0x0001;
constexpr uint16_t BODY_TRACKING_BODY_FLAG_HAS_ROOT_ORIENTATION = 0x0002;

constexpr size_t BODY_KEYPOINT_CAPACITY = 38;
constexpr size_t BODY_BOX2D_POINTS = 4;
constexpr size_t BODY_BOX3D_POINTS = 8;

constexpr int size_of(Depth depth) {
	switch (depth) {
	case Depth::U8:
	case Depth::S8:
		return 1;
	case Depth::U16:
	case Depth::S16:
	case Depth::F16:
		return 2;
	case Depth::S32:
	case Depth::F32:
		return 4;
	case Depth::F64:
		return 8;
	default:
		return 0;
	}
}

struct frame_info_t {
	uint16_t width;
	uint16_t height;
	uint8_t channels;
	Depth depth;
	PixelFormat pixel_format;
	uint8_t _reserved_0[1];
	uint32_t buffer_size;

	[[nodiscard]]
	int pixelSize() const {
		return size_of(depth) * channels;
	}
};
static_assert(alignof(frame_info_t) == 4, "frame_info_t must be 4-byte aligned");
static_assert(sizeof(frame_info_t) == 12, "frame_info_t must be 12 bytes");

struct frame_metadata_t {
	static constexpr auto CV_MMAP_MAGIC =
		std::array<uint8_t, 8>{'C', 'V', '-', 'M', 'M', 'A', 'P', '\0'};

	[[nodiscard]]
	std::string_view label() const {
		return std::string_view{reinterpret_cast<const char *>(magic)};
	}

	uint8_t magic[CV_MMAP_MAGIC.size()];
	uint8_t versions_major;
	uint8_t versions_minor;
	uint8_t _reserved_0[2];
	uint32_t frame_count;
	uint64_t timestamp_ns;
	frame_info_t info;
};
static_assert(sizeof(frame_metadata_t) < SHM_PAYLOAD_OFFSET,
			  "frame_metadata_t size must be less than SHM_PAYLOAD_OFFSET");
static_assert(alignof(frame_metadata_t) == 8,
			  "frame_metadata_t must be 8-byte aligned");

struct frame_plane_descriptor_v2_t {
	FramePlaneType plane_type;
	PixelFormat pixel_format;
	Depth depth;
	uint8_t _reserved_0{0};
	uint32_t width;
	uint32_t height;
	uint32_t stride_bytes;
	uint32_t offset_bytes;
	uint32_t size_bytes;
};
static_assert(sizeof(frame_plane_descriptor_v2_t) == 24,
			  "frame_plane_descriptor_v2_t must be 24 bytes");

struct frame_metadata_v2_header_t {
	uint8_t magic[frame_metadata_t::CV_MMAP_MAGIC.size()];
	uint8_t versions_major;
	uint8_t versions_minor;
	uint16_t flags;
	uint32_t frame_id;
	uint64_t capture_ts_ns;
	uint64_t publish_seq;
	uint8_t plane_count;
	uint8_t plane_presence_mask;
	uint16_t plane_descriptors_offset;
	uint16_t plane_descriptor_size;
	uint16_t plane_descriptor_capacity;
	uint32_t payload_size_bytes;
	DepthUnit depth_unit;
	uint8_t reserved_0[19];

	[[nodiscard]]
	bool uses_sparse_plane_mask_semantics() const {
		return versions_minor >= FRAME_METADATA_V2_MINOR_ENCODED_AU;
	}
};
static_assert(sizeof(frame_metadata_v2_header_t) == 64,
			  "frame_metadata_v2_header_t must be 64 bytes");
static_assert(offsetof(frame_metadata_v2_header_t, depth_unit) == 0x2C,
			  "frame_metadata_v2_header_t::depth_unit must be at offset 0x2C");
static_assert(offsetof(frame_metadata_v2_header_t, reserved_0) == 0x2D,
			  "frame_metadata_v2_header_t::reserved_0 must start at offset 0x2D");

#pragma pack(push, 1)
struct frame_metadata_v2_encoded_extension_t {
	EncodedCodec encoded_codec{EncodedCodec::Unknown};
	EncodedBitstreamFormat encoded_bitstream_format{EncodedBitstreamFormat::Unknown};
	uint16_t encoded_flags{0};
	uint16_t encoded_frame_rate_num{0};
	uint16_t encoded_frame_rate_den{0};
	uint64_t encoded_stream_pts_ns{0};
	uint8_t reserved_0[3]{};
};
#pragma pack(pop)
static_assert(sizeof(frame_metadata_v2_encoded_extension_t) == 19,
			  "frame_metadata_v2_encoded_extension_t must fit reserved_0");

struct frame_metadata_v2_t {
	frame_metadata_v2_header_t header;
	std::array<frame_plane_descriptor_v2_t, 4> descriptors;
	uint8_t trailing_padding_to_payload[96];
};
static_assert(sizeof(frame_metadata_v2_t) == SHM_PAYLOAD_OFFSET,
			  "frame_metadata_v2_t must be 256 bytes");

struct frame_planes_view_t {
	std::span<const uint8_t> left{};
	DepthUnit depth_unit{DepthUnit::Unknown};
	std::optional<frame_info_t> depth_info{};
	std::span<const uint8_t> depth{};
	std::optional<frame_info_t> confidence_info{};
	std::span<const uint8_t> confidence{};
	EncodedCodec encoded_codec{EncodedCodec::Unknown};
	EncodedBitstreamFormat encoded_bitstream_format{EncodedBitstreamFormat::Unknown};
	uint16_t encoded_flags{0};
	uint16_t encoded_frame_rate_num{0};
	uint16_t encoded_frame_rate_den{0};
	uint64_t encoded_stream_pts_ns{0};
	std::span<const uint8_t> encoded_access_unit{};
};

struct sync_message_t {
	[[nodiscard]]
	std::string_view label() const {
		return std::string_view{reinterpret_cast<const char *>(_label)};
	}

	[[nodiscard]]
	static constexpr size_t size() {
		return sizeof(sync_message_t);
	}

	uint8_t _magic{FRAME_TOPIC_MAGIC};
	uint8_t _reserved_0[1];
	uint8_t versions_major{VERSION_MAJOR};
	uint8_t versions_minor{VERSION_MINOR};
	uint32_t frame_count;
	uint8_t _reserved_1[8];
	uint64_t timestamp_ns;
	uint8_t _label[LABEL_LEN_MAX];
};
static_assert(alignof(sync_message_t) == 8,
			  "sync_message_t must be 8-byte aligned");
static_assert(sizeof(sync_message_t) == 48, "sync_message_t must be 48 bytes");
struct body_tracking_message_header_t {
	[[nodiscard]]
	std::string_view label() const {
		return std::string_view{reinterpret_cast<const char *>(_label)};
	}

	[[nodiscard]]
	BodyCoordinateSystem coordinate_system() const {
		return static_cast<BodyCoordinateSystem>(coordinate_system_code);
	}

	void set_coordinate_system(const BodyCoordinateSystem value) {
		coordinate_system_code = static_cast<uint8_t>(value);
	}

	[[nodiscard]]
	BodyReferenceFrame reference_frame() const {
		return static_cast<BodyReferenceFrame>(reference_frame_code);
	}

	void set_reference_frame(const BodyReferenceFrame value) {
		reference_frame_code = static_cast<uint8_t>(value);
	}

	[[nodiscard]]
	bool floor_as_origin() const {
		return (flags & BODY_TRACKING_FLAG_FLOOR_AS_ORIGIN) != 0;
	}

	void set_floor_as_origin(const bool enabled) {
		if (enabled) {
			flags |= BODY_TRACKING_FLAG_FLOOR_AS_ORIGIN;
		} else {
			flags &= static_cast<uint16_t>(~BODY_TRACKING_FLAG_FLOOR_AS_ORIGIN);
		}
	}

	uint8_t _magic{BODY_TRACKING_MAGIC};
	uint8_t coordinate_system_code{0};
	uint8_t versions_major{VERSION_MAJOR};
	uint8_t versions_minor{VERSION_MINOR};
	uint32_t frame_count{0};
	uint64_t timestamp_ns{0};
	uint64_t sdk_timestamp_ns{0};
	uint16_t body_count{0};
	uint16_t body_record_size{0};
	BodyFormat body_format{BodyFormat::Body18};
	BodyKeypointSelection body_selection{BodyKeypointSelection::Full};
	BodyTrackingModel detection_model{BodyTrackingModel::HumanBodyAccurate};
	InferencePrecision inference_precision{InferencePrecision::FP32};
	uint16_t flags{0};
	uint8_t reference_frame_code{0};
	uint8_t body_header_reserved_0{0};
	uint32_t payload_size_bytes{0};
	uint8_t _label[LABEL_LEN_MAX]{};
};
static_assert(sizeof(body_tracking_message_header_t) == 64,
			  "body_tracking_message_header_t must be 64 bytes");
static_assert(offsetof(body_tracking_message_header_t, coordinate_system_code) == 0x01,
			  "coordinate_system_code offset must be 0x01");
static_assert(offsetof(body_tracking_message_header_t, reference_frame_code) == 0x22,
			  "reference_frame_code offset must be 0x22");
static_assert(offsetof(body_tracking_message_header_t, body_header_reserved_0) == 0x23,
			  "body_header_reserved_0 offset must be 0x23");

struct body_tracking_body_t {
	int32_t id{-1};
	ObjectTrackingState tracking_state{ObjectTrackingState::Off};
	ObjectActionState action_state{ObjectActionState::Idle};
	uint8_t _reserved_0[2]{};
	float confidence{0.0f};
	std::array<float, 3> position{};
	std::array<float, 3> velocity{};
	std::array<float, 6> position_covariance{};
	std::array<std::array<float, 2>, BODY_BOX2D_POINTS> bounding_box_2d{};
	std::array<std::array<float, 3>, BODY_BOX3D_POINTS> bounding_box_3d{};
	std::array<float, 3> dimensions{};
	std::array<std::array<float, 2>, BODY_KEYPOINT_CAPACITY> keypoint_2d{};
	std::array<std::array<float, 3>, BODY_KEYPOINT_CAPACITY> keypoint_3d{};
	std::array<float, BODY_KEYPOINT_CAPACITY> keypoint_confidence{};
	std::array<std::array<float, 6>, BODY_KEYPOINT_CAPACITY> keypoint_covariance{};
	std::array<std::array<float, 2>, BODY_BOX2D_POINTS> head_bounding_box_2d{};
	std::array<std::array<float, 3>, BODY_BOX3D_POINTS> head_bounding_box_3d{};
	std::array<float, 3> head_position{};
	std::array<std::array<float, 3>, BODY_KEYPOINT_CAPACITY> local_position_per_joint{};
	std::array<std::array<float, 4>, BODY_KEYPOINT_CAPACITY> local_orientation_per_joint{};
	std::array<float, 4> global_root_orientation{};
	uint16_t keypoint_count{0};
	uint16_t flags{0};
};
static_assert(sizeof(body_tracking_body_t) == 3248,
			  "body_tracking_body_t must be 3248 bytes");
struct body_tracking_frame_t {
	body_tracking_message_header_t header{};
	std::vector<body_tracking_body_t> bodies{};
};

} // namespace cvmmap
