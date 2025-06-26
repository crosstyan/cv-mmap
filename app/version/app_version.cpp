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

#define STRR(X) #X
#define STR(X)  STRR(X)

constexpr std::string_view trim(std::string_view s) {
	s.remove_prefix(std::min(s.find_first_not_of(" \t\r\v\n"), s.size()));
	s.remove_suffix(std::min(s.size() - s.find_last_not_of(" \t\r\v\n") - 1, s.size()));
	return s;
}

namespace app::version {
constexpr auto revision          = STR(GIT_REV);
constexpr auto tag               = STR(GIT_TAG);
constexpr auto trim_tag          = trim(tag);
constexpr auto branch            = STR(GIT_BRANCH);
constexpr auto compile_timestamp = STR(COMPILE_TIMESTAMP);

void print_version() {
	if constexpr (constexpr auto t = std::string_view{tag}; t.empty()) {
		std::cout << "version: " << revision << " (" << branch << ")\n";
	} else {
		std::cout << "version: " << tag << " (" << revision << ")\n";
	}
}
}
