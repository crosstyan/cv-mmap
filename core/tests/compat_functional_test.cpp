#include <cvmmap/compat/functional.hpp>

#include <iostream>
#include <memory>
#include <type_traits>
#include <utility>

namespace {

struct DestructionProbe {
	explicit DestructionProbe(int &destroyed_count) : destroyed_count_(&destroyed_count) {}

	DestructionProbe(const DestructionProbe &) = delete;
	DestructionProbe &operator=(const DestructionProbe &) = delete;

	DestructionProbe(DestructionProbe &&other) noexcept
		: destroyed_count_(std::exchange(other.destroyed_count_, nullptr)) {}
	DestructionProbe &operator=(DestructionProbe &&) = delete;

	~DestructionProbe() {
		if (destroyed_count_ != nullptr) {
			++*destroyed_count_;
		}
	}

	void operator()() const {}

	int *destroyed_count_{nullptr};
};

static_assert(!std::is_copy_constructible_v<cvmmap::move_only_function<void()>>);
static_assert(!std::is_copy_assignable_v<cvmmap::move_only_function<void()>>);
static_assert(std::is_move_constructible_v<cvmmap::move_only_function<void()>>);
static_assert(std::is_move_assignable_v<cvmmap::move_only_function<void()>>);

bool test_move_only_capture() {
	auto value = std::make_unique<int>(41);
	cvmmap::move_only_function<int(int)> fn{
		[value = std::move(value)](const int delta) { return *value + delta; }};

	if (!fn || fn(1) != 42) {
		std::cerr << "move-only capture did not invoke correctly\n";
		return false;
	}

	cvmmap::move_only_function<int(int)> moved{std::move(fn)};
	if (fn || !moved || moved(0) != 41) {
		std::cerr << "move construction did not transfer ownership\n";
		return false;
	}

	moved = [value = std::make_unique<int>(10)](const int delta) {
		return *value + delta;
	};
	if (!moved || moved(5) != 15) {
		std::cerr << "move-only lambda assignment did not invoke correctly\n";
		return false;
	}

	return true;
}

bool test_assignment_releases_previous_target() {
	int destroyed_count = 0;
	{
		cvmmap::move_only_function<void()> fn{DestructionProbe{destroyed_count}};
		fn = [] {};
		if (destroyed_count != 1) {
			std::cerr << "move assignment did not destroy the previous target\n";
			return false;
		}

		fn = nullptr;
		if (fn) {
			std::cerr << "nullptr assignment did not clear the function\n";
			return false;
		}
	}

	return destroyed_count == 1;
}

} // namespace

int main() {
	const auto ok = test_move_only_capture() && test_assignment_releases_previous_target();
	return ok ? 0 : 1;
}
