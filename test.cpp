#include <cstdint>
#include <print>

int main() {
	float a = 5.4;
	uint32_t *b;
	static_assert(sizeof(float) == sizeof(uint32_t));
	b = reinterpret_cast<uint32_t *>(&a);
	std::print("float: {}, hex: {:08x}, int: {}\n", a, *b, *b);
	return 0;
}
