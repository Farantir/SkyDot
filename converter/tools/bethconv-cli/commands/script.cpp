// SPDX-License-Identifier: GPL-3.0-or-later
#include "commands/commands.hpp"
#include "common.hpp"

#include "bethconv/archive/archive_set.hpp"
#include "bethconv/script/pex.hpp"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace bethconv::cli {
namespace {

/// Decode every `.pex` in the sources (or the named ones) and count what they
/// contain; `dump` prints each one's disassembly instead.
int cmd_script(const std::vector<std::filesystem::path>& sources, std::vector<std::string> vpaths,
               const std::string& filter, bool dump) {
    bethconv::archive::ArchiveSet set;
    mount_all(set, sources);
    if (vpaths.empty()) {
        set.for_each([&](const bethconv::archive::Resolution& entry) {
            if (entry.vpath.ends_with(".pex") &&
                (filter.empty() || entry.vpath.find(filter) != std::string::npos)) {
                vpaths.push_back(entry.vpath);
            }
        });
        std::ranges::sort(vpaths);
    }
    std::size_t scripts = 0;
    std::size_t failed = 0;
    std::size_t objects = 0;
    std::size_t functions = 0;
    std::size_t natives = 0;
    std::size_t instructions = 0;
    std::size_t with_lines = 0;
    std::size_t debug = 0;
    std::array<std::size_t, bethconv::script::k_pex_op_count> ops{};
    const auto started = std::chrono::steady_clock::now();
    for (const auto& vpath : vpaths) {
        auto bytes = set.read(vpath);
        if (!bytes) {
            ++failed;
            std::fprintf(stderr, "error: %s\n", bytes.error().to_string().c_str());
            continue;
        }
        auto script = bethconv::script::read_pex_script(*bytes, vpath);
        ++scripts;
        if (!script) {
            ++failed;
            std::fprintf(stderr, "error: %s\n", script.error().to_string().c_str());
            continue;
        }
        if (dump) {
            std::printf("; %s\n%s\n", vpath.c_str(), bethconv::script::disassemble(*script).c_str());
            continue;
        }
        debug += script->has_debug_info ? 1U : 0U;
        const auto count = [&](const bethconv::script::PexFunction& f) {
            ++functions;
            natives += f.is_native() ? 1U : 0U;
            instructions += f.opcodes.size();
            with_lines += f.lines.empty() ? 0U : 1U;
            for (const auto op : f.opcodes) {
                ++ops[static_cast<std::size_t>(op)];
            }
        };
        for (const auto& o : script->objects) {
            ++objects;
            for (const auto& p : o.properties) {
                if (p.getter) {
                    count(*p.getter);
                }
                if (p.setter) {
                    count(*p.setter);
                }
            }
            for (const auto& state : o.states) {
                for (const auto& f : state.functions) {
                    count(f);
                }
            }
        }
    }
    if (dump) {
        return failed == 0 ? 0 : 1;
    }
    const auto elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("%zu scripts, %zu failed, %zu with debug info | %zu objects, %zu functions "
                "(%zu native, %zu with line numbers), %zu instructions | %.1f s\n",
                scripts, failed, debug, objects, functions, natives, with_lines, instructions,
                elapsed);
    std::printf("opcodes:");
    for (std::size_t i = 0; i < ops.size(); ++i) {
        std::printf(" %s %zu", std::string(bethconv::script::op_info(
                                               static_cast<bethconv::script::PexOp>(i)).name)
                                   .c_str(),
                    ops[i]);
    }
    std::printf("\n");
    return failed == 0 ? 0 : 1;
}

struct ScriptArgs {
    std::vector<std::filesystem::path> sources;
    std::vector<std::string> vpaths;
    std::string filter;
    bool dump = false;
};

} // namespace

void register_script(CLI::App& app) {
    auto args = std::make_shared<ScriptArgs>();
    auto* script_cmd = app.add_subcommand("script", "Decode compiled Papyrus scripts");
    script_cmd->add_option("--source", args->sources, "Archive or directory to mount (repeatable)")
        ->required();
    script_cmd->add_option("vpath", args->vpaths, "Scripts to decode; default every .pex");
    script_cmd->add_option("--filter", args->filter, "Only paths containing this substring");
    script_cmd->add_flag("--dump", args->dump, "Print each script's disassembly");
    script_cmd->callback([args] {
        set_exit_status(cmd_script(args->sources, args->vpaths, args->filter, args->dump));
    });
}

} // namespace bethconv::cli
