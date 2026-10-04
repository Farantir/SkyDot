// SPDX-License-Identifier: GPL-3.0-or-later
//
// PACK and FLST: AI packages with their branches and conditions, and the form
// lists NPCs name their default packages by. Private to pack/world/.
#pragma once

#include "context.hpp"

#include "bethconv/io/span_reader.hpp"
#include "bethconv/pack/world.hpp"
#include "bethconv/pack/world_generated.h"
#include "bethconv/record/field_reader.hpp"
#include "bethconv/record/merge.hpp"

#include <cstdint>
#include <map>
#include <vector>

namespace bethconv::pack::detail {

class AiCollector {
public:
    explicit AiCollector(CollectContext& shared) : shared_(shared) {}

    /// PACK or FLST; nothing for another type.
    void collect(const record::MergedRecord& merged, io::SpanReader& data,
                 const record::FormContext& form_ctx);

    /// FLST: kept to expand NPCs' default package lists.
    [[nodiscard]] const FormLists& form_lists() const noexcept { return form_lists_; }

    [[nodiscard]] std::vector<flatbuffers::Offset<wfb::Package>> write_packages(
        flatbuffers::FlatBufferBuilder& builder);

private:
    void on_form_list(const record::MergedRecord& merged, io::SpanReader& data,
                      const record::FormContext& form_ctx);
    void on_package(const record::MergedRecord& merged, io::SpanReader& data,
                    const record::FormContext& form_ctx);

    CollectContext& shared_;
    std::map<std::uint32_t, WorldPackage> packages_;
    FormLists form_lists_;
};

} // namespace bethconv::pack::detail
