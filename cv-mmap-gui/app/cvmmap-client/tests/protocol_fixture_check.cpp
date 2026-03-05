/**
 * Headless CLI Protocol Fixture Checker for cv-mmap-gui
 * 
 * This is a standalone, CLI-only test harness that verifies protocol
 * parsing logic without any GUI or OpenSSL dependencies.
 * 
 * Usage: protocol_fixture_check [--verbose]
 * Exit code: 0 on success, non-zero on any fixture failure
 */

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>
#include <vector>

// Protocol constants matching cvmmap.ksy specification
namespace cvmmap {
    constexpr uint8_t PROTOCOL_VERSION_V1 = 1;
    constexpr uint8_t PROTOCOL_VERSION_V2 = 2;
    constexpr size_t HEADER_SIZE_V1 = 128;
    constexpr size_t HEADER_SIZE_V2 = 256;
    constexpr size_t PLANE_DESCRIPTOR_SIZE = 64;
    constexpr size_t MAX_PLANES = 2;
    
    // Header offsets
    constexpr size_t OFFSET_VERSION_MAJOR = 0;
    constexpr size_t OFFSET_VERSION_MINOR = 1;
    constexpr size_t OFFSET_PLANE_COUNT = 2;
    constexpr size_t OFFSET_WIDTH = 4;
    constexpr size_t OFFSET_HEIGHT = 8;
    constexpr size_t OFFSET_PLANES = 64;  // Plane descriptors start here
    
    // Plane descriptor offsets (within each 64-byte descriptor)
    constexpr size_t PLANE_OFFSET_DATA_PTR = 0;
    constexpr size_t PLANE_OFFSET_STRIDE = 8;
    constexpr size_t PLANE_OFFSET_SIZE = 16;
    constexpr size_t PLANE_OFFSET_TYPE = 24;
}

// Test result tracking
struct TestResult {
    std::string_view name;
    bool passed;
    std::string_view message;
};

class FixtureChecker {
public:
    std::vector<TestResult> results;
    bool verbose = false;
    
    void log(const std::string_view& msg) {
        if (verbose) {
            std::cout << "  [INFO] " << msg << std::endl;
        }
    }
    
    void check(bool condition, std::string_view name, std::string_view fail_msg) {
        if (condition) {
            results.push_back({name, true, "OK"});
            std::cout << "  [PASS] " << name << std::endl;
        } else {
            results.push_back({name, false, fail_msg});
            std::cout << "  [FAIL] " << name << ": " << fail_msg << std::endl;
        }
    }
    
    // Parse and validate v1 header
    bool parse_v1(const std::vector<uint8_t>& data) {
        if (data.size() < cvmmap::HEADER_SIZE_V1) {
            return false;
        }
        
        uint8_t major = data[cvmmap::OFFSET_VERSION_MAJOR];
        uint8_t minor = data[cvmmap::OFFSET_VERSION_MINOR];
        
        // v1 should have major=1
        if (major != cvmmap::PROTOCOL_VERSION_V1) {
            return false;
        }
        
        // Check basic dimensions are non-zero
        uint32_t width = *reinterpret_cast<const uint32_t*>(&data[cvmmap::OFFSET_WIDTH]);
        uint32_t height = *reinterpret_cast<const uint32_t*>(&data[cvmmap::OFFSET_HEIGHT]);
        
        return width > 0 && height > 0 && width < 100000 && height < 100000;
    }
    
    // Parse and validate v2 header
    bool parse_v2(const std::vector<uint8_t>& data, bool expect_depth = false) {
        if (data.size() < cvmmap::HEADER_SIZE_V2) {
            return false;
        }
        
        uint8_t major = data[cvmmap::OFFSET_VERSION_MAJOR];
        uint8_t minor = data[cvmmap::OFFSET_VERSION_MINOR];
        uint8_t plane_count = data[cvmmap::OFFSET_PLANE_COUNT];
        
        // v2 should have major=2
        if (major != cvmmap::PROTOCOL_VERSION_V2) {
            return false;
        }
        
        // Check plane count
        if (plane_count == 0 || plane_count > cvmmap::MAX_PLANES) {
            return false;
        }
        
        if (expect_depth && plane_count != 2) {
            return false;
        }
        
        if (!expect_depth && plane_count != 1) {
            return false;
        }
        
        // Validate plane descriptors
        for (size_t i = 0; i < plane_count; ++i) {
            size_t plane_offset = cvmmap::OFFSET_PLANES + (i * cvmmap::PLANE_DESCRIPTOR_SIZE);
            
            uint64_t data_ptr = *reinterpret_cast<const uint64_t*>(&data[plane_offset + cvmmap::PLANE_OFFSET_DATA_PTR]);
            uint64_t stride = *reinterpret_cast<const uint64_t*>(&data[plane_offset + cvmmap::PLANE_OFFSET_STRIDE]);
            uint64_t size = *reinterpret_cast<const uint64_t*>(&data[plane_offset + cvmmap::PLANE_OFFSET_SIZE]);
            
            // Basic sanity checks
            if (stride == 0 || size == 0) {
                return false;
            }
            
            if (size > 100 * 1024 * 1024) {  // 100MB max per plane
                return false;
            }
        }
        
        return true;
    }
    
