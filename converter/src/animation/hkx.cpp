// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/animation/hkx.hpp"

#include "bethconv/io/span_reader.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <utility>

namespace bethconv::animation {
namespace {

constexpr std::uint32_t k_magic0 = 0x57E0E057;
constexpr std::uint32_t k_magic1 = 0x10C0C010;
/// Havok binary tagfiles: self-describing, a different container. Six vanilla
/// SE files use it (Creation Club fishing); not read yet.
constexpr std::uint32_t k_tagfile0 = 0xCAB00D1E;
constexpr std::uint32_t k_tagfile1 = 0xD011FACE;
constexpr std::int32_t k_packfile_version = 8;
constexpr std::size_t k_section_header = 48;
constexpr std::size_t k_qs_transform = 48;
/// De Boor works on degree + 1 points; vanilla uses degree 1 to 3.
constexpr std::uint8_t k_max_degree = 7;
/// Interleaved animations are split into blocks of this many frames, the
/// size spline-compressed ones use, so knots fit a byte.
constexpr std::uint32_t k_interleaved_block = 256;

[[nodiscard]] std::size_t align_up(std::size_t value, std::size_t to) noexcept {
    return (value + to - 1) / to * to;
}

struct Section {
    std::string name;
    std::size_t start{};
    std::size_t local{};
    std::size_t global{};
    std::size_t virtual_{};
    std::size_t exports{};
    std::size_t end{};
};

/// Random-access reads over the whole file with a sticky first failure, so
/// the many small reads below need no individual checks. Every read of a
/// failed reader returns a zero value.
class Bytes {
public:
    Bytes(std::span<const std::byte> data, std::string_view origin) : whole_(data, origin) {}

    template <typename T>
    T at(std::size_t offset) {
        if (failure_) {
            return T{};
        }
        io::SpanReader r = whole_;
        if (auto seeked = r.seek(offset); !seeked) {
            failure_ = std::move(seeked).error();
            return T{};
        }
        auto value = r.get<T>();
        if (!value) {
            failure_ = std::move(value).error();
            return T{};
        }
        return *value;
    }

    std::string zstring_at(std::size_t offset) {
        if (failure_) {
            return {};
        }
        io::SpanReader r = whole_;
        if (auto seeked = r.seek(offset); !seeked) {
            failure_ = std::move(seeked).error();
            return {};
        }
        auto text = r.zstring();
        if (!text) {
            failure_ = std::move(text).error();
            return {};
        }
        return std::string(*text);
    }

    /// Records the first failure; later ones are dropped.
    void fail(io::ErrorKind kind, std::string detail) {
        if (!failure_) {
            failure_ = whole_.fail(kind, std::move(detail)).error();
        }
    }

    [[nodiscard]] bool ok() const noexcept { return !failure_; }
    [[nodiscard]] std::size_t size() const noexcept { return whole_.size(); }
    [[nodiscard]] std::optional<io::ParseError>& failure() noexcept { return failure_; }

private:
    io::SpanReader whole_;
    std::optional<io::ParseError> failure_;
};

class Packfile {
public:
    Packfile(std::span<const std::byte> data, std::string_view origin) : b_(data, origin) {}

    [[nodiscard]] Bytes& bytes() noexcept { return b_; }

    void open(HkxFile& out) {
        if (b_.size() < 0x40) {
            b_.fail(io::ErrorKind::truncated, "packfile header needs 64 bytes");
            return;
        }
        if (b_.at<std::uint32_t>(0) == k_tagfile0 && b_.at<std::uint32_t>(4) == k_tagfile1) {
            b_.fail(io::ErrorKind::unsupported, "Havok binary tagfile, not a packfile; not read yet");
            return;
        }
        if (b_.at<std::uint32_t>(0) != k_magic0 || b_.at<std::uint32_t>(4) != k_magic1) {
            b_.fail(io::ErrorKind::bad_magic, "not a Havok packfile");
            return;
        }
        const auto version = b_.at<std::int32_t>(12);
        ptr_ = b_.at<std::uint8_t>(16);
        const auto little = b_.at<std::uint8_t>(17);
        if (version != k_packfile_version) {
            b_.fail(io::ErrorKind::unsupported, "packfile version " + std::to_string(version));
            return;
        }
        if ((ptr_ != 4 && ptr_ != 8) || little != 1) {
            b_.fail(io::ErrorKind::unsupported,
                    "layout: " + std::to_string(ptr_) + "-byte pointers, little endian " +
                        std::to_string(little));
            return;
        }
        out.pointer_size = ptr_;
        const auto sections = b_.at<std::int32_t>(20);
        out.version = b_.zstring_at(40);
        if (out.version.size() > 15) {
            out.version.resize(15);
        }
        if (sections < 1 || sections > 16) {
            b_.fail(io::ErrorKind::bad_value, std::to_string(sections) + " sections");
            return;
        }
        for (std::int32_t s = 0; s < sections && b_.ok(); ++s) {
            const std::size_t h = 0x40 + static_cast<std::size_t>(s) * k_section_header;
            Section sec;
            for (std::size_t i = 0; i < 19; ++i) {
                const auto c = b_.at<char>(h + i);
                if (c == '\0') {
                    break;
                }
                sec.name.push_back(c);
            }
            const auto start = b_.at<std::int32_t>(h + 20);
            std::array<std::int32_t, 6> rel{};
            for (std::size_t i = 0; i < rel.size(); ++i) {
                rel[i] = b_.at<std::int32_t>(h + 24 + 4 * i);
            }
            // local, global, virtual, exports, imports, end: nondecreasing.
            bool ordered = start >= 0 && rel[0] >= 0;
            for (std::size_t i = 1; i < rel.size(); ++i) {
                ordered = ordered && rel[i] >= rel[i - 1];
            }
            if (!b_.ok()) {
                return;
            }
            if (!ordered || static_cast<std::size_t>(start) + static_cast<std::size_t>(rel[5]) > b_.size()) {
                b_.fail(io::ErrorKind::corrupt, "section '" + sec.name + "' offsets out of order or past the end");
                return;
            }
            sec.start = static_cast<std::size_t>(start);
            sec.local = sec.start + static_cast<std::size_t>(rel[0]);
            sec.global = sec.start + static_cast<std::size_t>(rel[1]);
            sec.virtual_ = sec.start + static_cast<std::size_t>(rel[2]);
            sec.exports = sec.start + static_cast<std::size_t>(rel[3]);
            sec.end = sec.start + static_cast<std::size_t>(rel[5]);
            sections_.push_back(std::move(sec));
        }
        if (!b_.ok()) {
            return;
        }
        for (const Section& s : sections_) {
            read_fixups(s);
        }
        for (const auto& [object, name] : classes_) {
            ++out.classes[name];
        }
    }

