// SPDX-License-Identifier: GPL-3.0-or-later
//
// The save container for GDScript: `SkydotSaveWriter` collects sections as
// PackedByteArrays and writes the file; `SkydotSaveReader` checks a file and
// hands its sections back. Lets a GDScript class (the player) write and read
// a section of its own. The format is save/save_file.hpp.
#pragma once

#include "save/save_file.hpp"

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace skydot {

class SkydotSaveWriter : public godot::RefCounted {
    GDCLASS(SkydotSaveWriter, godot::RefCounted)

public:
    /// `tag` is four characters.
    void add_section(const godot::String& tag, std::int64_t version, const godot::PackedByteArray& bytes);
    /// The whole file as bytes.
    godot::PackedByteArray to_bytes() const;
    /// Write to `path` (a filesystem path, not res:// or user://) through a
    /// temporary file and a rename.
    godot::Error write(const godot::String& path);
    godot::String get_error() const { return error_; }

protected:
    static void _bind_methods();

private:
    save::SaveWriter writer_;
    godot::String error_;
};

class SkydotSaveReader : public godot::RefCounted {
    GDCLASS(SkydotSaveReader, godot::RefCounted)

public:
    /// Check a file's magic, container version, framing and CRC.
    godot::Error parse(const godot::PackedByteArray& bytes);
    godot::Error open(const godot::String& path);
    bool has_section(const godot::String& tag) const;
    /// The section's bytes, empty if there is none.
    godot::PackedByteArray get_section(const godot::String& tag) const;
    std::int64_t get_section_version(const godot::String& tag) const;
    godot::String get_error() const { return error_; }
    /// The parsed file, for the classes that decode their sections.
    const save::SaveReader& file() const { return reader_; }

protected:
    static void _bind_methods();

private:
    save::SaveReader reader_;
    godot::String error_;
};

} // namespace skydot
