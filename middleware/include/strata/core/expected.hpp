#pragma once

#include <strata/core/types.hpp>

#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace strata {

/// Tag wrapper for constructing an error-state Expected<T, E>.
template <typename E>
struct Unexpected {
    E error;

    constexpr explicit Unexpected(E err) noexcept(std::is_nothrow_move_constructible_v<E>)
        : error(std::move(err)) {}
};

template <typename E>
Unexpected(E) -> Unexpected<E>;

/// Zero-exception, zero-heap C++20 Expected<T, E> (flight-safe equivalent of C++23 std::expected).
template <typename T, typename E = Status>
class [[nodiscard]] Expected {
public:
    static_assert(!std::is_reference_v<T>, "Expected<T, E> does not support reference types");
    static_assert(!std::is_reference_v<E>, "Expected<T, E> error type must not be a reference");

    constexpr Expected(const T& val) noexcept(std::is_nothrow_copy_constructible_v<T>)
        requires std::is_copy_constructible_v<T>
        : has_val_(true) {
        std::construct_at(&storage_.val, val);
    }

    constexpr Expected(T&& val) noexcept(std::is_nothrow_move_constructible_v<T>)
        requires std::is_move_constructible_v<T>
        : has_val_(true) {
        std::construct_at(&storage_.val, std::move(val));
    }

    constexpr Expected(Unexpected<E> unexp) noexcept(std::is_nothrow_move_constructible_v<E>)
        : has_val_(false) {
        std::construct_at(&storage_.err, std::move(unexp.error));
    }

    constexpr Expected(const Expected& other) noexcept(
        std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_copy_constructible_v<E>)
        requires(std::is_copy_constructible_v<T> && std::is_copy_constructible_v<E>)
        : has_val_(other.has_val_) {
        if (has_val_) {
            std::construct_at(&storage_.val, other.storage_.val);
        } else {
            std::construct_at(&storage_.err, other.storage_.err);
        }
    }

    constexpr Expected(Expected&& other) noexcept(
        std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_constructible_v<E>)
        requires(std::is_move_constructible_v<T> && std::is_move_constructible_v<E>)
        : has_val_(other.has_val_) {
        if (has_val_) {
            std::construct_at(&storage_.val, std::move(other.storage_.val));
        } else {
            std::construct_at(&storage_.err, std::move(other.storage_.err));
        }
    }

    constexpr Expected& operator=(Expected&& other) noexcept(
        std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_constructible_v<E>)
        requires(std::is_move_constructible_v<T> && std::is_move_constructible_v<E>)
    {
        if (this != &other) {
            destroy();
            has_val_ = other.has_val_;
            if (has_val_) {
                std::construct_at(&storage_.val, std::move(other.storage_.val));
            } else {
                std::construct_at(&storage_.err, std::move(other.storage_.err));
            }
        }
        return *this;
    }

    constexpr ~Expected() noexcept { destroy(); }

    [[nodiscard]] constexpr bool has_value() const noexcept { return has_val_; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_val_; }

    [[nodiscard]] constexpr T& value() & noexcept { return storage_.val; }
    [[nodiscard]] constexpr const T& value() const& noexcept { return storage_.val; }
    [[nodiscard]] constexpr T&& value() && noexcept { return std::move(storage_.val); }

    [[nodiscard]] constexpr T& operator*() & noexcept { return storage_.val; }
    [[nodiscard]] constexpr const T& operator*() const& noexcept { return storage_.val; }
    [[nodiscard]] constexpr T&& operator*() && noexcept { return std::move(storage_.val); }

    [[nodiscard]] constexpr T* operator->() noexcept { return &storage_.val; }
    [[nodiscard]] constexpr const T* operator->() const noexcept { return &storage_.val; }

    [[nodiscard]] constexpr E error() const noexcept { return storage_.err; }

    [[nodiscard]] constexpr T value_or(T fallback) const noexcept
        requires std::is_copy_constructible_v<T>
    {
        return has_val_ ? storage_.val : std::move(fallback);
    }

private:
    constexpr void destroy() noexcept {
        if (has_val_) {
            std::destroy_at(&storage_.val);
        } else {
            std::destroy_at(&storage_.err);
        }
    }

    union Storage {
        T val;
        E err;

        constexpr Storage() noexcept {}
        constexpr ~Storage() noexcept {}
    } storage_;
    bool has_val_{false};
};

/// Specialization for void success type.
template <typename E>
class [[nodiscard]] Expected<void, E> {
public:
    constexpr Expected() noexcept : has_val_(true) {}
    constexpr Expected(Unexpected<E> unexp) noexcept : err_(std::move(unexp.error)), has_val_(false) {}

    [[nodiscard]] constexpr bool has_value() const noexcept { return has_val_; }
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return has_val_; }
    [[nodiscard]] constexpr E error() const noexcept { return err_; }

private:
    E    err_{};
    bool has_val_{true};
};

} // namespace strata