    // Validate that malformed data is rejected
    bool validate_malformed_rejected(const std::vector<uint8_t>& data) {
        // If it parses as valid v1 or v2, it's not malformed enough
        bool parses_as_v1 = parse_v1(data);
        bool parses_as_v2_left_only = parse_v2(data, false);
        bool parses_as_v2_with_depth = parse_v2(data, true);
        
        // Should NOT parse as any valid format
        return !parses_as_v1 && !parses_as_v2_left_only && !parses_as_v2_with_depth;
    }
};

// Build fixture data
namespace fixtures {
    
    // v1 valid: 128-byte header with version=1
    std::vector<uint8_t> build_v1_valid() {
        std::vector<uint8_t> data(cvmmap::HEADER_SIZE_V1, 0);
        data[cvmmap::OFFSET_VERSION_MAJOR] = cvmmap::PROTOCOL_VERSION_V1;
        data[cvmmap::OFFSET_VERSION_MINOR] = 0;
        
        // Set dimensions: 1920x1080
        uint32_t width = 1920;
        uint32_t height = 1080;
        std::memcpy(&data[cvmmap::OFFSET_WIDTH], &width, sizeof(width));
        std::memcpy(&data[cvmmap::OFFSET_HEIGHT], &height, sizeof(height));
        
        return data;
    }
    
    // v2 left-only: 256-byte header with version=2, plane_count=1
    std::vector<uint8_t> build_v2_left_only() {
        std::vector<uint8_t> data(cvmmap::HEADER_SIZE_V2, 0);
        data[cvmmap::OFFSET_VERSION_MAJOR] = cvmmap::PROTOCOL_VERSION_V2;
        data[cvmmap::OFFSET_VERSION_MINOR] = 0;
        data[cvmmap::OFFSET_PLANE_COUNT] = 1;
        
        // Set dimensions: 1280x720
        uint32_t width = 1280;
        uint32_t height = 720;
        std::memcpy(&data[cvmmap::OFFSET_WIDTH], &width, sizeof(width));
        std::memcpy(&data[cvmmap::OFFSET_HEIGHT], &height, sizeof(height));
        
        // Set plane 0 (left) descriptor
        size_t plane_offset = cvmmap::OFFSET_PLANES;
        uint64_t data_ptr = 0x1000;  // Mock pointer
        uint64_t stride = 2560;      // 1280 * 2 bytes per pixel
        uint64_t size = stride * height;
        uint32_t type = 0;           // Left image
        
        std::memcpy(&data[plane_offset + cvmmap::PLANE_OFFSET_DATA_PTR], &data_ptr, sizeof(data_ptr));
        std::memcpy(&data[plane_offset + cvmmap::PLANE_OFFSET_STRIDE], &stride, sizeof(stride));
        std::memcpy(&data[plane_offset + cvmmap::PLANE_OFFSET_SIZE], &size, sizeof(size));
        std::memcpy(&data[plane_offset + cvmmap::PLANE_OFFSET_TYPE], &type, sizeof(type));
        
        return data;
    }
    
