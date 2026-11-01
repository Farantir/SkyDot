// SPDX-License-Identifier: GPL-3.0-or-later
//
// The one list of everything that keeps state per reference id. A store that
// is keyed by a reference (or by the id of an effect instance) registers a
// forget function (and a size, for tests); forgetting an id calls every store,
// so a store added later cannot be missed by a hand-kept list. Stores whose
// owner can die before the registry hold the Registration token, which
// removes the entry in its destructor. Nothing here touches Godot.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace skydot::vm {

class ForgetRegistry {
public:
    using Forget = std::function<void(std::uint32_t)>;
    using Size = std::function<std::size_t()>;

private:
    struct Store {
        std::uint64_t id;
        std::string name;
        Forget forget;
        Size size;
    };
    struct State {
        std::vector<Store> stores;
        std::uint64_t next = 1;
    };

public:

    /// A registration; unregisters in its destructor. Move-only.
    class Registration {
    public:
        Registration() = default;
        Registration(const Registration&) = delete;
        Registration& operator=(const Registration&) = delete;
        Registration(Registration&& other) noexcept : state_(std::move(other.state_)), id_(other.id_) { other.state_.reset(); }
        Registration& operator=(Registration&& other) noexcept {
            if (this != &other) {
                reset();
                state_ = std::move(other.state_);
                other.state_.reset();
                id_ = other.id_;
            }
            return *this;
        }
        ~Registration() { reset(); }
        void reset() noexcept {
            if (const auto state = state_.lock()) {
                std::erase_if(state->stores, [id = id_](const Store& s) { return s.id == id; });
            }
            state_.reset();
        }

    private:
        friend class ForgetRegistry;
        Registration(const std::shared_ptr<State>& state, std::uint64_t id) : state_(state), id_(id) {}
        std::weak_ptr<State> state_; ///< The registry may be gone first (a new VM): then there is nothing to undo.
        std::uint64_t id_ = 0;
    };

    ForgetRegistry() = default;
    ForgetRegistry(const ForgetRegistry&) = delete;
    ForgetRegistry& operator=(const ForgetRegistry&) = delete;

    /// Register a store: `forget(ref)` drops what it holds for `ref`, `size()` says how many entries it holds.
    [[nodiscard]] Registration add(std::string_view name, Forget forget, Size size = {}) {
        const std::uint64_t id = state_->next++;
        state_->stores.push_back({id, std::string(name), std::move(forget), std::move(size)});
        return Registration(state_, id);
    }
    /// The same for a store that lives as long as the registry (or longer and never forgets after it): no token.
    void add_owned(std::string_view name, Forget forget, Size size = {}) {
        state_->stores.push_back({state_->next++, std::string(name), std::move(forget), std::move(size)});
    }

    /// Every store forgets `ref`. A store may register or unregister others meanwhile (the list is walked by index).
    void forget(std::uint32_t ref) const {
        for (std::size_t i = 0; i < state_->stores.size(); ++i) {
            const Forget f = state_->stores[i].forget; // a copy: the store may be removed while it runs
            if (f) {
                f(ref);
            }
        }
    }

    /// (name, size) of every store that reports one, in registration order. Stores of one name add up.
    [[nodiscard]] std::vector<std::pair<std::string, std::size_t>> sizes() const {
        std::vector<std::pair<std::string, std::size_t>> out;
        for (const auto& s : state_->stores) {
            if (!s.size) {
                continue;
            }
            const auto it = std::ranges::find(out, s.name, &std::pair<std::string, std::size_t>::first);
            if (it != out.end()) {
                it->second += s.size();
            } else {
                out.emplace_back(s.name, s.size());
            }
        }
        return out;
    }
    [[nodiscard]] std::size_t count() const noexcept { return state_->stores.size(); }

private:
    std::shared_ptr<State> state_ = std::make_shared<State>();
};

} // namespace skydot::vm
