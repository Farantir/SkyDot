// SPDX-License-Identifier: GPL-3.0-or-later
#include "fb_write.hpp"

namespace bethconv::pack::detail {

flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<wfb::Script>>> write_scripts(
    flatbuffers::FlatBufferBuilder& builder, const std::vector<record::Script>& scripts) {
    std::vector<flatbuffers::Offset<wfb::Script>> out;
    out.reserve(scripts.size());
    for (const auto& script : scripts) {
        std::vector<flatbuffers::Offset<wfb::ScriptProperty>> properties;
        properties.reserve(script.properties.size());
        for (const auto& p : script.properties) {
            std::vector<wfb::ScriptObject> objects;
            objects.reserve(p.objects.size());
            for (const auto& o : p.objects) {
                objects.emplace_back(o.form.value, o.alias);
            }
            std::vector<flatbuffers::Offset<flatbuffers::String>> strings;
            strings.reserve(p.strings.size());
            for (const auto& text : p.strings) {
                strings.push_back(builder.CreateString(text));
            }
            const auto name = builder.CreateString(p.name);
            const auto objects_off = objects.empty() ? 0 : builder.CreateVectorOfStructs(objects);
            const auto strings_off = strings.empty() ? 0 : builder.CreateVector(strings);
            const auto ints_off = p.integers.empty() ? 0 : builder.CreateVector(p.integers);
            const auto floats_off = p.floats.empty() ? 0 : builder.CreateVector(p.floats);
            properties.push_back(wfb::CreateScriptProperty(
                builder, name, static_cast<std::uint8_t>(p.type), p.status, objects_off,
                strings_off, ints_off, floats_off));
        }
        const auto name = builder.CreateString(script.name);
        const auto properties_off = builder.CreateVector(properties);
        const auto status = static_cast<wfb::ScriptStatus>(script.status);
        out.push_back(wfb::CreateScript(builder, name, status, properties_off));
    }
    return builder.CreateVector(out);
}

wfb::Vec3f to_fb(const record::Vec3& v) { return wfb::Vec3f(v.x, v.y, v.z); }

} // namespace bethconv::pack::detail
