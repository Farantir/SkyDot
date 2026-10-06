// SPDX-License-Identifier: GPL-3.0-or-later
#include "sink.hpp"

namespace bethconv::pack::detail {
namespace {

using io::FourCC;

} // namespace

void WorldSink::on_superseded(const record::MergedRecord& merged, const record::RecordContext&,
                              io::SpanReader& data, const record::FormContext& form_ctx) {
    if (!merged.deleted) {
        environment_.collect_large_refs(merged, data, form_ctx);
    }
}

void WorldSink::on_record(const record::MergedRecord& merged, const record::RecordContext& ctx,
                          io::SpanReader& data, const record::FormContext& form_ctx) {
    if (merged.deleted) {
        return;
    }
    switch (merged.type.value) {
    case FourCC{"CELL"}.value:
    case FourCC{"REFR"}.value:
    case FourCC{"ACHR"}.value:
    case FourCC{"LAND"}.value:
    case FourCC{"NAVM"}.value:
    case FourCC{"LGTM"}.value:
        places_.collect(merged, ctx, data, form_ctx);
        break;
    case FourCC{"LIGH"}.value:
    case FourCC{"MATO"}.value:
    case FourCC{"ADDN"}.value:
    case FourCC{"TXST"}.value:
    case FourCC{"LTEX"}.value:
    case FourCC{"GRAS"}.value:
        bases_.collect(merged, data, form_ctx);
        break;
    case FourCC{"WRLD"}.value:
    case FourCC{"WATR"}.value:
    case FourCC{"CLMT"}.value:
    case FourCC{"WTHR"}.value:
    case FourCC{"SPGD"}.value:
    case FourCC{"REGN"}.value:
    case FourCC{"IMGS"}.value:
        environment_.collect(merged, data, form_ctx);
        break;
    case FourCC{"QUST"}.value:
    case FourCC{"GLOB"}.value:
        quests_.collect(merged, data, form_ctx);
        break;
    case FourCC{"RACE"}.value:
    case FourCC{"ARMA"}.value:
    case FourCC{"OTFT"}.value:
    case FourCC{"LVLI"}.value:
        actors_.collect(merged, data, form_ctx);
        break;
    case FourCC{"PACK"}.value:
    case FourCC{"FLST"}.value:
        ai_.collect(merged, data, form_ctx);
        break;
    case FourCC{"INFO"}.value:
        break;
    case FourCC{"NPC_"}.value:
    case FourCC{"ARMO"}.value:
    case FourCC{"LVLN"}.value: {
        // These are bases too (placed armor, scripted NPCs); read twice.
        io::SpanReader copy = data;
        actors_.collect(merged, copy, form_ctx);
        bases_.collect_generic(merged, data);
        break;
    }
    default:
        bases_.collect_generic(merged, data);
        break;
    }
}

} // namespace bethconv::pack::detail
