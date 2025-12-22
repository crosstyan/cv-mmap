#ifndef D266BFE1_D878_44FE_909C_6632F9055C9A
#define D266BFE1_D878_44FE_909C_6632F9055C9A
#include <string>
#include <span>
#include <cstdint>
#include <format>

namespace app {
/**
 * @brief xxd-style hex dump of a byte buffer
 * @param data the data to dump
 * @param bytes_per_line number of bytes per line (default 16, like xxd)
 * @return formatted string with offset, hex bytes, and ASCII representation
 */
inline std::string hexdump(std::span<const uint8_t> data, size_t bytes_per_line = 16) {
	std::string result;
	for (size_t offset = 0; offset < data.size(); offset += bytes_per_line) {
		// offset
		result += std::format("{:08x}: ", offset);
		// hex bytes
		for (size_t i = 0; i < bytes_per_line; ++i) {
			if (offset + i < data.size()) {
				result += std::format("{:02x}", data[offset + i]);
			} else {
				result += "  ";
			}
			if (i % 2 == 1) {
				result += ' ';
			}
		}
		// ASCII representation
		result += " ";
		for (size_t i = 0; i < bytes_per_line && offset + i < data.size(); ++i) {
			const auto c = static_cast<char>(data[offset + i]);
			result += (c >= 0x20 && c < 0x7f) ? c : '.';
		}
		result += '\n';
	}
	return result;
}
}

#endif /* D266BFE1_D878_44FE_909C_6632F9055C9A */
