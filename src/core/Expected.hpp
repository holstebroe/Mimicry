// mimicry::expected / mimicry::unexpected.
//
// The project builds as C++20. This header gives the subset of std::expected (C++23) that we use:
// construct from a value or from unexpected(e), has_value / operator bool, value, error, operator* / ->.
// When the standard library provides std::expected (C++23 mode) it is used instead, so call sites
// never change. Docs: docs/spec/01_ARCHITECTURE.md section 3.
#pragma once

#include <type_traits>
#include <utility>
#include <variant>

#if defined(__has_include)
#if __has_include(<version>)
#include <version>
#endif
#endif

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
#include <expected>
namespace mimicry {
template <class T, class E> using expected = std::expected<T, E>;
template <class E> using unexpected = std::unexpected<E>;
} // namespace mimicry
#else
namespace mimicry {

template <class E> class unexpected {
public:
    explicit unexpected(E e) : err_(std::move(e)) {}
    [[nodiscard]] const E& error() const& { return err_; }
    [[nodiscard]] E& error() & { return err_; }
    [[nodiscard]] E&& error() && { return std::move(err_); }

private:
    E err_;
};
template <class E> unexpected(E) -> unexpected<E>;

namespace detail {
struct Void {};
} // namespace detail

template <class T, class E> class expected {
    using Stored = std::conditional_t<std::is_void_v<T>, detail::Void, T>;

public:
    expected() requires(std::is_void_v<T> || std::is_default_constructible_v<T>) : v_(std::in_place_index<0>) {}
    template <class U = T>
        requires(!std::is_void_v<T> && std::is_convertible_v<U &&, T> &&
                 !std::is_same_v<std::remove_cvref_t<U>, expected> &&
                 !std::is_same_v<std::remove_cvref_t<U>, unexpected<E>>)
    expected(U&& value) : v_(std::in_place_index<0>, std::forward<U>(value)) {}
    expected(unexpected<E> e) : v_(std::in_place_index<1>, std::move(e).error()) {}

    [[nodiscard]] bool has_value() const { return v_.index() == 0; }
    explicit operator bool() const { return has_value(); }

    [[nodiscard]] Stored& value() & requires(!std::is_void_v<T>) { return std::get<0>(v_); }
    [[nodiscard]] const Stored& value() const& requires(!std::is_void_v<T>) { return std::get<0>(v_); }
    [[nodiscard]] Stored& operator*() & requires(!std::is_void_v<T>) { return std::get<0>(v_); }
    [[nodiscard]] const Stored& operator*() const& requires(!std::is_void_v<T>) { return std::get<0>(v_); }
    [[nodiscard]] Stored* operator->() requires(!std::is_void_v<T>) { return &std::get<0>(v_); }
    [[nodiscard]] const Stored* operator->() const requires(!std::is_void_v<T>) { return &std::get<0>(v_); }

    [[nodiscard]] E& error() & { return std::get<1>(v_); }
    [[nodiscard]] const E& error() const& { return std::get<1>(v_); }

    template <class U>
        requires(!std::is_void_v<T>)
    friend bool operator==(const expected& x, const U& value) {
        return x.has_value() && *x == value;
    }

private:
    std::variant<Stored, E> v_;
};

} // namespace mimicry
#endif
