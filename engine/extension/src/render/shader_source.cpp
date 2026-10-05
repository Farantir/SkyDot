// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/shader_source.hpp"

#include "render/materials.hpp"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstdint>
#include <mutex>
#include <unordered_map>

using godot::FileAccess;
using godot::Ref;
using godot::String;

namespace skydot::shader_source {

namespace {

void report(const String& path, const String& why) {
    godot::UtilityFunctions::push_error("shader_source: ", path, " ", why, ", so the shader made from it is empty");
}

/// `text` without its header comment, if it has one.
std::string without_header(std::string text, const String& path) {
    if (!text.starts_with("/*")) {
        return text;
    }
    const std::size_t end = text.find("*/");
    if (end == std::string::npos) {
        report(path, "starts a comment it never closes");
        return {};
    }
    const std::size_t next = end + 2;
    return text.substr(next < text.size() && text[next] == '\n' ? next + 1 : next);
}

std::string read(const char* name) {
    const String path = String("res://shaders/") + String::utf8(name);
    const godot::Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
    if (file.is_null()) {
        report(path, "cannot be opened (" + godot::UtilityFunctions::error_string(FileAccess::get_open_error()) + ")");
        return {};
    }
    const godot::PackedByteArray bytes = file->get_buffer(static_cast<std::int64_t>(file->get_length()));
    return without_header(std::string(bytes.ptr(), bytes.ptr() + bytes.size()), path);
}

} // namespace

std::string variant(const char* file, const std::vector<std::string>& defines) {
    SkydotMaterials::ensure_fog_globals(); // before anything compiles against them
    std::string code = "shader_type spatial;\n";
    for (const std::string& define : defines) {
        code += "#define " + define + "\n";
    }
    return code + "#include \"res://shaders/" + file + "\"\n";
}

Ref<godot::Shader> shader(const char* name) {
    SkydotMaterials::ensure_fog_globals();
    const String path = String("res://shaders/") + String::utf8(name);
    return godot::ResourceLoader::get_singleton()->load(path, "Shader");
}

String code(const char* name) {
    const Ref<godot::Shader> loaded = shader(name);
    return loaded.is_valid() ? loaded->get_code() : String();
}

const std::string& load(const char* name) {
    // A file is read once, under the lock; entries are never erased, so the
    // references stay valid.
    static std::mutex mutex;
    static std::unordered_map<std::string, std::string> files;
    const std::scoped_lock lock(mutex);
    if (const auto it = files.find(name); it != files.end()) {
        return it->second;
    }
    return files.emplace(name, read(name)).first->second;
}

} // namespace skydot::shader_source