    // v2 left+depth: 256-byte header with version=2, plane_count=2
    std::vector<uint8_t> build_v2_left_depth() {
        std::vector<uint8_t> data(cvmmap::HEADER_SIZE_V2, 0);
        data[cvmmap::OFFSET_VERSION_MAJOR] = cvmmap::PROTOCOL_VERSION_V2;
        data[cvmmap::OFFSET_VERSION_MINOR] = 0;
        data[cvmmap::OFFSET_PLANE_COUNT] = 2;
        
        // Set dimensions: 1280x720
        uint32_t width = 1280;
        uint32_t height = 720;
        std::memcpy(&data[cvmmap::OFFSET_WIDTH], &width, sizeof(width));
        std::memcpy(&data[cvmmap::OFFSET_HEIGHT], &height, sizeof(height));
        
        // Set plane 0 (left) descriptor
        size_t plane0_offset = cvmmap::OFFSET_PLANES;
        uint64_t data_ptr0 = 0x1000;
        uint64_t stride0 = 2560;
        uint64_t size0 = stride0 * height;
        uint32_t type0 = 0;  // Left image
        
        std::memcpy(&data[plane0_offset + cvmmap::PLANE_OFFSET_DATA_PTR], &data_ptr0, sizeof(data_ptr0));
        std::memcpy(&data[plane0_offset + cvmmap::PLANE_OFFSET_STRIDE], &stride0, sizeof(stride0));
        std::memcpy(&data[plane0_offset + cvmmap::PLANE_OFFSET_SIZE], &size0, sizeof(size0));
        std::memcpy(&data[plane0_offset + cvmmap::PLANE_OFFSET_TYPE], &type0, sizeof(type0));
        
        // Set plane 1 (depth) descriptor
        size_t plane1_offset = cvmmap::OFFSET_PLANES + cvmmap::PLANE_DESCRIPTOR_SIZE;
        uint64_t data_ptr1 = 0x2000;
        uint64_t stride1 = 1280 * sizeof(float);  // 4 bytes per depth pixel
        uint64_t size1 = stride1 * height;
        uint32_t type1 = 1;  // Depth image
        
        std::memcpy(&data[plane1_offset + cvmmap::PLANE_OFFSET_DATA_PTR], &data_ptr1, sizeof(data_ptr1));
        std::memcpy(&data[plane1_offset + cvmmap::PLANE_OFFSET_STRIDE], &stride1, sizeof(stride1));
        std::memcpy(&data[plane1_offset + cvmmap::PLANE_OFFSET_SIZE], &size1, sizeof(size1));
        std::memcpy(&data[plane1_offset + cvmmap::PLANE_OFFSET_TYPE], &type1, sizeof(type1));
        
        return data;
    }
    
    // v2 malformed: invalid plane_count (> MAX_PLANES)
    std::vector<uint8_t> build_v2_malformed() {
        std::vector<uint8_t> data(cvmmap::HEADER_SIZE_V2, 0);
        data[cvmmap::OFFSET_VERSION_MAJOR] = cvmmap::PROTOCOL_VERSION_V2;
        data[cvmmap::OFFSET_VERSION_MINOR] = 0;
        data[cvmmap::OFFSET_PLANE_COUNT] = 5;  // Invalid: > MAX_PLANES (2)
        
        // Set dimensions
        uint32_t width = 1280;
        uint32_t height = 720;
        std::memcpy(&data[cvmmap::OFFSET_WIDTH], &width, sizeof(width));
        std::memcpy(&data[cvmmap::OFFSET_HEIGHT], &height, sizeof(height));
        
        return data;
    }
    
    // Additional malformed: zero stride in plane descriptor
    std::vector<uint8_t> build_v2_zero_stride() {
        std::vector<uint8_t> data(cvmmap::HEADER_SIZE_V2, 0);
        data[cvmmap::OFFSET_VERSION_MAJOR] = cvmmap::PROTOCOL_VERSION_V2;
        data[cvmmap::OFFSET_VERSION_MINOR] = 0;
        data[cvmmap::OFFSET_PLANE_COUNT] = 1;
        
        uint32_t width = 1280;
        uint32_t height = 720;
        std::memcpy(&data[cvmmap::OFFSET_WIDTH], &width, sizeof(width));
        std::memcpy(&data[cvmmap::OFFSET_HEIGHT], &height, sizeof(height));
        
        // Set plane with zero stride (invalid)
        size_t plane_offset = cvmmap::OFFSET_PLANES;
        uint64_t stride = 0;  // Invalid: zero stride
        uint64_t size = 1000;
        
        std::memcpy(&data[plane_offset + cvmmap::PLANE_OFFSET_STRIDE], &stride, sizeof(stride));
        std::memcpy(&data[plane_offset + cvmmap::PLANE_OFFSET_SIZE], &size, sizeof(size));
        
        return data;
    }
    
    // Truncated data: too small for v2
    std::vector<uint8_t> build_truncated() {
        // Only 100 bytes, but v2 requires 256
        return std::vector<uint8_t>(100, 0xAB);
    }
}

