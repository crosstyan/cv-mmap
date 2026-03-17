if(NOT DEFINED INPUT_PATH OR NOT DEFINED OUTPUT_PATH OR NOT DEFINED SYMBOL_NAME)
	message(FATAL_ERROR "INPUT_PATH, OUTPUT_PATH, and SYMBOL_NAME are required")
endif()

file(READ "${INPUT_PATH}" INPUT_HEX HEX)
string(REGEX REPLACE "([0-9A-Fa-f][0-9A-Fa-f])" "0x\\1, " INPUT_BYTES "${INPUT_HEX}")
string(REGEX REPLACE "((0x[0-9A-Fa-f][0-9A-Fa-f], ){16})" "\\1\n" INPUT_BYTES "${INPUT_BYTES}")

set(HEADER_CONTENT
"#pragma once
#include <cstddef>

namespace app::backends {

inline constexpr unsigned char ${SYMBOL_NAME}[] = {
${INPUT_BYTES}
};

inline constexpr std::size_t ${SYMBOL_NAME}_size = sizeof(${SYMBOL_NAME});

} // namespace app::backends
")

file(WRITE "${OUTPUT_PATH}" "${HEADER_CONTENT}")
