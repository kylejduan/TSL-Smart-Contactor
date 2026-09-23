#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
namespace tsl {
// Bounded, allocation-free JSON view. Rejects duplicate/unescaped ambiguous keys,
// trailing data, invalid numbers, excessive depth/tokens and control characters.
class Json {
public:
    enum class Type : uint8_t { Object, Array, String, Number, Bool, Null };
    bool parse(std::string_view input);
    int get(int object, std::string_view key) const;
    int at(int array, size_t n) const;
    bool string(int token, char* dst, size_t capacity) const;
    bool equal(int token, std::string_view value) const;
    bool number(int token, double& value) const;
    bool number_text(int token, char* dst, size_t capacity) const;
    bool integer(int token, int64_t& value) const;
    bool boolean(int token, bool& value) const;
    bool is(int token, Type type) const;
private:
    struct Token { uint32_t start = 0, len = 0; uint16_t end = 0; Type type = Type::Null; };
    std::array<Token, 512> tokens_{};
    std::string_view input_;
    size_t pos_ = 0, count_ = 0;
    void ws();
    int value(unsigned depth);
    bool quoted(Token& t);
};
}
