// SPDX-License-Identifier: GPL-3.0-or-later
#include "assets/mapped_file.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace skydot {

MappedFile::~MappedFile() {
#if defined(_WIN32)
    if (data_ != nullptr) {
        UnmapViewOfFile(data_);
    }
    if (mapping_ != nullptr) {
        CloseHandle(mapping_);
    }
    if (file_ != nullptr && file_ != INVALID_HANDLE_VALUE) {
        CloseHandle(file_);
    }
#else
    if (data_ != nullptr && size_ != 0) {
        munmap(const_cast<std::uint8_t*>(data_), size_);
    }
#endif
}

bool MappedFile::open(const std::string& path, std::string& error) {
#if defined(_WIN32)
    const int wide_size = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(wide_size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), wide_size);
    // Shared for writing and deletion too: a converter may still hold the
    // blob open, and Windows otherwise refuses the open.
    file_ = CreateFileW(wide.c_str(), GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        error = "cannot open " + path;
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file_, &size)) {
        error = "cannot size " + path;
        return false;
    }
    size_ = static_cast<std::size_t>(size.QuadPart);
    if (size_ == 0) {
        return true;
    }
    mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping_ == nullptr) {
        error = "cannot map " + path;
        return false;
    }
    data_ = static_cast<const std::uint8_t*>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0));
    if (data_ == nullptr) {
        error = "cannot map " + path;
        return false;
    }
    return true;
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        error = "cannot open " + path;
        return false;
    }
    struct stat st {};
    if (fstat(fd, &st) != 0) {
        ::close(fd);
        error = "cannot size " + path;
        return false;
    }
    size_ = static_cast<std::size_t>(st.st_size);
    if (size_ == 0) {
        ::close(fd);
        return true;
    }
    void* mapped = mmap(nullptr, size_, PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (mapped == MAP_FAILED) {
        size_ = 0;
        error = "cannot map " + path;
        return false;
    }
    data_ = static_cast<const std::uint8_t*>(mapped);
    return true;
#endif
}

} // namespace skydot
