// SPDX-License-Identifier: GPL-3.0-or-later
#include "save/save_binding.hpp"

#include <godot_cpp/core/class_db.hpp>

#include <algorithm>

namespace skydot {

using godot::PackedByteArray;
using godot::String;

namespace {

std::vector<std::uint8_t> to_vector(const PackedByteArray& bytes) {
    return {bytes.ptr(), bytes.ptr() + bytes.size()};
}

String from_std(const std::string& text) { return String::utf8(text.c_str()); }

} // namespace

void SkydotSaveWriter::_bind_methods() {
    godot::ClassDB::bind_method(godot::D_METHOD("add_section", "tag", "version", "bytes"), &SkydotSaveWriter::add_section);
    godot::ClassDB::bind_method(godot::D_METHOD("to_bytes"), &SkydotSaveWriter::to_bytes);
    godot::ClassDB::bind_method(godot::D_METHOD("write", "path"), &SkydotSaveWriter::write);
    godot::ClassDB::bind_method(godot::D_METHOD("get_error"), &SkydotSaveWriter::get_error);
}

void SkydotSaveWriter::add_section(const String& tag, std::int64_t version, const PackedByteArray& bytes) {
    writer_.add(tag.utf8().get_data(), static_cast<std::uint32_t>(version), to_vector(bytes));
}

PackedByteArray SkydotSaveWriter::to_bytes() const {
    const auto bytes = writer_.finish();
    PackedByteArray out;
    out.resize(static_cast<std::int64_t>(bytes.size()));
    std::copy(bytes.begin(), bytes.end(), out.ptrw());
    return out;
}

godot::Error SkydotSaveWriter::write(const String& path) {
    std::string why;
    if (!save::SaveWriter::write_file(path.utf8().get_data(), writer_.finish(), why)) {
        error_ = from_std(why);
        return godot::ERR_FILE_CANT_WRITE;
    }
    error_ = String();
    return godot::OK;
}

void SkydotSaveReader::_bind_methods() {
    godot::ClassDB::bind_method(godot::D_METHOD("parse", "bytes"), &SkydotSaveReader::parse);
    godot::ClassDB::bind_method(godot::D_METHOD("open", "path"), &SkydotSaveReader::open);
    godot::ClassDB::bind_method(godot::D_METHOD("has_section", "tag"), &SkydotSaveReader::has_section);
    godot::ClassDB::bind_method(godot::D_METHOD("get_section", "tag"), &SkydotSaveReader::get_section);
    godot::ClassDB::bind_method(godot::D_METHOD("get_section_version", "tag"), &SkydotSaveReader::get_section_version);
    godot::ClassDB::bind_method(godot::D_METHOD("get_error"), &SkydotSaveReader::get_error);
}

godot::Error SkydotSaveReader::parse(const PackedByteArray& bytes) {
    std::string why;
    if (!reader_.parse(to_vector(bytes), why)) {
        error_ = from_std(why);
        return godot::ERR_FILE_CORRUPT;
    }
    error_ = String();
    return godot::OK;
}

godot::Error SkydotSaveReader::open(const String& path) {
    std::vector<std::uint8_t> bytes;
    std::string why;
    if (!save::SaveReader::read_file(path.utf8().get_data(), bytes, why)) {
        error_ = from_std(why);
        return godot::ERR_FILE_NOT_FOUND;
    }
    if (!reader_.parse(std::move(bytes), why)) {
        error_ = from_std(why);
        return godot::ERR_FILE_CORRUPT;
    }
    error_ = String();
    return godot::OK;
}

bool SkydotSaveReader::has_section(const String& tag) const { return reader_.find(tag.utf8().get_data()) != nullptr; }

PackedByteArray SkydotSaveReader::get_section(const String& tag) const {
    PackedByteArray out;
    if (const auto* s = reader_.find(tag.utf8().get_data())) {
        out.resize(static_cast<std::int64_t>(s->size));
        auto r = reader_.open(*s);
        for (std::size_t i = 0; i < s->size; ++i) {
            std::uint8_t b = 0;
            r.u8(b);
            out.set(static_cast<std::int64_t>(i), b);
        }
    }
    return out;
}

std::int64_t SkydotSaveReader::get_section_version(const String& tag) const {
    const auto* s = reader_.find(tag.utf8().get_data());
    return s != nullptr ? static_cast<std::int64_t>(s->version) : std::int64_t{-1};
}

} // namespace skydot
