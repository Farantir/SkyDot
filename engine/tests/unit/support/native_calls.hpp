// SPDX-License-Identifier: GPL-3.0-or-later
//
// Helpers for natives tests that call a method of an engine class from a
// script: declare it with `native`, call it through a global `wrap`per.
#pragma once

#include "script_builder.hpp"

#include <string>
#include <utility>
#include <vector>

namespace skydot::testing {

/// `Caller.<name>(o, extras...)`: o.<fn>(extras...) returning its result.
inline FunctionSpec wrap(const std::string& name, const std::string& cls, const std::string& fn,
                  std::vector<Param> extra = {}, const std::string& ret = "None") {
    std::vector<Param> params{{"o", cls}};
    params.insert(params.end(), extra.begin(), extra.end());
    std::vector<Param> locals;
    if (ret != "None") {
        locals.push_back({"r", ret});
    }
    auto f = global_function(name, std::move(params), std::move(locals), ret);
    std::vector<Arg> args{ident(fn), ident("o"), ident(ret == "None" ? "::NoneVar" : "r")};
    for (const auto& p : extra) {
        args.push_back(ident(p.name));
    }
    f.add(vm::Op::callmethod, args);
    if (ret != "None") {
        f.add(vm::Op::return_, {ident("r")});
    }
    return f;
}

inline FunctionSpec native(const std::string& name, std::vector<Param> params = {}, const std::string& ret = "None") {
    return native_function(name, std::move(params), ret, false);
}

} // namespace skydot::testing
