// SPDX-License-Identifier: GPL-3.0-or-later
//
// Standalone driver for toolchains without libFuzzer. Replays the given files
// so the targets keep compiling and running under ctest.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

int replay(const std::filesystem::path& path, std::size_t& count) {
    const auto bytes = read_file(path);
    ++count;
    return LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <file-or-directory>...\n"
                     "Replays each input through this target. Not fuzzing; see "
                     "tests/fuzz/README.md.\n",
                     argv[0]);
        return 2;
    }

    std::size_t count = 0;
    for (int i = 1; i < argc; ++i) {
        const std::filesystem::path path(argv[i]);
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(path, ec)) {
                if (entry.is_regular_file(ec)) {
                    replay(entry.path(), count);
                }
            }
        } else {
            replay(path, count);
        }
    }
    std::printf("%zu input(s) replayed clean\n", count);
    return 0;
}
