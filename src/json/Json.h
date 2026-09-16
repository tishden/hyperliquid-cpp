#pragma once

// Private control-plane JSON helper over simdjson's DOM API. Used for info /
// exchange responses (cold path). Every accessor is total: a missing field or a
// type mismatch yields an "absent" Value whose accessors return defaults, so
// response parsers read like the documented schema without error plumbing.

#include <simdjson.h>

#include <cstdint>
#include <string_view>

#include "hl/core/Decimal.h"

namespace hl::json {

class Value {
public:
    Value() = default;
    explicit Value(simdjson::dom::element e) : elem_(e), present_(true) {}

    [[nodiscard]] bool present() const noexcept { return present_; }
    [[nodiscard]] bool isNull() const noexcept { return present_ && elem_.is_null(); }
    [[nodiscard]] bool isObject() const noexcept { return present_ && elem_.is_object(); }
    [[nodiscard]] bool isArray() const noexcept { return present_ && elem_.is_array(); }
    [[nodiscard]] bool isString() const noexcept { return present_ && elem_.is_string(); }

    [[nodiscard]] Value field(std::string_view key) const noexcept {
        if (!isObject()) {
            return {};
        }
        auto r = elem_.get_object().at_key(key);
        return r.error() ? Value{} : Value{r.value_unsafe()};
    }

    [[nodiscard]] std::string_view asString(std::string_view fallback = {}) const noexcept {
        if (!isString()) {
            return fallback;
        }
        return elem_.get_string().value_unsafe();
    }

    [[nodiscard]] std::int64_t asInt(std::int64_t fallback = 0) const noexcept {
        if (!present_) {
            return fallback;
        }
        if (auto r = elem_.get_int64(); !r.error()) {
            return r.value_unsafe();
        }
        if (auto r = elem_.get_uint64(); !r.error()) {
            return static_cast<std::int64_t>(r.value_unsafe());
        }
        if (auto r = elem_.get_double(); !r.error()) {
            return static_cast<std::int64_t>(r.value_unsafe());
        }
        return fallback;
    }

    [[nodiscard]] std::uint64_t asUint(std::uint64_t fallback = 0) const noexcept {
        if (!present_) {
            return fallback;
        }
        if (auto r = elem_.get_uint64(); !r.error()) {
            return r.value_unsafe();
        }
        return static_cast<std::uint64_t>(asInt(static_cast<std::int64_t>(fallback)));
    }

    [[nodiscard]] bool asBool(bool fallback = false) const noexcept {
        if (!present_) {
            return fallback;
        }
        auto r = elem_.get_bool();
        return r.error() ? fallback : r.value_unsafe();
    }

    /// Decimal from a JSON string ("1.5") or number (1.5).
    [[nodiscard]] Decimal asDecimal() const noexcept {
        if (isString()) {
            return Decimal::parseOrZero(elem_.get_string().value_unsafe());
        }
        if (!present_) {
            return {};
        }
        if (auto r = elem_.get_int64(); !r.error()) {
            return Decimal::fromInt(r.value_unsafe());
        }
        if (auto r = elem_.get_double(); !r.error()) {
            return Decimal::fromDouble(r.value_unsafe());
        }
        return {};
    }

    /// Minified JSON text of this value.
    [[nodiscard]] std::string dump() const { return present_ ? simdjson::minify(elem_) : std::string{}; }

    class ArrayRange {
    public:
        ArrayRange() = default;
        explicit ArrayRange(simdjson::dom::array a) : arr_(a), valid_(true) {}
        class Iterator {
        public:
            explicit Iterator(simdjson::dom::array::iterator it) : it_(it) {}
            Value operator*() const { return Value{*it_}; }
            Iterator& operator++() {
                ++it_;
                return *this;
            }
            bool operator!=(const Iterator& o) const { return it_ != o.it_; }

        private:
            simdjson::dom::array::iterator it_;
        };
        [[nodiscard]] Iterator begin() const { return valid_ ? Iterator{arr_.begin()} : Iterator{{}}; }
        [[nodiscard]] Iterator end() const { return valid_ ? Iterator{arr_.end()} : Iterator{{}}; }
        [[nodiscard]] std::size_t size() const { return valid_ ? arr_.size() : 0; }

    private:
        simdjson::dom::array arr_{};
        bool valid_{false};
    };

    [[nodiscard]] ArrayRange array() const noexcept {
        return isArray() ? ArrayRange{elem_.get_array().value_unsafe()} : ArrayRange{};
    }

private:
    simdjson::dom::element elem_{};
    bool present_{false};
};

class Document {
public:
    /// Parse text (copied into a padded buffer). Returns false on invalid JSON.
    [[nodiscard]] bool parse(std::string_view text) {
        auto r = parser_.parse(text.data(), text.size(), true);
        if (r.error()) {
            return false;
        }
        root_ = Value{r.value_unsafe()};
        return true;
    }
    [[nodiscard]] Value root() const noexcept { return root_; }

private:
    simdjson::dom::parser parser_;
    Value root_{};
};

}  // namespace hl::json
