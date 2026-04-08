#include <string_view>
#include <iostream>

#ifndef GIT_REV
#define GIT_REV "N/A"
#endif
#ifndef GIT_TAG
#define GIT_TAG ""
#endif
#ifndef GIT_BRANCH
#define GIT_BRANCH "N/A"
#endif
#ifndef COMPILE_TIMESTAMP
#define COMPILE_TIMESTAMP "1970-01-01T00:00:00"
#endif
#ifndef COMPILE_TIMESTAMP_LOCAL
#define COMPILE_TIMESTAMP_LOCAL "1970-01-01T00:00:00"
#endif
#ifndef COMPILE_TIMESTAMP_UTC
#define COMPILE_TIMESTAMP_UTC "1970-01-01T00:00:00Z"
#endif
#ifndef GIT_DIFF
#define GIT_DIFF ""
#endif

#define STRR(X) #X
#define STR(X)  STRR(X)

constexpr std::string_view trim(std::string_view s) {
	s.remove_prefix(std::min(s.find_first_not_of(" \t\r\v\n"), s.size()));
	s.remove_suffix(std::min(s.size() - s.find_last_not_of(" \t\r\v\n") - 1, s.size()));
	return s;
}

namespace app::version {
constexpr auto revision_value                = STR(GIT_REV);
constexpr auto tag_value                     = STR(GIT_TAG);
constexpr auto trim_tag_value                = trim(tag_value);
constexpr auto branch_value                  = STR(GIT_BRANCH);
constexpr auto diff_value                    = STR(GIT_DIFF);
constexpr auto compile_timestamp_local_value = STR(COMPILE_TIMESTAMP_LOCAL);
constexpr auto compile_timestamp_utc_value   = STR(COMPILE_TIMESTAMP_UTC);

std::string_view revision() {
	return revision_value;
}

std::string_view tag() {
	return trim_tag_value;
}

std::string_view branch() {
	return branch_value;
}

std::string_view compile_timestamp_utc() {
	return compile_timestamp_utc_value;
}

void print_version() {
	if constexpr (constexpr auto t = std::string_view{tag_value}; t.empty()) {
		std::cout << "version: " << revision_value << diff_value << " (" << branch_value << ")\n";
	} else {
		std::cout << "version: " << tag_value << " (" << revision_value << diff_value << ")\n";
	}
	std::cout << "built: " << compile_timestamp_local_value << " (local), " << compile_timestamp_utc_value << " (UTC)\n";
}
}
