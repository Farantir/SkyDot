// SPDX-License-Identifier: GPL-3.0-or-later
#include "bethconv/install/vdf.hpp"

#include <fstream>
#include <iterator>

namespace bethconv::install {
namespace {

/// Steam's own files nest four or five levels; anything deeper is not one.
constexpr std::size_t k_max_depth = 64;

[[nodiscard]] char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) {
            return false;
        }
    }
    return true;
}

class Parser {
public:
    Parser(std::string_view text, std::string origin) : text_(text), origin_(std::move(origin)) {
        // UTF-8 BOM.
        if (text_.starts_with("\xEF\xBB\xBF")) {
            pos_ = 3;
        }
    }

    io::ParseResult<VdfNode> parse() {
        VdfNode root;
        root.is_object = true;
        if (auto ok = parse_entries(root, 0, false); !ok) {
            return std::unexpected(ok.error());
        }
        return root;
    }

private:
    enum class Token : std::uint8_t { string, open, close, end };

    io::ParseResult<void> parse_entries(VdfNode& parent, std::size_t depth, bool nested) {
        if (depth > k_max_depth) {
            return fail(io::ErrorKind::too_large, "nested deeper than 64 levels");
        }
        for (;;) {
            std::string key;
            auto token = next(key);
            if (!token) {
                return std::unexpected(token.error());
            }
            if (*token == Token::end) {
                if (nested) {
                    return fail(io::ErrorKind::truncated, "missing '}'");
                }
                return {};
            }
            if (*token == Token::close) {
                if (!nested) {
                    return fail(io::ErrorKind::bad_value, "'}' without '{'");
                }
                return {};
            }
            if (*token == Token::open) {
                return fail(io::ErrorKind::bad_value, "'{' where a key was expected");
            }

            VdfNode node;
            node.key = std::move(key);
            std::string value;
            auto value_token = next(value);
            if (!value_token) {
                return std::unexpected(value_token.error());
            }
            switch (*value_token) {
            case Token::string:
                node.value = std::move(value);
                break;
            case Token::open:
                node.is_object = true;
                if (auto ok = parse_entries(node, depth + 1, true); !ok) {
                    return ok;
                }
                break;
            case Token::close:
            case Token::end:
                return fail(io::ErrorKind::truncated, "key '" + node.key + "' has no value");
            }
            parent.children.push_back(std::move(node));
        }
    }

    void skip_space_and_comments() {
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else if (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '/') {
                while (pos_ < text_.size() && text_[pos_] != '\n') {
                    ++pos_;
                }
            } else {
                return;
            }
        }
    }

    io::ParseResult<Token> next(std::string& out) {
        skip_space_and_comments();
        if (pos_ >= text_.size()) {
            return Token::end;
        }
        const char c = text_[pos_];
        if (c == '{') {
            ++pos_;
            return Token::open;
        }
        if (c == '}') {
            ++pos_;
            return Token::close;
        }
        if (c == '"') {
            ++pos_;
            while (pos_ < text_.size()) {
                const char d = text_[pos_++];
                if (d == '"') {
                    return Token::string;
                }
                if (d == '\\' && pos_ < text_.size()) {
                    const char e = text_[pos_++];
                    switch (e) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    default: out.push_back(e); break;  // \\ and \" and anything else
                    }
                    continue;
                }
                out.push_back(d);
            }
            return fail(io::ErrorKind::unterminated, "string without closing quote");
        }
        // Bare token: up to whitespace, a brace or a quote.
        while (pos_ < text_.size()) {
            const char d = text_[pos_];
            if (d == ' ' || d == '\t' || d == '\r' || d == '\n' || d == '{' || d == '}' ||
                d == '"') {
                break;
            }
            out.push_back(d);
            ++pos_;
        }
        return Token::string;
    }

    [[nodiscard]] std::unexpected<io::ParseError> fail(io::ErrorKind kind, std::string detail) const {
        return std::unexpected(
            io::ParseError{.origin = origin_, .offset = pos_, .kind = kind, .detail = std::move(detail)});
    }

    std::string_view text_;
    std::string origin_;
    std::size_t pos_ = 0;
};

} // namespace

const VdfNode* VdfNode::find(std::string_view name) const noexcept {
    for (const auto& child : children) {
        if (iequals(child.key, name)) {
            return &child;
        }
    }
    return nullptr;
}

std::string_view VdfNode::get(std::string_view name) const noexcept {
    const auto* child = find(name);
    return (child != nullptr && !child->is_object) ? std::string_view(child->value)
                                                   : std::string_view();
}

io::ParseResult<VdfNode> parse_vdf(std::string_view text) {
    return Parser(text, "vdf").parse();
}

io::ParseResult<VdfNode> read_vdf(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::unexpected(io::ParseError{.origin = path.string(),
                                              .kind = io::ErrorKind::truncated,
                                              .detail = "cannot open"});
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return Parser(text, path.string()).parse();
}

} // namespace bethconv::install