int main(int argc, char* argv[]) {
    bool verbose = false;
    
    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  --verbose, -v    Enable verbose output\n"
                      << "  --help, -h       Show this help\n"
                      << "\n"
                      << "Exit codes:\n"
                      << "  0   All fixtures passed\n"
                      << "  1   One or more fixtures failed\n"
                      << "  2   Internal error\n";
            return 0;
        }
    }
    
    std::cout << "========================================\n"
              << "Protocol Fixture Checker (Headless CLI)\n"
              << "========================================\n" << std::endl;
    
    FixtureChecker checker;
    checker.verbose = verbose;
    
    // Test 1: v1 valid fixture
    std::cout << "[TEST] v1 valid fixture" << std::endl;
    {
        auto fixture = fixtures::build_v1_valid();
        checker.log("Fixture size: " + std::to_string(fixture.size()) + " bytes");
        checker.log("Version: " + std::to_string(fixture[cvmmap::OFFSET_VERSION_MAJOR]));
        
        bool result = checker.parse_v1(fixture);
        checker.check(result, "v1 parses correctly", "Failed to parse valid v1 header");
        
        // v1 should NOT parse as v2
        bool not_v2 = !checker.parse_v2(fixture, false);
        checker.check(not_v2, "v1 rejected by v2 parser", "v1 incorrectly accepted by v2 parser");
    }
    std::cout << std::endl;
    
    // Test 2: v2 left-only fixture
    std::cout << "[TEST] v2 left-only valid fixture" << std::endl;
    {
        auto fixture = fixtures::build_v2_left_only();
        checker.log("Fixture size: " + std::to_string(fixture.size()) + " bytes");
        checker.log("Version: " + std::to_string(fixture[cvmmap::OFFSET_VERSION_MAJOR]));
        checker.log("Plane count: " + std::to_string(fixture[cvmmap::OFFSET_PLANE_COUNT]));
        
        bool result = checker.parse_v2(fixture, false);
        checker.check(result, "v2 left-only parses correctly", "Failed to parse valid v2 left-only header");
        
        // Should NOT parse with depth expectation
        bool not_depth = !checker.parse_v2(fixture, true);
        checker.check(not_depth, "v2 left-only rejected when depth expected", 
                      "v2 left-only incorrectly accepted as depth");
    }
    std::cout << std::endl;
    
    // Test 3: v2 left+depth fixture
    std::cout << "[TEST] v2 left+depth valid fixture" << std::endl;
    {
        auto fixture = fixtures::build_v2_left_depth();
        checker.log("Fixture size: " + std::to_string(fixture.size()) + " bytes");
        checker.log("Version: " + std::to_string(fixture[cvmmap::OFFSET_VERSION_MAJOR]));
        checker.log("Plane count: " + std::to_string(fixture[cvmmap::OFFSET_PLANE_COUNT]));
        
        bool result = checker.parse_v2(fixture, true);
        checker.check(result, "v2 left+depth parses correctly", "Failed to parse valid v2 left+depth header");
        
        // v2 left+depth has 2 planes, so it should NOT parse when exactly 1 plane is expected
        bool also_valid = checker.parse_v2(fixture, false);
        checker.check(!also_valid, "v2 left+depth rejected when left-only expected",
                      "v2 left+depth incorrectly accepted as left-only");
    }
    std::cout << std::endl;
    
    // Test 4: Malformed fixtures should be rejected
    std::cout << "[TEST] malformed fixture rejection" << std::endl;
    {
        // Malformed: invalid plane count
        auto malformed = fixtures::build_v2_malformed();
        bool rejected = checker.validate_malformed_rejected(malformed);
        checker.check(rejected, "invalid plane count rejected", 
                      "Malformed data (invalid plane count) was incorrectly accepted");
        
        // Malformed: zero stride
        auto zero_stride = fixtures::build_v2_zero_stride();
        rejected = checker.validate_malformed_rejected(zero_stride);
        checker.check(rejected, "zero stride rejected", 
                      "Malformed data (zero stride) was incorrectly accepted");
        
        // Malformed: truncated data
        auto truncated = fixtures::build_truncated();
        rejected = checker.validate_malformed_rejected(truncated);
        checker.check(rejected, "truncated data rejected", 
                      "Malformed data (truncated) was incorrectly accepted");
    }
    std::cout << std::endl;
    
    // Summary
    std::cout << "========================================" << std::endl;
    std::cout << "SUMMARY" << std::endl;
    std::cout << "========================================" << std::endl;
    
    size_t passed = 0;
    size_t failed = 0;
    
    for (const auto& result : checker.results) {
        if (result.passed) {
            ++passed;
        } else {
            ++failed;
        }
    }
    
    std::cout << "Total:  " << checker.results.size() << std::endl;
    std::cout << "Passed: " << passed << std::endl;
    std::cout << "Failed: " << failed << std::endl;
    
    if (failed > 0) {
        std::cout << "\nFAILED TESTS:" << std::endl;
        for (const auto& result : checker.results) {
            if (!result.passed) {
                std::cout << "  - " << result.name << ": " << result.message << std::endl;
            }
        }
    }
    
    std::cout << "========================================" << std::endl;
    
    return (failed > 0) ? 1 : 0;
}