    [[nodiscard]] std::uint8_t ptr() const noexcept { return ptr_; }
    [[nodiscard]] std::size_t array_size() const noexcept { return ptr_ + 8u; }
    /// sizeof(hkReferencedObject): vtable, memSizeAndFlags, referenceCount.
    [[nodiscard]] std::size_t referenced() const noexcept { return ptr_ == 8 ? 16u : 8u; }

    /// Objects of `name` in file order.
    [[nodiscard]] std::vector<std::size_t> objects_of(std::string_view name) const {
        std::vector<std::size_t> out;
        for (const auto& [object, cls] : classes_) {
            if (cls == name) {
                out.push_back(object);
            }
        }
        return out;
    }

    [[nodiscard]] const std::string* class_of(std::size_t object) const {
        const auto it = classes_.find(object);
        return it == classes_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] std::optional<std::size_t> pointer(std::size_t at) const {
        const auto it = pointers_.find(at);
        if (it == pointers_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    /// hkStringPtr at `at`; empty when null.
    std::string string(std::size_t at) {
        const auto target = pointer(at);
        return target ? b_.zstring_at(*target) : std::string();
    }

    /// hkArray at `at`: its data and element count, checked against the file.
    struct Array {
        std::size_t data{};
        std::size_t count{};
    };
    Array array(std::size_t at, std::size_t element, std::string_view what) {
        const auto count = b_.at<std::int32_t>(at + ptr_);
        if (!b_.ok() || count == 0) {
            return {};
        }
        const auto data = pointer(at);
        if (count < 0 || !data || static_cast<std::size_t>(count) > b_.size() / element ||
            *data + static_cast<std::size_t>(count) * element > b_.size()) {
            b_.fail(io::ErrorKind::corrupt,
                    std::string(what) + ": " + std::to_string(count) + " elements of " +
                        std::to_string(element) + " bytes do not fit");
            return {};
        }
        return Array{*data, static_cast<std::size_t>(count)};
    }

private:
    void read_fixups(const Section& s) {
        const std::size_t length = s.local - s.start;
        const auto inside = [&](std::int64_t src) {
            return src >= 0 && static_cast<std::size_t>(src) + ptr_ <= length;
        };
        // Tables are padded with 0xFF (src == -1), so their size need not be a
        // multiple of the entry size.
        for (std::size_t p = s.local; p + 8 <= s.global && b_.ok(); p += 8) {
            const auto src = b_.at<std::int32_t>(p);
            const auto dst = b_.at<std::int32_t>(p + 4);
            if (src == -1) {
                continue;
            }
            if (!inside(src) || dst < 0 || static_cast<std::size_t>(dst) >= length) {
                b_.fail(io::ErrorKind::corrupt, "local fixup outside section '" + s.name + "'");
                return;
            }
            pointers_[s.start + static_cast<std::size_t>(src)] = s.start + static_cast<std::size_t>(dst);
        }
        for (std::size_t p = s.global; p + 12 <= s.virtual_ && b_.ok(); p += 12) {
            const auto src = b_.at<std::int32_t>(p);
            const auto sec = b_.at<std::int32_t>(p + 4);
            const auto dst = b_.at<std::int32_t>(p + 8);
            if (src == -1) {
                continue;
            }
            if (!inside(src) || !target_ok(sec, dst)) {
                b_.fail(io::ErrorKind::corrupt, "global fixup outside section '" + s.name + "'");
                return;
            }
            pointers_[s.start + static_cast<std::size_t>(src)] =
                sections_[static_cast<std::size_t>(sec)].start + static_cast<std::size_t>(dst);
        }
        for (std::size_t p = s.virtual_; p + 12 <= s.exports && b_.ok(); p += 12) {
            const auto src = b_.at<std::int32_t>(p);
            const auto sec = b_.at<std::int32_t>(p + 4);
            const auto name = b_.at<std::int32_t>(p + 8);
            if (src == -1) {
                continue;
            }
            if (src < 0 || static_cast<std::size_t>(src) >= length || !target_ok(sec, name)) {
                b_.fail(io::ErrorKind::corrupt, "virtual fixup outside section '" + s.name + "'");
                return;
            }
            classes_[s.start + static_cast<std::size_t>(src)] =
                b_.zstring_at(sections_[static_cast<std::size_t>(sec)].start + static_cast<std::size_t>(name));
        }
    }

    [[nodiscard]] bool target_ok(std::int32_t sec, std::int32_t offset) const {
        if (sec < 0 || static_cast<std::size_t>(sec) >= sections_.size() || offset < 0) {
            return false;
        }
        const Section& t = sections_[static_cast<std::size_t>(sec)];
        return static_cast<std::size_t>(offset) < t.local - t.start;
    }

    Bytes b_;
    std::uint8_t ptr_{};
    std::vector<Section> sections_;
    std::unordered_map<std::size_t, std::size_t> pointers_;
    std::map<std::size_t, std::string> classes_;
};

QsTransform qs_transform(Bytes& b, std::size_t at) {
    QsTransform t;
    for (std::size_t i = 0; i < 3; ++i) {
        t.translation[i] = b.at<float>(at + 4 * i);
        t.scale[i] = b.at<float>(at + 32 + 4 * i);
    }
    for (std::size_t i = 0; i < 4; ++i) {
        t.rotation[i] = b.at<float>(at + 16 + 4 * i);
    }
    return t;
}

Skeleton read_skeleton(Packfile& pf, std::size_t a) {
    Bytes& b = pf.bytes();
    const std::size_t A = pf.array_size();
    Skeleton s;
    s.name = pf.string(a + pf.referenced());
    const std::size_t o = a + pf.referenced() + pf.ptr();
    const auto parents = pf.array(o, 2, "skeleton parents");
    for (std::size_t i = 0; i < parents.count; ++i) {
        s.parents.push_back(b.at<std::int16_t>(parents.data + 2 * i));
    }
    // hkaBone: hkStringPtr name, hkBool lockTranslation, padded.
    const auto bones = pf.array(o + A, 2u * pf.ptr(), "skeleton bones");
    for (std::size_t i = 0; i < bones.count; ++i) {
        s.bones.push_back(pf.string(bones.data + i * 2u * pf.ptr()));
    }
    const auto pose = pf.array(o + 2 * A, k_qs_transform, "reference pose");
    for (std::size_t i = 0; i < pose.count; ++i) {
        s.reference_pose.push_back(qs_transform(b, pose.data + i * k_qs_transform));
    }
    // referenceFloats at o + 3A, floatSlots (hkStringPtr) at o + 4A.
    const auto slots = pf.array(o + 4 * A, pf.ptr(), "float slots");
    for (std::size_t i = 0; i < slots.count; ++i) {
        s.float_slots.push_back(pf.string(slots.data + i * pf.ptr()));
    }
    if (b.ok() && (s.parents.size() != s.bones.size() || s.reference_pose.size() != s.bones.size())) {
        b.fail(io::ErrorKind::corrupt, "skeleton '" + s.name + "': " + std::to_string(s.parents.size()) +
                                           " parents, " + std::to_string(s.bones.size()) + " bones, " +
                                           std::to_string(s.reference_pose.size()) + " poses");
    }
    for (std::size_t i = 0; i < s.parents.size() && b.ok(); ++i) {
        if (s.parents[i] < -1 || s.parents[i] >= static_cast<std::int16_t>(i)) {
            // Havok orders bones parents first.
            b.fail(io::ErrorKind::corrupt, "skeleton '" + s.name + "': bone " + std::to_string(i) +
                                               " has parent " + std::to_string(s.parents[i]));
        }
    }
    return s;
}

// ---- spline-compressed blocks ------------------------------------------

/// Reads one block's tracks in sequence from `data` (the animation's byte
/// array). Alignment is relative to the array, as Havok aligns in memory.
class BlockReader {
public:
    BlockReader(Bytes& b, std::size_t data, std::size_t data_size, std::size_t offset)
        : b_(b), data_(data), size_(data_size), o_(offset) {}

    [[nodiscard]] std::size_t offset() const noexcept { return o_; }
    void seek(std::size_t offset) { o_ = offset; }
    void align(std::size_t to) { o_ = align_up(o_, to); }

    template <typename T>
    T get() {
        if (o_ + sizeof(T) > size_) {
            b_.fail(io::ErrorKind::truncated, "animation data ends inside a block");
            o_ = size_;
            return T{};
        }
        const T v = b_.at<T>(data_ + o_);
        o_ += sizeof(T);
        return v;
    }

    /// Spline header: control point count - 1, degree, knots.
    bool knots(Channel& c, std::uint16_t& n) {
        n = get<std::uint16_t>();
        c.degree = get<std::uint8_t>();
        if (c.degree == 0 || c.degree > k_max_degree || c.degree > n) {
            if (b_.ok()) {
                b_.fail(io::ErrorKind::bad_value, "spline of degree " + std::to_string(c.degree) +
                                                      " over " + std::to_string(n + 1) + " points");
            }
            return false;
        }
        const std::size_t count = std::size_t{n} + c.degree + 2;
        c.knots.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            c.knots.push_back(get<std::uint8_t>());
        }
        if (!std::ranges::is_sorted(c.knots)) {
            if (b_.ok()) {
                b_.fail(io::ErrorKind::bad_value, "spline knots decrease");
            }
            return false;
        }
        return b_.ok();
    }

    /// Translation or scale. `flags`: bits 0-2 static axes, 4-6 spline axes.
    Channel vector(std::uint8_t flags, bool eight_bit, float identity) {
        Channel c;
        const bool any_spline = (flags & 0x70) != 0;
        const bool any_static = (flags & 0x07) != 0;
        if (any_spline) {
            std::uint16_t n = 0;
            if (!knots(c, n)) {
                return c;
            }
            align(4);
            c.lo.assign(3, identity);
            c.hi.assign(3, identity);
            for (std::size_t a = 0; a < 3; ++a) {
                if ((flags >> (4 + a)) & 1u) {
                    c.lo[a] = get<float>();
                    c.hi[a] = get<float>();
                } else if ((flags >> a) & 1u) {
                    c.lo[a] = c.hi[a] = get<float>();
                }
            }
            c.points.assign((std::size_t{n} + 1) * 3, 0);
            for (std::size_t i = 0; i <= n; ++i) {
                for (std::size_t a = 0; a < 3; ++a) {
                    if ((flags >> (4 + a)) & 1u) {
                        // 8-bit sources widen exactly: 255 * 257 = 65535.
                        c.points[i * 3 + a] = eight_bit
                                                  ? static_cast<std::uint16_t>(get<std::uint8_t>() * 257u)
                                                  : get<std::uint16_t>();
                    }
                }
            }
            align(4);
        } else if (any_static) {
            c.lo.assign(3, identity);
            for (std::size_t a = 0; a < 3; ++a) {
                if ((flags >> a) & 1u) {
                    c.lo[a] = get<float>();
                }
            }
            c.hi = c.lo;
        }
        return c;
    }

    /// Rotation. `flags`: 0xF0 spline, 0x0F static. Control points become
    /// 16-bit components between -1 and 1.
    Channel rotation(std::uint8_t flags, std::uint8_t quantization) {
        Channel c;
        if ((flags & 0xFF) == 0) {
            return c;
        }
        // THREECOMP40 (5 bytes), THREECOMP48 (6), uncompressed (16).
        std::size_t alignment = 1;
        switch (quantization) {
        case 1: alignment = 1; break;
        case 2: alignment = 2; break;
        case 5: alignment = 4; break;
        default:
            b_.fail(io::ErrorKind::unsupported,
                    "rotation quantization " + std::to_string(quantization) +
                        " (vanilla uses THREECOMP40 only)");
            return c;
        }
        if ((flags & 0xF0) != 0) {
            std::uint16_t n = 0;
            if (!knots(c, n)) {
                return c;
            }
            align(alignment);
            c.lo.assign(4, -1.0f);
            c.hi.assign(4, 1.0f);
            c.points.reserve((std::size_t{n} + 1) * 4);
            for (std::size_t i = 0; i <= n; ++i) {
                const auto q = quaternion(quantization);
                for (const float v : q) {
                    const float clamped = std::clamp(v, -1.0f, 1.0f);
                    c.points.push_back(static_cast<std::uint16_t>(
                        std::lround((clamped + 1.0f) * 0.5f * 65535.0f)));
                }
            }
        } else {
            align(alignment);
            const auto q = quaternion(quantization);
            c.lo.assign(q.begin(), q.end());
            c.hi = c.lo;
        }
        align(4);
        return c;
    }

    /// A float track: 0xF0 spline (16-bit points), 0x0F static.
    Channel scalar(std::uint8_t flags) {
        Channel c;
        if ((flags & 0xF0) != 0) {
            std::uint16_t n = 0;
            if (!knots(c, n)) {
                return c;
            }
            align(4);
            c.lo = {get<float>()};
            c.hi = {get<float>()};
            for (std::size_t i = 0; i <= n; ++i) {
                c.points.push_back(get<std::uint16_t>());
            }
            align(4);
        } else if ((flags & 0x0F) != 0) {
            c.lo = {get<float>()};
            c.hi = c.lo;
        }
        return c;
    }

private:
    std::array<float, 4> quaternion(std::uint8_t quantization) {
        std::array<float, 3> t{};
        std::uint32_t shift = 0;
        bool negative = false;
        if (quantization == 5) {
            std::array<float, 4> q{};
            for (float& v : q) {
                v = get<float>();
            }
            return q;
        }
        if (quantization == 1) {
            std::uint64_t c = 0;
            for (std::size_t i = 0; i < 5; ++i) {
                c |= std::uint64_t{get<std::uint8_t>()} << (8 * i);
            }
            constexpr std::uint64_t mask = (1u << 12) - 1;
            constexpr float half = static_cast<float>(mask >> 1);
            const float fraction = 1.0f / (half * std::sqrt(2.0f));
            for (std::size_t i = 0; i < 3; ++i) {
                t[i] = (static_cast<float>((c >> (12 * i)) & mask) - half) * fraction;
            }
            shift = static_cast<std::uint32_t>((c >> 36) & 3u);
            negative = ((c >> 38) & 1u) != 0;
        } else {
            const auto x = get<std::uint16_t>();
            const auto y = get<std::uint16_t>();
            const auto z = get<std::uint16_t>();
            shift = static_cast<std::uint32_t>(((y >> 14) & 2u) | ((x >> 15) & 1u));
            negative = ((z >> 15) & 1u) != 0;
            constexpr std::uint32_t mask = (1u << 15) - 1;
            constexpr float half = static_cast<float>(mask >> 1);
            const float fraction = 1.0f / (half * std::sqrt(2.0f));
            const std::array<std::uint16_t, 3> raw{x, y, z};
            for (std::size_t i = 0; i < 3; ++i) {
                t[i] = (static_cast<float>(raw[i] & mask) - half) * fraction;
            }
        }
        // The largest component was dropped; `shift` says where it goes.
        const float w2 = 1.0f - (t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
        float w = std::sqrt(std::max(0.0f, w2));
        if (negative) {
            w = -w;
        }
        std::array<float, 4> q{};
        std::size_t from = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            q[i] = i == shift ? w : t[from++];
        }
        return q;
    }

    Bytes& b_;
    std::size_t data_;
    std::size_t size_;
    std::size_t o_;
};

std::vector<std::int32_t> int_array(Packfile& pf, std::size_t at, std::string_view what) {
    const auto arr = pf.array(at, 4, what);
    std::vector<std::int32_t> out;
    out.reserve(arr.count);
    for (std::size_t i = 0; i < arr.count; ++i) {
        out.push_back(pf.bytes().at<std::int32_t>(arr.data + 4 * i));
    }
    return out;
}

void read_spline(Packfile& pf, std::size_t base, Clip& clip) {
    Bytes& b = pf.bytes();
    const auto frames = b.at<std::int32_t>(base);
    const auto blocks = b.at<std::int32_t>(base + 4);
    const auto per_block = b.at<std::int32_t>(base + 8);
    const auto mask_size = b.at<std::int32_t>(base + 12);
    clip.frame_duration = b.at<float>(base + 24);
    const std::size_t arrays = align_up(base + 28, pf.ptr());
    const std::size_t A = pf.array_size();
    const auto block_offsets = int_array(pf, arrays, "block offsets");
    const auto float_offsets = int_array(pf, arrays + A, "float block offsets");
    const auto track_offsets = int_array(pf, arrays + 2 * A, "transform offsets");
    const auto data = pf.array(arrays + 4 * A, 1, "animation data");
    if (!b.ok()) {
        return;
    }
    const std::size_t nt = clip.transform_tracks;
    const std::size_t nf = clip.float_tracks;
    if (frames < 1 || static_cast<std::uint32_t>(frames) > k_max_frames || blocks < 1 ||
        per_block < 2 || per_block > 256 || mask_size < 0 ||
        // The masks' size is padded to 4 bytes.
        static_cast<std::size_t>(mask_size) != align_up(4 * nt + nf, 4) ||
        block_offsets.size() != static_cast<std::size_t>(blocks) ||
        (nf != 0 && float_offsets.size() != static_cast<std::size_t>(blocks)) ||
        (!track_offsets.empty() && track_offsets.size() != static_cast<std::size_t>(blocks) * nt)) {
        b.fail(io::ErrorKind::bad_value,
               "spline animation: " + std::to_string(frames) + " frames, " + std::to_string(blocks) +
                   " blocks of " + std::to_string(per_block) + ", mask size " + std::to_string(mask_size) +
                   " for " + std::to_string(nt) + "+" + std::to_string(nf) + " tracks, " +
                   std::to_string(block_offsets.size()) + " block offsets");
        return;
    }
    // Blocks share their boundary frame, so this many cover the clip.
    const auto needed = (static_cast<std::uint32_t>(frames) - 1 + static_cast<std::uint32_t>(per_block) - 2) /
                        static_cast<std::uint32_t>(per_block - 1);
    if (static_cast<std::uint32_t>(blocks) < std::max(needed, 1u)) {
        b.fail(io::ErrorKind::bad_value, std::to_string(blocks) + " blocks for " + std::to_string(frames) + " frames");
        return;
    }
    clip.frame_count = static_cast<std::uint32_t>(frames);
    clip.frames_per_block = static_cast<std::uint32_t>(per_block);
    clip.blocks.resize(static_cast<std::size_t>(blocks));
    for (std::size_t blk = 0; blk < clip.blocks.size() && b.ok(); ++blk) {
        const auto start = block_offsets[blk];
        if (start < 0 || static_cast<std::size_t>(start) + static_cast<std::size_t>(mask_size) > data.count) {
            b.fail(io::ErrorKind::corrupt, "block " + std::to_string(blk) + " starts past the data");
            return;
        }
        const auto s = static_cast<std::size_t>(start);
        BlockReader in(b, data.data, data.count, s);
        std::vector<std::array<std::uint8_t, 4>> masks(nt);
        for (auto& m : masks) {
            for (auto& v : m) {
                v = in.get<std::uint8_t>();
            }
        }
        std::vector<std::uint8_t> float_masks(nf);
        for (auto& m : float_masks) {
            m = in.get<std::uint8_t>();
        }
        in.seek(s + align_up(static_cast<std::size_t>(mask_size), 4));
        Block& block = clip.blocks[blk];
        block.tracks.resize(nt);
        for (std::size_t t = 0; t < nt && b.ok(); ++t) {
            if (!track_offsets.empty()) {
                const auto expect = track_offsets[blk * nt + t];
                if (expect < 0 || s + static_cast<std::size_t>(expect) != in.offset()) {
                    b.fail(io::ErrorKind::corrupt, "block " + std::to_string(blk) + " track " + std::to_string(t) +
                                                       " starts at " + std::to_string(in.offset() - s) +
                                                       ", the file says " + std::to_string(expect));
                    return;
                }
            }
            const auto [quantization, position, rotation, scale] = masks[t];
            Track& track = block.tracks[t];
            track.translation = in.vector(position, (quantization & 3u) == 0, 0.0f);
            track.rotation = in.rotation(rotation, static_cast<std::uint8_t>((quantization >> 2) & 0xFu));
            track.scale = in.vector(scale, ((quantization >> 6) & 3u) == 0, 1.0f);
        }
        if (nf != 0 && b.ok()) {
            const auto fo = float_offsets[blk];
            if (fo < 0 || s + static_cast<std::size_t>(fo) < in.offset()) {
                b.fail(io::ErrorKind::corrupt, "block " + std::to_string(blk) + " float data overlaps its tracks");
                return;
            }
            in.seek(s + static_cast<std::size_t>(fo));
            block.floats.reserve(nf);
            for (std::size_t f = 0; f < nf; ++f) {
                block.floats.push_back(in.scalar(float_masks[f]));
            }
        }
    }
}

/// One block of linear splines per 256 frames, a control point per frame.
void read_interleaved(Packfile& pf, std::size_t base, Clip& clip) {
    Bytes& b = pf.bytes();
    const auto transforms = pf.array(base, k_qs_transform, "interleaved transforms");
    const auto floats = pf.array(base + pf.array_size(), 4, "interleaved floats");
    if (!b.ok()) {
        return;
    }
    const std::size_t nt = clip.transform_tracks;
    const std::size_t nf = clip.float_tracks;
    if (nt == 0 || transforms.count % nt != 0 || (nf != 0 && floats.count % nf != 0) ||
        (nf != 0 && floats.count / nf != transforms.count / nt) || transforms.count / nt > k_max_frames) {
        b.fail(io::ErrorKind::bad_value, "interleaved animation: " + std::to_string(transforms.count) +
                                             " transforms for " + std::to_string(nt) + " tracks");
        return;
    }
    const auto frames = static_cast<std::uint32_t>(transforms.count / nt);
    clip.frame_count = frames;
    clip.frames_per_block = k_interleaved_block;
    clip.frame_duration = frames > 1 ? clip.duration / static_cast<float>(frames - 1) : 0.0f;
    const std::uint32_t step = k_interleaved_block - 1;
    const std::uint32_t blocks = frames <= 1 ? 1 : (frames - 1 + step - 1) / step;

    // Quantize one block of a property between its per-axis extremes.
    const auto channel = [](std::span<const float> values, std::size_t width, std::uint32_t count) {
        Channel c;
        c.lo.assign(width, 0.0f);
        c.hi.assign(width, 0.0f);
        for (std::size_t a = 0; a < width; ++a) {
            c.lo[a] = c.hi[a] = values[a];
            for (std::uint32_t i = 0; i < count; ++i) {
                c.lo[a] = std::min(c.lo[a], values[i * width + a]);
                c.hi[a] = std::max(c.hi[a], values[i * width + a]);
            }
        }
        if (count == 1) {
            c.lo.assign(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(width));
            c.hi = c.lo;
            return c;
        }
        c.degree = 1;
        c.knots.push_back(0);
        for (std::uint32_t i = 0; i < count; ++i) {
            c.knots.push_back(static_cast<std::uint8_t>(i));
        }
        c.knots.push_back(static_cast<std::uint8_t>(count - 1));
        for (std::uint32_t i = 0; i < count; ++i) {
            for (std::size_t a = 0; a < width; ++a) {
                const float range = c.hi[a] - c.lo[a];
                const float q = range > 0.0f ? (values[i * width + a] - c.lo[a]) / range : 0.0f;
                c.points.push_back(static_cast<std::uint16_t>(std::lround(std::clamp(q, 0.0f, 1.0f) * 65535.0f)));
            }
        }
        return c;
    };

    clip.blocks.resize(blocks);
    for (std::uint32_t blk = 0; blk < blocks && b.ok(); ++blk) {
        const std::uint32_t first = blk * step;
        const std::uint32_t count = std::min(k_interleaved_block, frames - first);
        Block& block = clip.blocks[blk];
        for (std::size_t t = 0; t < nt; ++t) {
            std::vector<float> tr, rot, sc;
            for (std::uint32_t f = first; f < first + count; ++f) {
                const auto q = qs_transform(b, transforms.data + (f * nt + t) * k_qs_transform);
                tr.insert(tr.end(), q.translation.begin(), q.translation.end());
                rot.insert(rot.end(), q.rotation.begin(), q.rotation.end());
                sc.insert(sc.end(), q.scale.begin(), q.scale.end());
            }
            Track track{channel(tr, 3, count), channel(rot, 4, count), channel(sc, 3, count)};
            block.tracks.push_back(std::move(track));
        }
        for (std::size_t i = 0; i < nf; ++i) {
            std::vector<float> v;
            for (std::uint32_t f = first; f < first + count; ++f) {
                v.push_back(b.at<float>(floats.data + 4 * (f * nf + i)));
            }
            block.floats.push_back(channel(v, 1, count));
        }
    }
}

Clip read_clip(Packfile& pf, std::size_t a, const std::string& cls) {
    Bytes& b = pf.bytes();
    const std::size_t r = pf.referenced();
    Clip clip;
    clip.duration = b.at<float>(a + r + 4);
    const auto tracks = b.at<std::int32_t>(a + r + 8);
    const auto floats = b.at<std::int32_t>(a + r + 12);
    if (tracks < 0 || floats < 0 || static_cast<std::uint32_t>(tracks) > k_max_tracks ||
        static_cast<std::uint32_t>(floats) > k_max_tracks || !std::isfinite(clip.duration) || clip.duration < 0) {
        b.fail(io::ErrorKind::bad_value, "animation: " + std::to_string(tracks) + " tracks, " +
                                             std::to_string(floats) + " float tracks");
        return clip;
    }
    clip.transform_tracks = static_cast<std::uint32_t>(tracks);
    clip.float_tracks = static_cast<std::uint32_t>(floats);
    if (const auto motion = pf.pointer(a + r + 16)) {
        if (const auto* name = pf.class_of(*motion)) {
            clip.extracted_motion = *name;
        }
    }
    const std::size_t P = pf.ptr();
    const auto notes = pf.array(a + r + 16 + P, P + pf.array_size(), "annotation tracks");
    for (std::size_t i = 0; i < notes.count && b.ok(); ++i) {
        const std::size_t t = notes.data + i * (P + pf.array_size());
        AnnotationTrack track;
        track.name = pf.string(t);
        const auto entries = pf.array(t + P, 2 * P, "annotations");
        for (std::size_t j = 0; j < entries.count; ++j) {
            const std::size_t e = entries.data + j * 2 * P;
            track.annotations.push_back(Annotation{b.at<float>(e), pf.string(e + P)});
        }
        if (!track.annotations.empty() || !track.name.empty()) {
            clip.annotations.push_back(std::move(track));
        }
    }
    const std::size_t base = a + r + 16 + P + pf.array_size();
    if (cls == "hkaSplineCompressedAnimation") {
        clip.encoding = ClipEncoding::spline;
        read_spline(pf, base, clip);
    } else {
        clip.encoding = ClipEncoding::interleaved;
        read_interleaved(pf, base, clip);
    }
    return clip;
}

struct Binding {
    std::string skeleton;
    std::size_t animation{};
    std::vector<std::int16_t> track_to_bone;
    std::vector<std::int16_t> float_to_slot;
    std::uint8_t blend_hint{};
};

Binding read_binding(Packfile& pf, std::size_t a) {
    Bytes& b = pf.bytes();
    const std::size_t r = pf.referenced();
    const std::size_t P = pf.ptr();
    Binding out;
    out.skeleton = pf.string(a + r);
    out.animation = pf.pointer(a + r + P).value_or(0);
    const std::size_t o = a + r + 2 * P;
    const auto tracks = pf.array(o, 2, "track to bone");
    for (std::size_t i = 0; i < tracks.count; ++i) {
        out.track_to_bone.push_back(b.at<std::int16_t>(tracks.data + 2 * i));
    }
    const auto floats = pf.array(o + pf.array_size(), 2, "float to slot");
    for (std::size_t i = 0; i < floats.count; ++i) {
        out.float_to_slot.push_back(b.at<std::int16_t>(floats.data + 2 * i));
    }
    out.blend_hint = b.at<std::uint8_t>(o + 2 * pf.array_size());
    return out;
}

/// Standard de Boor over `degree + 1` points of `width` floats.
void de_boor(const Channel& c, float u, std::span<float> out) noexcept {
    const std::size_t width = c.width();
    const std::size_t p = c.degree;
    const std::size_t n = c.points.size() / width - 1;
    // Span: the last knot index in [p, n] with knots[i] <= u.
    std::size_t span = p;
    while (span < n && static_cast<float>(c.knots[span + 1]) <= u) {
        ++span;
    }
    std::array<std::array<float, 4>, k_max_degree + 1> d{};
    for (std::size_t j = 0; j <= p; ++j) {
        const std::size_t i = j + span - p;
        for (std::size_t a = 0; a < width; ++a) {
            d[j][a] = c.lo[a] + (c.hi[a] - c.lo[a]) * static_cast<float>(c.points[i * width + a]) / 65535.0f;
        }
    }
    for (std::size_t r = 1; r <= p; ++r) {
        for (std::size_t j = p; j >= r; --j) {
            const std::size_t i = j + span - p;
            const float k0 = c.knots[i];
            const float k1 = c.knots[i + p - r + 1];
            const float alpha = k1 > k0 ? (u - k0) / (k1 - k0) : 0.0f;
            for (std::size_t a = 0; a < width; ++a) {
                d[j][a] = (1.0f - alpha) * d[j - 1][a] + alpha * d[j][a];
            }
        }
    }
    for (std::size_t a = 0; a < width && a < out.size(); ++a) {
        out[a] = d[p][a];
    }
}

} // namespace

void evaluate(const Channel& channel, float u, std::span<float> out) noexcept {
    const std::size_t width = channel.width();
    if (width == 0 || width > 4) {
        return;
    }
    // A malformed channel (from a hand-built IR) evaluates to its constant.
    const bool usable = channel.is_spline() && channel.degree <= k_max_degree && channel.degree >= 1 &&
                        channel.hi.size() == width && channel.points.size() % width == 0 &&
                        channel.points.size() / width > channel.degree &&
                        channel.knots.size() == channel.points.size() / width + channel.degree + 1;
    if (!usable) {
        for (std::size_t a = 0; a < width && a < out.size(); ++a) {
            out[a] = channel.lo[a];
        }
        return;
    }
    de_boor(channel, u, out);
}

QsTransform sample(const Clip& clip, std::uint32_t track, std::uint32_t frame) noexcept {
    QsTransform out;
    if (clip.blocks.empty() || track >= clip.transform_tracks) {
        return out;
    }
    const std::uint32_t step = clip.frames_per_block > 1 ? clip.frames_per_block - 1 : 1;
    const std::size_t block = std::min<std::size_t>(frame / step, clip.blocks.size() - 1);
    const float u = static_cast<float>(frame - block * step);
    const Block& b = clip.blocks[block];
    if (track >= b.tracks.size()) {
        return out;
    }
    const Track& t = b.tracks[track];
    if (t.translation.width() == 3) {
        evaluate(t.translation, u, out.translation);
    }
    if (t.rotation.width() == 4) {
        evaluate(t.rotation, u, out.rotation);
        float len = 0.0f;
        for (const float v : out.rotation) {
            len += v * v;
        }
        len = std::sqrt(len);
        if (len > 0.0f) {
            for (float& v : out.rotation) {
                v /= len;
            }
        } else {
            out.rotation = {0, 0, 0, 1};
        }
    }
    if (t.scale.width() == 3) {
        evaluate(t.scale, u, out.scale);
    }
    return out;
}

io::ParseResult<HkxFile> read_hkx(std::span<const std::byte> bytes, std::string_view origin) {
    HkxFile out;
    Packfile pf(bytes, origin);
    pf.open(out);
    Bytes& b = pf.bytes();
    for (const std::size_t a : pf.objects_of("hkaSkeleton")) {
        if (!b.ok()) {
            break;
        }
        out.skeletons.push_back(read_skeleton(pf, a));
    }
    std::vector<Binding> bindings;
    for (const std::size_t a : pf.objects_of("hkaAnimationBinding")) {
        if (!b.ok()) {
            break;
        }
        bindings.push_back(read_binding(pf, a));
    }
    std::vector<std::size_t> clips;
    for (const char* cls : {"hkaSplineCompressedAnimation", "hkaInterleavedUncompressedAnimation"}) {
        for (const std::size_t a : pf.objects_of(cls)) {
            clips.push_back(a);
        }
    }
    std::ranges::sort(clips);
    for (const std::size_t a : clips) {
        if (!b.ok()) {
            break;
        }
        Clip clip = read_clip(pf, a, *pf.class_of(a));
        for (const Binding& bind : bindings) {
            if (bind.animation == a) {
                clip.skeleton_name = bind.skeleton;
                clip.track_to_bone = bind.track_to_bone;
                clip.float_to_slot = bind.float_to_slot;
                clip.blend_hint = bind.blend_hint;
            }
        }
        if (!clip.track_to_bone.empty() && clip.track_to_bone.size() != clip.transform_tracks) {
            b.fail(io::ErrorKind::corrupt, "binding maps " + std::to_string(clip.track_to_bone.size()) +
                                               " of " + std::to_string(clip.transform_tracks) + " tracks");
        }
        out.clips.push_back(std::move(clip));
    }
    for (const auto& [name, count] : out.classes) {
        if (name == "hkaQuantizedAnimation" || name == "hkaDeltaCompressedAnimation" ||
            name == "hkaWaveletCompressedAnimation") {
            b.fail(io::ErrorKind::unsupported, name + " (vanilla uses spline compression only)");
        }
    }
    if (auto& failure = b.failure()) {
        return std::unexpected(std::move(*failure));
    }
    return out;
}

} // namespace bethconv::animation
