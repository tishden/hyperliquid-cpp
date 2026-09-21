// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Denis Tishkov <denis8825@ya.ru>
#pragma once

#include <utility>
#include <variant>

#include "hl/core/Types.h"

namespace hl {

/**
 * @brief Value-or-error returned by asynchronous request callbacks.
 *
 * A minimal `std::expected` substitute so the library stays C++20.
 */
template <typename T>
class Result {
public:
    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}  // NOLINT(google-explicit-constructor)
    Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}  // NOLINT(google-explicit-constructor)

    [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
    [[nodiscard]] T& value() & { return std::get<0>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }
    [[nodiscard]] const Error& error() const& { return std::get<1>(storage_); }

    [[nodiscard]] const T* operator->() const { return &value(); }
    [[nodiscard]] const T& operator*() const& { return value(); }

private:
    std::variant<T, Error> storage_;
};

}  // namespace hl
