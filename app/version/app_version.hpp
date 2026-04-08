#pragma once

#include <string_view>

namespace app::version {
std::string_view revision();
std::string_view tag();
std::string_view branch();
std::string_view compile_timestamp_utc();
void print_version();
}
