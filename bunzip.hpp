#pragma once
#include <vector>
#include <cstdint>
#include <cstddef>

namespace gm14 {
    bool bz2_decompress(const uint8_t* in_data, size_t in_len, std::vector<uint8_t>& out_data, size_t expected_len = 0);
}
