// SPDX-License-Identifier: GPL-3.0-or-later
#include "session/clock.hpp"

#include <algorithm>
#include <cmath>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>

using godot::Dictionary;
using godot::String;

namespace skydot {

void SkydotClock::_bind_methods() {
    using godot::ClassDB;
    using godot::D_METHOD;
    using godot::PropertyInfo;
    ClassDB::bind_method(D_METHOD("setup", "start_hour", "time_scale", "still"), &SkydotClock::setup);
    ClassDB::bind_method(D_METHOD("get_hour"), &SkydotClock::get_hour);
    ClassDB::bind_method(D_METHOD("get_day"), &SkydotClock::get_day);
    ClassDB::bind_method(D_METHOD("set_time_scale", "scale"), &SkydotClock::set_time_scale);
    ClassDB::bind_method(D_METHOD("get_time_scale"), &SkydotClock::get_time_scale);
    ADD_PROPERTY(PropertyInfo(godot::Variant::FLOAT, "hour"), "", "get_hour");
    ADD_PROPERTY(PropertyInfo(godot::Variant::INT, "day"), "", "get_day");
    ADD_PROPERTY(PropertyInfo(godot::Variant::FLOAT, "time_scale"), "set_time_scale", "get_time_scale");
    ClassDB::bind_method(D_METHOD("attach_ai", "ai"), &SkydotClock::attach_ai);
    ClassDB::bind_method(D_METHOD("get_weather"), &SkydotClock::get_weather);
    ADD_PROPERTY(PropertyInfo(godot::Variant::OBJECT, "weather", godot::PROPERTY_HINT_NODE_TYPE,
                              "SkydotWeather"),
                 "", "get_weather");
    ClassDB::bind_method(D_METHOD("has_weather"), &SkydotClock::has_weather);
    ClassDB::bind_method(D_METHOD("configure", "node"), &SkydotClock::configure);
    ClassDB::bind_method(D_METHOD("bind_weather", "node"), &SkydotClock::bind_weather);
    ClassDB::bind_method(D_METHOD("release_weather"), &SkydotClock::release_weather);
    ClassDB::bind_method(D_METHOD("sync", "benchmarking"), &SkydotClock::sync);
    ClassDB::bind_method(D_METHOD("shift", "hours"), &SkydotClock::shift);
    ClassDB::bind_method(D_METHOD("get_game_days"), &SkydotClock::get_game_days);
    ClassDB::bind_method(D_METHOD("set_game_days", "days"), &SkydotClock::set_game_days);
    ClassDB::bind_method(D_METHOD("set_hour", "hour"), &SkydotClock::set_hour);
    ClassDB::bind_method(D_METHOD("write_section", "writer"), &SkydotClock::write_section);
    ClassDB::bind_method(D_METHOD("prepare_load", "reader"), &SkydotClock::prepare_load);
    ClassDB::bind_method(D_METHOD("commit_load"), &SkydotClock::commit_load);
    ClassDB::bind_method(D_METHOD("cancel_load"), &SkydotClock::cancel_load);
    ClassDB::bind_method(D_METHOD("get_last_error"), &SkydotClock::get_last_error);
    ClassDB::bind_method(D_METHOD("take_saved_weather"), &SkydotClock::take_saved_weather);
    ClassDB::bind_method(D_METHOD("describe"), &SkydotClock::describe);
}

void SkydotClock::setup(double start_hour, double time_scale, bool still) {
    time_ = session::GameTime(start_hour, time_scale, still);
}

SkydotWeather* SkydotClock::get_weather() const {
    return godot::Object::cast_to<SkydotWeather>(godot::ObjectDB::get_instance(weather_));
}

double SkydotClock::get_hour() const {
    const auto* weather = get_weather();
    return weather != nullptr ? weather->get_hour() : time_.hour();
}

std::int64_t SkydotClock::get_day() const {
    const auto* weather = get_weather();
    return weather != nullptr ? weather->get_day() : time_.day();
}

void SkydotClock::attach_ai(const godot::Ref<SkydotAi>& ai) {
    ai_ = ai;
    const session::AiTime at = time_.for_ai();
    ai_->set_days(at.days);
    ai_->set_time_scale(at.speed);
}

void SkydotClock::configure(SkydotWeather* node) const {
    const session::WeatherTime at = time_.for_weather();
    node->set_hour(at.hour);
    node->set_day(at.day);
    node->set_time_scale(at.speed);
}

void SkydotClock::bind_weather(SkydotWeather* node) { weather_ = node->get_instance_id(); }

void SkydotClock::release_weather() {
    if (const auto* weather = get_weather()) {
        time_.keep(weather->get_hour(), weather->get_day());
    }
    weather_ = 0;
}

void SkydotClock::sync(bool benchmarking) {
    if (ai_.is_null()) {
        return;
    }
    if (const auto* weather = get_weather()) {
        const session::AiTime at = session::GameTime::follow_weather(weather->get_hour(), weather->get_day());
        ai_->set_time_scale(at.speed);
        ai_->set_days(at.days);
    } else {
        ai_->set_time_scale(time_.speed(benchmarking));
        time_.keep_days(ai_->get_days());
    }
}

String SkydotClock::shift(double hours) {
    if (auto* weather = get_weather()) {
        weather->set_hour(session::shifted_hour(weather->get_hour(), hours));
        return String::utf8(session::hour_text(weather->get_hour()).c_str());
    }
    if (ai_.is_valid()) {
        const double moved = session::shifted_days(ai_->get_days(), hours);
        ai_->set_days(moved);
        time_.keep(session::hour_of(moved), time_.day());
        return String::utf8(session::hour_text(time_.hour()).c_str());
    }
    return String();
}

double SkydotClock::get_game_days() const { return session::days_of(get_hour(), get_day()); }

void SkydotClock::set_game_days(double days) {
    days = std::max(days, 0.0);
    if (auto* weather = get_weather()) {
        weather->set_hour(session::hour_of(days));
        weather->set_day(session::day_of(days));
    }
    if (ai_.is_valid()) {
        ai_->set_days(days);
    }
    time_.keep_days(days);
}

void SkydotClock::set_hour(double hour) {
    set_game_days(session::days_of(std::fmod(std::fmod(hour, 24.0) + 24.0, 24.0), get_day()));
}

namespace {
constexpr std::uint32_t k_clock_section_version = 1;
}

void SkydotClock::write_section(const godot::Ref<SkydotSaveWriter>& writer) const {
    save::ByteWriter w;
    w.f64(get_game_days());
    w.f64(get_time_scale());
    const auto* weather = get_weather();
    w.u32(static_cast<std::uint32_t>(weather != nullptr ? weather->get_weather() : saved_weather_));
    writer->core().add("CLCK", k_clock_section_version, w.out);
}

godot::Error SkydotClock::prepare_load(const godot::Ref<SkydotSaveReader>& reader) {
    pending_ = false;
    const auto* section = reader->file().find("CLCK");
    if (section == nullptr) {
        error_ = "the save lacks its CLCK section";
        return godot::ERR_FILE_CORRUPT;
    }
    if (section->version != k_clock_section_version) {
        error_ = "section CLCK has a version this engine does not read";
        return godot::ERR_FILE_CORRUPT;
    }
    auto r = reader->file().open(*section);
    double days = 0.0, scale = 0.0;
    std::uint32_t weather = 0;
    if (!r.f64(days) || !r.f64(scale) || !r.u32(weather) || !r.at_end() || !std::isfinite(days) ||
        !std::isfinite(scale) || days < 0.0 || scale < 0.0) {
        error_ = "the CLCK section of the save is damaged";
        return godot::ERR_FILE_CORRUPT;
    }
    pending_days_ = days;
    pending_scale_ = scale;
    pending_weather_ = weather;
    pending_ = true;
    error_ = String();
    return godot::OK;
}

godot::Error SkydotClock::commit_load() {
    if (!pending_) {
        error_ = "no save was prepared";
        return godot::ERR_UNCONFIGURED;
    }
    pending_ = false;
    time_.set_scale(pending_scale_);
    set_game_days(pending_days_);
    if (auto* weather = get_weather()) {
        weather->set_time_scale(time_.speed(false));
        if (pending_weather_ != 0) {
            weather->set_weather(pending_weather_, 0.0);
        }
        saved_weather_ = 0;
    } else {
        saved_weather_ = pending_weather_;
    }
    return godot::OK;
}

std::int64_t SkydotClock::take_saved_weather() {
    const std::int64_t weather = saved_weather_;
    saved_weather_ = 0;
    return weather;
}

Dictionary SkydotClock::describe() const {
    Dictionary out;
    if (const auto* weather = get_weather()) {
        out["hour"] = weather->get_hour();
        out["day"] = weather->get_day();
        out["time_scale"] = weather->get_time_scale();
    } else {
        out["hour"] = time_.hour(); // inside: kept for when one leaves
        out["day"] = time_.day();
    }
    return out;
}

} // namespace skydot
