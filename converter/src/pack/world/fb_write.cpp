// SPDX-License-Identifier: GPL-3.0-or-later
#include "fb_write.hpp"

namespace bethconv::pack::detail {

flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<wfb::Script>>> write_scripts(
    flatbuffers::FlatBufferBuilder& builder,
    const std::vector<std::unique_ptr<wfb::ScriptT>>& scripts) {
    std::vector<flatbuffers::Offset<wfb::Script>> out;
    out.reserve(scripts.size());
    for (const auto& script : scripts) {
        out.push_back(wfb::CreateScript(builder, script.get()));
    }
    return builder.CreateVector(out);
}

wfb::Vec3f to_fb(const record::Vec3& v) { return wfb::Vec3f(v.x, v.y, v.z); }

} // namespace bethconv::pack::detail
