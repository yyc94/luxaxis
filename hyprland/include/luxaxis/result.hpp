#pragma once

#include <string>
#include <utility>
#include <variant>

namespace luxaxis {

struct Error {
    std::string source;
    std::string message;

    friend bool operator==(const Error&, const Error&) = default;
};

template <typename T>
class Result {
  public:
    Result(T value) : data_(std::move(value)) {}
    Result(Error error) : data_(std::move(error)) {}

    [[nodiscard]] bool hasValue() const {
        return std::holds_alternative<T>(data_);
    }

    explicit operator bool() const {
        return hasValue();
    }

    [[nodiscard]] const T& value() const {
        return std::get<T>(data_);
    }

    [[nodiscard]] T& value() {
        return std::get<T>(data_);
    }

    [[nodiscard]] const Error& error() const {
        return std::get<Error>(data_);
    }

  private:
    std::variant<T, Error> data_;
};

} // namespace luxaxis
