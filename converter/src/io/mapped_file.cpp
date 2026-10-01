// SPDX-License-Identifier: GPL-3.0-or-later
//
// Platform mmap wrapper. Exempt from the raw-access check: this is where the
// OS pointer becomes a span.
#include "bethconv/io/mapped_file.hpp"

#include <system_error>
#include <utility>

#if defined(_WIN32)
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#else
#    include <fcntl.h>
#    include <sys/mman.h>
#    include <sys/stat.h>
#    include <unistd.h>
#endif

namespace bethconv::io {
namespace {

ParseError os_error(const std::filesystem::path& path, std::string what) {
#if defined(_WIN32)
    const auto code = static_cast<int>(::GetLastError());
    const std::error_code ec(code, std::system_category());
#else
    const std::error_code ec(errno, std::system_category());
#endif
    return ParseError{
        .origin = path.string(),
        .offset = 0,
        .kind = ErrorKind::corrupt,
        .detail = std::move(what) + ": " + ec.message(),
    };
}

} // namespace

MappedFile::~MappedFile() { reset(); }

MappedFile::MappedFile(MappedFile&& other) noexcept
    : data_(other.data_), origin_(std::move(other.origin_)), handle_(other.handle_) {
    other.data_ = {};
    other.handle_ = nullptr;
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        reset();
        data_ = other.data_;
        origin_ = std::move(other.origin_);
        handle_ = other.handle_;
        other.data_ = {};
        other.handle_ = nullptr;
    }
    return *this;
}

void MappedFile::reset() noexcept {
    if (!data_.empty()) {
#if defined(_WIN32)
        ::UnmapViewOfFile(const_cast<void*>(static_cast<const void*>(data_.data())));
#else
        ::munmap(const_cast<void*>(static_cast<const void*>(data_.data())), data_.size());
#endif
    }
#if defined(_WIN32)
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
        ::CloseHandle(handle_);
    }
#endif
    data_ = {};
    handle_ = nullptr;
}

ParseResult<MappedFile> MappedFile::open(const std::filesystem::path& path) {
    MappedFile out;
    out.origin_ = path.filename().string();

#if defined(_WIN32)
    // Shared for writing and deletion too: a pack's blob may still be open in
    // the writer, and Windows otherwise refuses the open.
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::unexpected(os_error(path, "CreateFileW"));
    }
    LARGE_INTEGER file_size{};
    if (::GetFileSizeEx(file, &file_size) == 0) {
        ::CloseHandle(file);
        return std::unexpected(os_error(path, "GetFileSizeEx"));
    }
    if (file_size.QuadPart == 0) {
        ::CloseHandle(file);
        return out; // empty file, empty span
    }
    HANDLE mapping = ::CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    ::CloseHandle(file);
    if (mapping == nullptr) {
        return std::unexpected(os_error(path, "CreateFileMappingW"));
    }
    const void* view = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        ::CloseHandle(mapping);
        return std::unexpected(os_error(path, "MapViewOfFile"));
    }
    out.handle_ = mapping;
    out.data_ = std::span(static_cast<const std::byte*>(view),
                          static_cast<std::size_t>(file_size.QuadPart));
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return std::unexpected(os_error(path, "open"));
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return std::unexpected(os_error(path, "fstat"));
    }
    if (!S_ISREG(st.st_mode)) {
        ::close(fd);
        return std::unexpected(ParseError{.origin = path.string(),
                                          .offset = 0,
                                          .kind = ErrorKind::bad_value,
                                          .detail = "not a regular file"});
    }
    const auto len = static_cast<std::size_t>(st.st_size);
    if (len == 0) {
        ::close(fd);
        return out; // empty file, empty span
    }
    void* view = ::mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd); // the mapping keeps its own reference
    if (view == MAP_FAILED) {
        return std::unexpected(os_error(path, "mmap"));
    }
    // Files are read front to back.
    ::madvise(view, len, MADV_SEQUENTIAL);
    out.data_ = std::span(static_cast<const std::byte*>(view), len);
#endif
    return out;
}

} // namespace bethconv::io
