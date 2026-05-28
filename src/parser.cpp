// Parser.cpp -- Scheme source reader.
// Direct port of pyscheme/Parser.py.
#include "Parser.h"
#include "rational.h"
#include <cassert>
#include <cctype>
#include <cmath>
#include <optional>
#include <unordered_map>
#include <variant>

// ── Delimiter helpers ─────────────────────────────────────────────────────────
// Port of Parser.py _TOKEN_RE boundary set [\s()\[\]'"`,;|].

static bool is_delim(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' ||
           c == '(' || c == ')' || c == '[' || c == ']' ||
           c == '\'' || c == '"' || c == '`' || c == ',' || c == ';' || c == '|';
}

// Identifier delimiter set: also excludes '#' and '\\'.
// Port of IDENT pattern [^\s()\[\]'"`,;|#\\]+.
static bool is_ident_delim(char c) {
    return is_delim(c) || c == '#' || c == '\\';
}

static bool is_hex(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// ── Source-line construction ───────────────────────────────────────────────────
// Port of Parser.py: source.expandtabs().splitlines().

static std::vector<std::string> make_source_lines(const std::string& src) {
    // expandtabs(tabsize=8)
    std::string expanded;
    int col = 0;
    for (char c : src) {
        if (c == '\t') {
            int sp = 8 - (col % 8);
            expanded.append(sp, ' ');
            col += sp;
        } else {
            expanded += c;
            col = (c == '\n') ? 0 : col + 1;
        }
    }
    // splitlines (drops trailing empty line if source ends with \n)
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i <= expanded.size(); ++i) {
        if (i == expanded.size() || expanded[i] == '\n') {
            lines.push_back(expanded.substr(start, i - start));
            start = i + 1;
        }
    }
    if (!lines.empty() && lines.back().empty() &&
        !expanded.empty() && expanded.back() == '\n')
        lines.pop_back();
    return lines;
}

// ── _make_src ─────────────────────────────────────────────────────────────────
// Port of Parser.py _make_src — produces a SourceInfo value (not heap-allocated).

static SourceInfo make_src_val(int line, int col,
                                const std::vector<std::string>& lines,
                                const std::string& filename) {
    std::string sl;
    if (line >= 1 && line <= (int)lines.size())
        sl = lines[line - 1];
    return SourceInfo(line, col, sl, filename);
}

// ── _CHAR_NAMES ───────────────────────────────────────────────────────────────
// Port of Parser.py _CHAR_NAMES.

static const std::unordered_map<std::string, char32_t> CHAR_NAMES = {
    {"space",     U' '},
    {"newline",   U'\n'},
    {"tab",       U'\t'},
    {"return",    U'\r'},
    {"null",      U'\0'},
    {"alarm",     U'\a'},
    {"backspace", U'\b'},
    {"delete",    U'\x7f'},
    {"escape",    U'\x1b'},
};

// ── _decode_char_literal ──────────────────────────────────────────────────────
// Port of Parser.py _decode_char_literal.
// rest is the text after '#\' (NOT including '#\' itself).

static char32_t decode_char_literal(const std::string& rest, const SourceInfo& src) {
    if (rest.size() == 1)
        return (char32_t)(unsigned char)rest[0];
    if (rest.size() >= 2 && rest[0] == 'x') {
        std::string hex_str = rest.substr(1);
        bool all_hex = !hex_str.empty();
        for (char c : hex_str) if (!is_hex(c)) { all_hex = false; break; }
        if (all_hex) {
            unsigned long cp = std::stoul(hex_str, nullptr, 16);
            return (char32_t)cp;
        }
    }
    std::string name = rest;
    for (char& c : name) c = (char)std::tolower((unsigned char)c);
    auto it = CHAR_NAMES.find(name);
    if (it != CHAR_NAMES.end()) return it->second;
    throw SchemeSyntaxError("unknown character name: #\\" + rest, new SourceInfo(src));
}

// ── _STRING_ESCAPES / _decode_string_escapes ──────────────────────────────────
// Port of Parser.py _STRING_ESCAPES and _decode_string_escapes.

static char decode_string_simple_escape(char esc) {
    switch (esc) {
        case 'n': return '\n'; case 't': return '\t'; case 'r': return '\r';
        case '\\': return '\\'; case '"': return '"'; case '|': return '|';
        case 'a': return '\a'; case 'b': return '\b';
        default: return '\0';
    }
}

static void utf8_append(std::string& out, char32_t cp) {
    if (cp < 0x80)        { out += (char)cp; return; }
    if (cp < 0x800)       { out += (char)(0xC0|(cp>>6));  out += (char)(0x80|(cp&0x3F)); return; }
    if (cp < 0x10000)     { out += (char)(0xE0|(cp>>12)); out += (char)(0x80|((cp>>6)&0x3F));
                            out += (char)(0x80|(cp&0x3F)); return; }
    out += (char)(0xF0|(cp>>18)); out += (char)(0x80|((cp>>12)&0x3F));
    out += (char)(0x80|((cp>>6)&0x3F)); out += (char)(0x80|(cp&0x3F));
}

static std::string decode_string_escapes(const std::string& raw, const SourceInfo& src) {
    std::string result;
    size_t i = 0, n = raw.size();
    while (i < n) {
        char c = raw[i];
        if (c != '\\') { result += c; ++i; continue; }
        if (i + 1 >= n)
            throw SchemeSyntaxError("unterminated string escape", new SourceInfo(src));
        char esc = raw[i + 1];
        char simple = decode_string_simple_escape(esc);
        if (simple != '\0') { result += simple; i += 2; continue; }
        if (esc == 'x') {
            size_t j = i + 2;
            while (j < n && is_hex(raw[j])) ++j;
            if (j == i + 2)
                throw SchemeSyntaxError("malformed \\x escape: no hex digits", new SourceInfo(src));
            if (j >= n || raw[j] != ';')
                throw SchemeSyntaxError("malformed \\x escape: missing semicolon", new SourceInfo(src));
            unsigned long cp = std::stoul(raw.substr(i + 2, j - (i + 2)), nullptr, 16);
            utf8_append(result, (char32_t)cp);
            i = j + 1; continue;
        }
        // String line continuation: \<h-space>*<newline><h-space>*
        {
            size_t j = i + 1;
            while (j < n && (raw[j] == ' ' || raw[j] == '\t')) ++j;
            if (j < n && (raw[j] == '\r' || raw[j] == '\n')) {
                if (raw[j] == '\r') ++j;
                if (j < n && raw[j] == '\n') ++j;
                while (j < n && (raw[j] == ' ' || raw[j] == '\t')) ++j;
                i = j; continue;
            }
        }
        throw SchemeSyntaxError(std::string("unknown string escape \\") + esc, new SourceInfo(src));
    }
    return result;
}

// ── _SYMBOL_ESCAPES / _decode_symbol_escapes ──────────────────────────────────
// Port of Parser.py _SYMBOL_ESCAPES and _decode_symbol_escapes.

static char decode_symbol_simple_escape(char esc) {
    switch (esc) {
        case 'a': return '\a'; case 'b': return '\b'; case 't': return '\t';
        case 'n': return '\n'; case 'r': return '\r';
        case '\\': return '\\'; case '|': return '|'; case '"': return '"';
        default: return '\0';
    }
}

static std::string decode_symbol_escapes(const std::string& raw, const SourceInfo& src) {
    std::string result;
    size_t i = 0, n = raw.size();
    while (i < n) {
        char c = raw[i];
        if (c != '\\') { result += c; ++i; continue; }
        if (i + 1 >= n)
            throw SchemeSyntaxError("unterminated symbol escape", new SourceInfo(src));
        char esc = raw[i + 1];
        char simple = decode_symbol_simple_escape(esc);
        if (simple != '\0') { result += simple; i += 2; continue; }
        if (esc == 'x') {
            size_t j = i + 2;
            while (j < n && is_hex(raw[j])) ++j;
            if (j == i + 2)
                throw SchemeSyntaxError("malformed \\x escape: no hex digits", new SourceInfo(src));
            if (j >= n || raw[j] != ';')
                throw SchemeSyntaxError("malformed \\x escape: missing semicolon", new SourceInfo(src));
            unsigned long cp = std::stoul(raw.substr(i + 2, j - (i + 2)), nullptr, 16);
            utf8_append(result, (char32_t)cp);
            i = j + 1; continue;
        }
        throw SchemeSyntaxError(std::string("unknown symbol escape \\") + esc, new SourceInfo(src));
    }
    return result;
}

// ── _starts_like_number ───────────────────────────────────────────────────────
// Port of Parser.py _starts_like_number.

static bool starts_like_number(const std::string& s) {
    if (s.empty()) return false;
    if (s[0] >= '0' && s[0] <= '9') return true;
    if ((s[0] == '+' || s[0] == '-') && s.size() > 1) {
        if (s[1] >= '0' && s[1] <= '9') return true;
        if (s[1] == '.' && s.size() > 2 && s[2] >= '0' && s[2] <= '9') return true;
    }
    if (s[0] == '.' && s.size() > 1 && s[1] >= '0' && s[1] <= '9') return true;
    return false;
}

// ── ComplexComp ───────────────────────────────────────────────────────────────
// Port of _parse_number_for_complex return type: int | _Rat | float.

using ComplexComp = std::variant<int64_t, Rat, double>;

static bool is_exact_comp(const ComplexComp& v) {
    return std::holds_alternative<int64_t>(v) || std::holds_alternative<Rat>(v);
}

static double comp_to_double(const ComplexComp& v) {
    if (auto* ip = std::get_if<int64_t>(&v)) return (double)*ip;
    if (auto* rp = std::get_if<Rat>(&v))    return static_cast<double>(*rp);
    return std::get<double>(v);
}

static Value comp_to_scheme(const ComplexComp& v) {
    if (auto* ip = std::get_if<int64_t>(&v)) return make_integer(*ip);
    if (auto* rp = std::get_if<Rat>(&v)) {
        if (rp->denominator == 1) return make_integer(rp->numerator);
        return make_rational(rp->numerator, rp->denominator);
    }
    return make_real(std::get<double>(v));
}

// ── _parse_number_for_complex ─────────────────────────────────────────────────
// Port of Parser.py _parse_number_for_complex.
// Returns int64_t, Rat, or double; nullopt if not a valid number.

static std::optional<ComplexComp> parse_num_for_complex(const std::string& s) {
    if (s.empty()) return std::nullopt;
    if (s == "+inf.0") return ComplexComp(double( INFINITY));
    if (s == "-inf.0") return ComplexComp(double(-INFINITY));
    if (s == "+nan.0" || s == "-nan.0") return ComplexComp(double(NAN));
    // Try integer
    {
        size_t st = ((s[0] == '+' || s[0] == '-') ? 1 : 0);
        if (st < s.size()) {
            bool ok = true;
            for (size_t k = st; k < s.size(); ++k)
                if (s[k] < '0' || s[k] > '9') { ok = false; break; }
            if (ok) {
                try { return ComplexComp(std::stoll(s)); } catch (...) {}
            }
        }
    }
    // Try rational
    {
        size_t slash = s.find('/');
        if (slash != std::string::npos) {
            try {
                int64_t num = std::stoll(s.substr(0, slash));
                int64_t den = std::stoll(s.substr(slash + 1));
                if (den != 0) return ComplexComp(Rat(num, den));
            } catch (...) {}
        }
    }
    // Try float
    {
        try {
            size_t idx;
            double f = std::stod(s, &idx);
            if (idx == s.size()) return ComplexComp(f);
        } catch (...) {}
    }
    return std::nullopt;
}

// ── _try_parse_complex_literal ────────────────────────────────────────────────
// Port of Parser.py _try_parse_complex_literal.

static std::optional<Token> try_parse_complex_literal(const std::string& text,
                                                        const SourceInfo& src) {
    // text ends in 'i', len >= 2
    std::string body = text.substr(0, text.size() - 1);
    if (body.empty()) return std::nullopt;   // bare 'i' is identifier

    // Find rightmost +/- that is not an exponent sign
    int split = -1;
    int j = (int)body.size() - 1;
    while (j >= 0) {
        char c = body[j];
        if (c == '+' || c == '-') {
            if (j > 0 && std::tolower((unsigned char)body[j - 1]) == 'e') {
                --j; continue;
            }
            split = j; break;
        }
        --j;
    }
    if (split == -1) return std::nullopt;

    std::string real_str  = body.substr(0, split);
    std::string sign_imag = body.substr(split);

    ComplexComp re_val, im_val;
    if (real_str.empty()) {
        re_val = int64_t(0);
    } else {
        auto opt = parse_num_for_complex(real_str);
        if (!opt) return std::nullopt;
        re_val = *opt;
    }

    if (sign_imag == "+") {
        im_val = int64_t(1);
    } else if (sign_imag == "-") {
        im_val = int64_t(-1);
    } else {
        auto opt = parse_num_for_complex(sign_imag);
        if (!opt) return std::nullopt;
        im_val = *opt;
    }

    if (is_exact_comp(re_val) && is_exact_comp(im_val)) {
        Token tok(TokenKind::EXACT_COMPLEX, src);
        tok.val  = comp_to_scheme(re_val);
        tok.val2 = comp_to_scheme(im_val);
        return tok;
    }
    Token tok(TokenKind::COMPLEX, src);
    tok.dbl_val  = comp_to_double(re_val);
    tok.dbl_val2 = comp_to_double(im_val);
    return tok;
}

// ── _try_parse_polar_literal ──────────────────────────────────────────────────
// Port of Parser.py _try_parse_polar_literal.

static std::optional<Token> try_parse_polar_literal(const std::string& text,
                                                      const SourceInfo& src) {
    size_t at = text.find('@');
    if (at == std::string::npos || at == 0 || at + 1 == text.size())
        return std::nullopt;
    auto r_opt = parse_num_for_complex(text.substr(0, at));
    auto t_opt = parse_num_for_complex(text.substr(at + 1));
    if (!r_opt || !t_opt) return std::nullopt;
    double r     = comp_to_double(*r_opt);
    double theta = comp_to_double(*t_opt);
    Token tok(TokenKind::COMPLEX, src);
    tok.dbl_val  = r * std::cos(theta);
    tok.dbl_val2 = r * std::sin(theta);
    return tok;
}

// ── _try_parse_prefixed_number ────────────────────────────────────────────────
// Port of Parser.py _try_parse_prefixed_number.
// text is the full hash-word starting with '#' (e.g. "#b101", "#e3.14").
// Throws SchemeSyntaxError on any recognised-but-invalid form.
// Throws SchemeSyntaxError("unknown #-syntax: ...") for unrecognised letters.

// Port of _Rat(rest) for exact decimal strings: parses "0.1" -> 1/10, "1.5" -> 3/2.
// Handles optional sign, decimal point, and e/E exponent.  Avoids double.
static Rat decimal_str_to_rat(const std::string& s) {
    int64_t sign = 1;
    size_t i = 0;
    if (!s.empty() && s[0] == '-') { sign = -1; i = 1; }
    else if (!s.empty() && s[0] == '+') { i = 1; }
    // Split on 'e'/'E'.
    int64_t exp = 0;
    size_t e_pos = s.find_first_of("eE", i);
    std::string mantissa;
    if (e_pos != std::string::npos) {
        mantissa = s.substr(i, e_pos - i);
        exp = std::stoll(s.substr(e_pos + 1));
    } else {
        mantissa = s.substr(i);
    }
    // Split mantissa on '.'.
    size_t dot_pos = mantissa.find('.');
    std::string int_str;
    int64_t frac_digits = 0;
    if (dot_pos != std::string::npos) {
        int_str = mantissa.substr(0, dot_pos) + mantissa.substr(dot_pos + 1);
        frac_digits = (int64_t)(mantissa.size() - dot_pos - 1);
    } else {
        int_str = mantissa;
    }
    if (int_str.empty()) int_str = "0";
    int64_t num = std::stoll(int_str);
    int64_t den = 1;
    for (int64_t k = 0; k < frac_digits; ++k) den *= 10;
    if (exp > 0) { for (int64_t k = 0; k < exp; ++k) num *= 10; }
    else if (exp < 0) { for (int64_t k = 0; k < -exp; ++k) den *= 10; }
    return Rat(sign * num, den);
}

// Helper: validate that a string is digits in the given radix (no sign).
static bool valid_uint_radix(const std::string& s, int radix) {
    if (s.empty()) return false;
    for (char c : s) {
        char lc = (char)std::tolower((unsigned char)c);
        int d = (lc >= '0' && lc <= '9') ? (lc - '0') :
                (lc >= 'a' && lc <= 'z') ? (lc - 'a' + 10) : radix;
        if (d >= radix) return false;
    }
    return true;
}

static bool valid_int_radix(const std::string& s, int radix) {
    if (s.empty()) return false;
    size_t st = (s[0] == '+' || s[0] == '-') ? 1 : 0;
    return valid_uint_radix(s.substr(st), radix);
}

static Token try_parse_prefixed_number(const std::string& text, const SourceInfo& src) {
    int radix = -1;   // -1 = unspecified
    int exact = -1;   // -1 = unspecified; 1 = exact; 0 = inexact
    size_t i = 0;

    while (i < text.size() && text[i] == '#') {
        if (i + 1 >= text.size())
            throw SchemeSyntaxError("unknown #-syntax: '" + text + "'", new SourceInfo(src));
        char ch = (char)std::tolower((unsigned char)text[i + 1]);
        if      (ch == 'b') { if (radix!=-1) throw SchemeSyntaxError("duplicate radix prefix: '"+text+"'",new SourceInfo(src)); radix=2;  i+=2; }
        else if (ch == 'o') { if (radix!=-1) throw SchemeSyntaxError("duplicate radix prefix: '"+text+"'",new SourceInfo(src)); radix=8;  i+=2; }
        else if (ch == 'd') { if (radix!=-1) throw SchemeSyntaxError("duplicate radix prefix: '"+text+"'",new SourceInfo(src)); radix=10; i+=2; }
        else if (ch == 'x') { if (radix!=-1) throw SchemeSyntaxError("duplicate radix prefix: '"+text+"'",new SourceInfo(src)); radix=16; i+=2; }
        else if (ch == 'e') { if (exact!=-1) throw SchemeSyntaxError("duplicate exactness prefix: '"+text+"'",new SourceInfo(src)); exact=1; i+=2; }
        else if (ch == 'i') { if (exact!=-1) throw SchemeSyntaxError("duplicate exactness prefix: '"+text+"'",new SourceInfo(src)); exact=0; i+=2; }
        else throw SchemeSyntaxError("unknown #-syntax: '" + text + "'", new SourceInfo(src));
    }

    if (radix == -1) radix = 10;
    std::string rest = text.substr(i);
    if (rest.empty())
        throw SchemeSyntaxError("number prefix with no digits: '" + text + "'", new SourceInfo(src));

    // Try integer with given radix
    if (valid_int_radix(rest, radix)) {
        try {
            int64_t n = std::stoll(rest, nullptr, radix);
            if (exact == 0) { Token t(TokenKind::REAL, src); t.dbl_val=(double)n; return t; }
            Token t(TokenKind::INT, src); t.int_val = n; return t;
        } catch (...) {}
    }

    // Try rational with given radix
    {
        size_t slash = rest.find('/');
        if (slash != std::string::npos) {
            std::string num_s = rest.substr(0, slash);
            std::string den_s = rest.substr(slash + 1);
            if (valid_int_radix(num_s, radix) && valid_uint_radix(den_s, radix)) {
                try {
                    int64_t num = std::stoll(num_s, nullptr, radix);
                    int64_t den = std::stoll(den_s, nullptr, radix);
                    if (den != 0) {
                        Rat frac(num, den);
                        if (exact == 0) { Token t(TokenKind::REAL,src); t.dbl_val=static_cast<double>(frac); return t; }
                        Token t(TokenKind::RATIONAL, src);
                        t.int_val = frac.numerator; t.int_val2 = frac.denominator; return t;
                    }
                } catch (...) {}
            }
        }
    }

    // Try real (radix 10 only)
    if (radix == 10) {
        if (rest == "+inf.0") {
            if (exact==1) throw SchemeSyntaxError("#e cannot be applied to +inf.0",new SourceInfo(src));
            Token t(TokenKind::REAL,src); t.dbl_val=INFINITY; return t;
        }
        if (rest == "-inf.0") {
            if (exact==1) throw SchemeSyntaxError("#e cannot be applied to -inf.0",new SourceInfo(src));
            Token t(TokenKind::REAL,src); t.dbl_val=-INFINITY; return t;
        }
        if (rest == "+nan.0" || rest == "-nan.0") {
            if (exact==1) throw SchemeSyntaxError("#e cannot be applied to +nan.0",new SourceInfo(src));
            Token t(TokenKind::REAL,src); t.dbl_val=NAN; return t;
        }
        try {
            size_t idx;
            double f = std::stod(rest, &idx);
            if (idx == rest.size()) {
                if (exact == 1) {
                    if (!std::isfinite(f))
                        throw SchemeSyntaxError("#e applied to non-finite real "+rest,new SourceInfo(src));
                    if (f == std::floor(f)) {
                        Token t(TokenKind::INT, src); t.int_val=(int64_t)f; return t;
                    }
                    Rat frac = decimal_str_to_rat(rest);
                    Token t(TokenKind::RATIONAL, src);
                    t.int_val=frac.numerator; t.int_val2=frac.denominator; return t;
                }
                Token t(TokenKind::REAL,src); t.dbl_val=f; return t;
            }
        } catch (const SchemeSyntaxError&) { throw; } catch (...) {}
    }

    throw SchemeSyntaxError("invalid prefixed number: '" + text + "'", new SourceInfo(src));
}

// ── build_word_token ──────────────────────────────────────────────────────────
// Port of Parser.py _build_token for IDENT, INT, REAL, RATIONAL, COMPLEX cases.
// word is a non-empty string of non-ident-delimiter characters.

static Token build_word_token(const std::string& word, const SourceInfo& src, bool fold_case) {
    // Special real literals (port of IDENT branch in _build_token)
    if (word=="+inf.0"){ Token t(TokenKind::REAL,src); t.dbl_val= INFINITY; return t; }
    if (word=="-inf.0"){ Token t(TokenKind::REAL,src); t.dbl_val=-INFINITY; return t; }
    if (word=="+nan.0"||word=="-nan.0"){ Token t(TokenKind::REAL,src); t.dbl_val=NAN; return t; }

    // Try rational [+-]?\d+/\d+
    {
        size_t slash = word.find('/');
        if (slash != std::string::npos) {
            std::string num_s = word.substr(0, slash);
            std::string den_s = word.substr(slash + 1);
            auto is_int_s = [](const std::string& s) -> bool {
                if (s.empty()) return false;
                size_t st = ((s[0]=='+' || s[0]=='-') ? 1 : 0);
                if (st >= s.size()) return false;
                for (size_t k=st; k<s.size(); ++k) if(s[k]<'0'||s[k]>'9') return false;
                return true;
            };
            auto is_uint_s = [](const std::string& s) -> bool {
                if (s.empty()) return false;
                for (char c : s) if (c<'0'||c>'9') return false;
                return true;
            };
            if (is_int_s(num_s) && is_uint_s(den_s)) {
                try {
                    Rat r(std::stoll(num_s), std::stoll(den_s));
                    Token t(TokenKind::RATIONAL, src);
                    t.int_val=r.numerator; t.int_val2=r.denominator; return t;
                } catch (...) {}
            }
        }
    }

    // Try real (pattern: has '.' or 'e'/'E')
    {
        bool has_dot = word.find('.') != std::string::npos;
        bool has_exp = word.find('e') != std::string::npos || word.find('E') != std::string::npos;
        if (has_dot || has_exp) {
            try {
                size_t idx;
                double f = std::stod(word, &idx);
                if (idx == word.size()) {
                    Token t(TokenKind::REAL, src); t.dbl_val=f; return t;
                }
            } catch (...) {}
        }
    }

    // Try integer [+-]?\d+
    {
        size_t st = ((word[0]=='+' || word[0]=='-') ? 1 : 0);
        if (st < word.size()) {
            bool ok = true;
            for (size_t k=st; k<word.size(); ++k) if(word[k]<'0'||word[k]>'9'){ok=false;break;}
            if (ok) {
                try {
                    Token t(TokenKind::INT, src); t.int_val=std::stoll(word); return t;
                } catch (...) {}
            }
        }
    }

    // Complex literal (ends in 'i', len >= 2)
    if (word.size() >= 2 && word.back() == 'i') {
        auto opt = try_parse_complex_literal(word, src);
        if (opt) return *opt;
    }

    // Polar literal (contains '@')
    if (word.find('@') != std::string::npos) {
        auto opt = try_parse_polar_literal(word, src);
        if (opt) return *opt;
    }

    // Leading '@' is not a valid identifier initial
    if (word[0] == '@')
        throw SchemeSyntaxError("'@' is not a valid identifier initial: '" + word + "'",
                                 new SourceInfo(src));

    // Malformed number?
    if (starts_like_number(word))
        throw SchemeSyntaxError("malformed number or identifier: '" + word + "'",
                                 new SourceInfo(src));

    // Identifier
    std::string name = word;
    if (fold_case) for (char& c : name) c = (char)std::tolower((unsigned char)c);
    Token t(TokenKind::IDENT, src);
    t.str_val = std::move(name);
    return t;
}

// ── scheme_tokenize ───────────────────────────────────────────────────────────
// Port of Parser.py tokenize.

std::vector<Token> scheme_tokenize(const std::string& source, const std::string& filename) {
    std::vector<std::string> source_lines = make_source_lines(source);
    std::vector<Token> tokens;
    bool   fold_case = false;
    size_t pos  = 0;
    int    line = 1, col = 1;
    size_t n    = source.size();

    auto cur_src = [&]() { return make_src_val(line, col, source_lines, filename); };

    while (pos < n) {
        // Block comment #| ... |#  (possibly nested)  R7RS §2.2
        if (pos+1 < n && source[pos]=='#' && source[pos+1]=='|') {
            int depth = 1; pos+=2; col+=2;
            while (pos < n && depth > 0) {
                if (pos+1<n && source[pos]=='#' && source[pos+1]=='|')
                    { ++depth; pos+=2; col+=2; }
                else if (pos+1<n && source[pos]=='|' && source[pos+1]=='#')
                    { --depth; pos+=2; col+=2; }
                else {
                    if (source[pos]=='\n') { ++line; col=1; } else ++col;
                    ++pos;
                }
            }
            if (depth != 0)
                throw SchemeSyntaxError("unterminated block comment", new SourceInfo(cur_src()));
            continue;
        }

        // Datum comment #;  R7RS §2.2
        if (pos+1 < n && source[pos]=='#' && source[pos+1]==';') {
            tokens.emplace_back(TokenKind::DATUM_COMMENT, cur_src());
            pos+=2; col+=2; continue;
        }

        // Datum label #n= or #n#  R7RS §2.4
        if (source[pos] == '#') {
            size_t j = pos + 1;
            while (j < n && source[j] >= '0' && source[j] <= '9') ++j;
            if (j > pos+1 && j < n && (source[j]=='=' || source[j]=='#')) {
                SourceInfo sv = cur_src();
                int64_t label_n = 0;
                for (size_t k = pos+1; k < j; ++k) label_n = label_n*10 + (source[k]-'0');
                TokenKind tk = (source[j]=='=') ? TokenKind::LABEL_DEF : TokenKind::LABEL_REF;
                Token tok(tk, sv); tok.int_val = label_n;
                tokens.push_back(std::move(tok));
                col += (int)(j - pos + 1); pos = j + 1; continue;
            }
        }

        // Directive #!fold-case / #!no-fold-case  R7RS §2.1
        if (pos+1 < n && source[pos]=='#' && source[pos+1]=='!') {
            if (pos+11<=n && source.substr(pos+2,9)=="fold-case")
                { fold_case=true;  col+=11; pos+=11; continue; }
            if (pos+14<=n && source.substr(pos+2,12)=="no-fold-case")
                { fold_case=false; col+=14; pos+=14; continue; }
            throw SchemeSyntaxError("unknown #!-directive", new SourceInfo(cur_src()));
        }

        // Vertical-bar symbol |...|  R7RS §2.1
        if (source[pos] == '|') {
            SourceInfo sv = cur_src(); ++pos; ++col;
            std::string raw;
            while (pos < n && source[pos] != '|') {
                char c = source[pos];
                if (c == '\\') {
                    raw += c; ++pos; ++col;
                    if (pos < n) {
                        char ec = source[pos]; raw += ec;
                        if (ec=='\n') { ++line; col=1; } else ++col;
                        ++pos;
                        // keep collecting \xHEX; as a unit in raw
                        if (ec == 'x') {
                            while (pos<n && is_hex(source[pos]))  { raw+=source[pos++]; ++col; }
                            if (pos<n && source[pos]==';') { raw+=source[pos++]; ++col; }
                        }
                    }
                } else {
                    if (c=='\n') { ++line; col=1; } else ++col;
                    raw += c; ++pos;
                }
            }
            if (pos >= n)
                throw SchemeSyntaxError("unterminated |...| symbol", new SourceInfo(sv));
            ++pos; ++col; // consume closing |
            std::string name = decode_symbol_escapes(raw, sv);
            if (fold_case) for (char& c : name) c=(char)std::tolower((unsigned char)c);
            Token tok(TokenKind::IDENT, sv); tok.str_val = std::move(name);
            tokens.push_back(std::move(tok)); continue;
        }

        char c = source[pos];

        // Whitespace
        if (c==' ' || c=='\t' || c=='\r') { ++col; ++pos; continue; }
        if (c=='\n') { ++line; col=1; ++pos; continue; }

        // Line comment
        if (c == ';') { while (pos<n && source[pos]!='\n') { ++col; ++pos; } continue; }

        SourceInfo sv = cur_src();

        // Parentheses and brackets
        if (c=='(') { tokens.emplace_back(TokenKind::LPAREN,   sv); ++pos; ++col; continue; }
        if (c==')') { tokens.emplace_back(TokenKind::RPAREN,   sv); ++pos; ++col; continue; }
        if (c=='[') { tokens.emplace_back(TokenKind::LBRACKET, sv); ++pos; ++col; continue; }
        if (c==']') { tokens.emplace_back(TokenKind::RBRACKET, sv); ++pos; ++col; continue; }

        // Quote abbreviations
        if (c=='\'') { tokens.emplace_back(TokenKind::QUOTE,      sv); ++pos; ++col; continue; }
        if (c=='`')  { tokens.emplace_back(TokenKind::QUASIQUOTE, sv); ++pos; ++col; continue; }
        if (c == ',') {
            if (pos+1 < n && source[pos+1]=='@')
                { tokens.emplace_back(TokenKind::UNQUOTE_SPLICING,sv); pos+=2; col+=2; }
            else
                { tokens.emplace_back(TokenKind::UNQUOTE, sv); ++pos; ++col; }
            continue;
        }

        // String literal
        if (c == '"') {
            ++pos; ++col;
            std::string raw;
            while (pos < n && source[pos] != '"') {
                char sc = source[pos];
                if (sc == '\\') {
                    raw += sc; ++pos; ++col;
                    if (pos < n) {
                        char ec = source[pos]; raw += ec;
                        if (ec=='\n') { ++line; col=1; } else ++col;
                        ++pos;
                    }
                } else {
                    if (sc=='\n') { ++line; col=1; } else ++col;
                    raw += sc; ++pos;
                }
            }
            if (pos >= n)
                throw SchemeSyntaxError("unexpected character '\"'", new SourceInfo(sv));
            ++pos; ++col; // consume closing "
            std::string content = decode_string_escapes(raw, sv);
            Token tok(TokenKind::STRING, sv); tok.str_val = std::move(content);
            tokens.push_back(std::move(tok)); continue;
        }

        // Hash forms
        if (c == '#') {
            // #u8( bytevector
            if (pos+3 < n && source[pos+1]=='u' && source[pos+2]=='8' && source[pos+3]=='(')
                { tokens.emplace_back(TokenKind::BYTEVECTOR_LPAREN, sv); pos+=4; col+=4; continue; }
            // #( vector
            if (pos+1 < n && source[pos+1]=='(')
                { tokens.emplace_back(TokenKind::VECTOR_LPAREN, sv); pos+=2; col+=2; continue; }
            // #\ character
            if (pos+1 < n && source[pos+1]=='\\') {
                pos+=2; col+=2;
                if (pos >= n)
                    throw SchemeSyntaxError("unexpected end of input after #\\",
                                             new SourceInfo(sv));
                std::string char_rest;
                char fc = source[pos];
                if (std::isalpha((unsigned char)fc)) {
                    if (fc=='x' || fc=='X') {
                        // x[hex]+ or just alphabetic word
                        size_t j = pos + 1;
                        while (j < n && is_hex(source[j])) ++j;
                        if (j > pos + 1) {
                            // x + one or more hex digits
                            while (pos < j) { char_rest += source[pos++]; ++col; }
                        } else {
                            // not hex → scan alphabetic word
                            while (pos<n && std::isalpha((unsigned char)source[pos]))
                                { char_rest+=source[pos++]; ++col; }
                        }
                    } else {
                        while (pos<n && std::isalpha((unsigned char)source[pos]))
                            { char_rest+=source[pos++]; ++col; }
                    }
                } else {
                    char_rest += source[pos++]; ++col;
                }
                char32_t cv = decode_char_literal(char_rest, sv);
                Token tok(TokenKind::CHAR, sv); tok.char_val = cv;
                tokens.push_back(std::move(tok)); continue;
            }
            // #t / #true
            if (pos+1 < n && (source[pos+1]=='t' || source[pos+1]=='T')) {
                size_t rp = pos + 2;
                if (rp+3 <= n && source.substr(rp,3)=="rue") rp += 3;
                if (rp >= n || is_delim(source[rp])) {
                    Token tok(TokenKind::BOOL, sv); tok.bool_val = true;
                    tokens.push_back(std::move(tok));
                    col += (int)(rp-pos); pos = rp; continue;
                }
            }
            // #f / #false
            if (pos+1 < n && (source[pos+1]=='f' || source[pos+1]=='F')) {
                size_t rp = pos + 2;
                if (rp+4 <= n && source.substr(rp,4)=="alse") rp += 4;
                if (rp >= n || is_delim(source[rp])) {
                    Token tok(TokenKind::BOOL, sv); tok.bool_val = false;
                    tokens.push_back(std::move(tok));
                    col += (int)(rp-pos); pos = rp; continue;
                }
            }
            // Hash-ident: #[^\s()\[\]'"`,;|\\]*
            {
                size_t hw_start = pos;
                while (pos<n && !is_delim(source[pos]) && source[pos]!='\\')
                    { ++pos; ++col; }
                std::string hw = source.substr(hw_start, pos - hw_start);
                tokens.push_back(try_parse_prefixed_number(hw, sv));
                continue;
            }
        }

        // Lone dot: '.' followed by delimiter or EOF
        if (c == '.') {
            if (pos+1 >= n || is_delim(source[pos+1]))
                { tokens.emplace_back(TokenKind::DOT, sv); ++pos; ++col; continue; }
            // not a lone dot — fall through to word scan
        }

        // Word scan: number or identifier
        {
            size_t ws = pos;
            while (pos < n && !is_ident_delim(source[pos])) ++pos;
            if (pos == ws)
                throw SchemeSyntaxError(
                    std::string("unexpected character '") + c + "'", new SourceInfo(sv));
            std::string word = source.substr(ws, pos - ws);
            col += (int)(pos - ws);
            tokens.push_back(build_word_token(word, sv, fold_case));
        }
    }

    tokens.emplace_back(TokenKind::END_OF_FILE, cur_src());
    return tokens;
}

// ── SchemeParser ──────────────────────────────────────────────────────────────
// Port of Parser.py Parser class.  Not exported — used only by scheme_parse/parse_one.

class SchemeParser {
    std::vector<Token>                 tokens_;
    size_t                             pos_;
    std::unordered_map<int64_t, Value> labels_;

    const Token& _peek() const { return tokens_[pos_]; }  // public: used by scheme_parse_one

    void _advance() {
        if (tokens_[pos_].kind != TokenKind::END_OF_FILE) ++pos_;
    }

    // Port of Parser._skip_datum_comments
    void _skip_datum_comments() {
        while (_peek().kind == TokenKind::DATUM_COMMENT) {
            _advance();
            parse_expr();  // discard the skipped datum
        }
    }

    // Port of Parser._build_list
    static Value build_list(std::vector<Value>& items,
                             bool has_tail, const Value& dotted_tail,
                             const SourceInfo* list_src) {
        // alloc_cons clones src; pass the borrowed list_src directly.
        SourceInfo* borrowed = const_cast<SourceInfo*>(list_src);
        // make_nil stores src into a NilAtom (inline) — for that one we still
        // need a fresh allocation since NilAtom owns its src.
        Value result = has_tail ? dotted_tail
                                : make_nil(list_src ? new SourceInfo(*list_src) : nullptr);
        for (int i = (int)items.size() - 1; i >= 0; --i)
            result = alloc_cons(items[i], result, borrowed);
        return result;
    }

    // Port of Parser._read_bytevector
    Value read_bytevector() {
        SourceInfo bv_src = _peek().src;
        _advance();  // consume '#u8('
        std::vector<uint8_t> items;
        while (true) {
            _skip_datum_comments();
            const Token& tok = _peek();
            if (tok.kind == TokenKind::RPAREN) {
                _advance();
                Value bv = make_bytevector(items);
                mark_literal_immutable(bv);
                return bv;
            }
            if (tok.kind == TokenKind::END_OF_FILE)
                throw SchemeSyntaxError("unterminated bytevector literal",
                                         new SourceInfo(bv_src));
            Value elem = parse_expr();
            if (!is_integer(elem))
                throw SchemeSyntaxError("bytevector element must be an exact integer in 0-255",
                                         new SourceInfo(bv_src));
            int64_t nv = as_integer(elem);
            if (nv < 0 || nv > 255)
                throw SchemeSyntaxError("bytevector element out of range 0-255: "
                                         + std::to_string(nv), new SourceInfo(bv_src));
            items.push_back((uint8_t)nv);
        }
    }

    // Port of Parser._read_vector
    Value read_vector() {
        SourceInfo vec_src = _peek().src;
        _advance();  // consume '#('
        std::vector<Value> items;
        while (true) {
            _skip_datum_comments();
            const Token& tok = _peek();
            if (tok.kind == TokenKind::RPAREN) {
                _advance();
                Value v = make_vector(items);
                mark_literal_immutable(v);
                return v;
            }
            if (tok.kind == TokenKind::END_OF_FILE)
                throw SchemeSyntaxError("unterminated vector literal",
                                         new SourceInfo(vec_src));
            items.push_back(parse_expr());
        }
    }

    // Port of Parser._read_list
    Value read_list() {
        SourceInfo lparen_src = _peek().src;
        bool is_bracket = (_peek().kind == TokenKind::LBRACKET);
        _advance();  // consume '(' or '['
        TokenKind closer    = is_bracket ? TokenKind::RBRACKET : TokenKind::RPAREN;
        char      closer_ch = is_bracket ? ']' : ')';

        std::vector<Value> items;
        bool  has_tail = false;
        Value dotted_tail;

        while (true) {
            _skip_datum_comments();
            const Token& tok = _peek();

            if (tok.kind == closer) {
                _advance();
                return build_list(items, has_tail, dotted_tail, &lparen_src);
            }
            if (tok.kind == TokenKind::RPAREN || tok.kind == TokenKind::RBRACKET)
                throw SchemeSyntaxError(
                    std::string("mismatched bracket: expected '") + closer_ch + "'",
                    new SourceInfo(tok.src));
            if (tok.kind == TokenKind::DOT) {
                SourceInfo dot_src = tok.src;
                _advance();
                if (items.empty())
                    throw SchemeSyntaxError("dot must be preceded by at least one element",
                                             new SourceInfo(dot_src));
                const Token& nxt = _peek();
                if (nxt.kind==TokenKind::RPAREN || nxt.kind==TokenKind::RBRACKET ||
                    nxt.kind==TokenKind::END_OF_FILE || nxt.kind==TokenKind::DOT)
                    throw SchemeSyntaxError("dot must be followed by an expression",
                                             new SourceInfo(nxt.src));
                dotted_tail = parse_expr();
                has_tail = true;
                const Token& closing = _peek();
                if (closing.kind != closer)
                    throw SchemeSyntaxError(
                        std::string("expected '") + closer_ch + "' after dotted tail",
                        new SourceInfo(closing.src));
                _advance();
                return build_list(items, has_tail, dotted_tail, &lparen_src);
            }
            if (tok.kind == TokenKind::END_OF_FILE)
                throw SchemeSyntaxError(
                    std::string("unterminated list (missing '") + closer_ch + "')",
                    new SourceInfo(lparen_src));
            items.push_back(parse_expr());
        }
    }

public:
    explicit SchemeParser(std::vector<Token> toks)
        : tokens_(std::move(toks)), pos_(0) {}

    // Port of Parser.parse_expr called standalone — no EOF check (used by read primitive)
    Value parse_first_expr() {
        _skip_datum_comments();
        return parse_expr();
    }

    // Port of Parser.parse_one (called from scheme_parse_one)
    Value parse_one_expr() {
        _skip_datum_comments();
        Value expr = parse_expr();
        _skip_datum_comments();
        const Token& tok = _peek();
        if (tok.kind != TokenKind::END_OF_FILE)
            throw SchemeSyntaxError("unexpected token after expression",
                                     new SourceInfo(tok.src));
        return expr;
    }

    // Port of Parser.parse_program
    std::vector<Value> parse_program() {
        std::vector<Value> forms;
        while (true) {
            _skip_datum_comments();
            if (_peek().kind == TokenKind::END_OF_FILE) break;
            forms.push_back(parse_expr());
        }
        return forms;
    }

    // Port of Parser.parse_expr
    Value parse_expr() {
        _skip_datum_comments();
        const Token& tok  = _peek();
        TokenKind    kind = tok.kind;

        // Datum label definition #n=  R7RS §2.4
        if (kind == TokenKind::LABEL_DEF) {
            int64_t    label_n  = tok.int_val;
            SourceInfo tok_src  = tok.src;
            _advance();
            if (labels_.count(label_n))
                throw SchemeSyntaxError(
                    "duplicate datum label #" + std::to_string(label_n) + "=",
                    new SourceInfo(tok_src));
            const Token& nxt = _peek();
            if (nxt.kind==TokenKind::LPAREN || nxt.kind==TokenKind::LBRACKET) {
                // Lists: pre-allocate a ConsCell stub so forward #n# refs work.
                // After parsing, mutate stub in-place so all prior refs see the result.
                SourceInfo nxt_src = nxt.src;
                Value stub = alloc_cons(NIL_VALUE, NIL_VALUE, &nxt_src);
                labels_[label_n] = stub;
                Value datum = parse_expr();
                if (is_cons(datum)) {
                    set_car(stub, car(datum));
                    set_cdr(stub, cdr(datum));
                    return stub;
                }
                labels_[label_n] = datum;
                return datum;
            }
            if (nxt.kind==TokenKind::VECTOR_LPAREN) {
                // Vectors: pre-create the vector with an empty items list so any
                // #n# references inside the vector resolve to the final object.
                // After parsing, copy the actual items into the pre-allocated list.
                Value pre_vector = make_vector({});
                labels_[label_n] = pre_vector;
                Value datum = parse_expr();
                std::vector<Value>& pre_items  = as_vector_items(pre_vector);
                const std::vector<Value>& parsed = as_vector_items_const(datum);
                pre_items.insert(pre_items.end(), parsed.begin(), parsed.end());
                return pre_vector;
            }
            Value datum = parse_expr();
            labels_[label_n] = datum;
            return datum;
        }

        // Datum label reference #n#  R7RS §2.4
        if (kind == TokenKind::LABEL_REF) {
            int64_t   label_n = tok.int_val;
            SourceInfo tok_src = tok.src;
            _advance();
            auto it = labels_.find(label_n);
            if (it == labels_.end())
                throw SchemeSyntaxError(
                    "undefined datum label #" + std::to_string(label_n) + "#",
                    new SourceInfo(tok_src));
            return it->second;
        }

        // Atomic literals
        if (kind == TokenKind::INT) {
            int64_t v = tok.int_val; SourceInfo ss = tok.src; _advance();
            return make_integer(v, new SourceInfo(ss));
        }
        if (kind == TokenKind::REAL) {
            double v = tok.dbl_val; SourceInfo ss = tok.src; _advance();
            return make_real(v, new SourceInfo(ss));
        }
        if (kind == TokenKind::RATIONAL) {
            int64_t num=tok.int_val, den=tok.int_val2; SourceInfo ss = tok.src; _advance();
            return make_rational(num, den, new SourceInfo(ss));
        }
        if (kind == TokenKind::COMPLEX) {
            double re=tok.dbl_val, im=tok.dbl_val2; _advance();
            return make_complex(re, im);
        }
        if (kind == TokenKind::EXACT_COMPLEX) {
            Value re_s=tok.val, im_s=tok.val2; _advance();
            if (is_integer(im_s) && as_integer(im_s) == 0) return re_s;
            return make_exact_complex(re_s, im_s);
        }
        if (kind == TokenKind::STRING) {
            std::string s = tok.str_val; SourceInfo ss = tok.src; _advance();
            Value sv = make_string(s, new SourceInfo(ss));
            mark_literal_immutable(sv);
            return sv;
        }
        if (kind == TokenKind::CHAR) {
            char32_t cv = tok.char_val; SourceInfo ss = tok.src; _advance();
            return make_character(cv, new SourceInfo(ss));
        }
        if (kind == TokenKind::BOOL) {
            bool bv = tok.bool_val; SourceInfo ss = tok.src; _advance();
            return make_boolean(bv, new SourceInfo(ss));
        }
        if (kind == TokenKind::IDENT) {
            std::string name = tok.str_val; SourceInfo ss = tok.src; _advance();
            return make_symbol(name, new SourceInfo(ss));
        }

        // List / vector / bytevector
        if (kind==TokenKind::LPAREN || kind==TokenKind::LBRACKET)
            return read_list();
        if (kind == TokenKind::BYTEVECTOR_LPAREN)
            return read_bytevector();
        if (kind == TokenKind::VECTOR_LPAREN)
            return read_vector();

        // Reader abbreviations.  alloc_cons clones src; pass &q_src directly.
        if (kind == TokenKind::QUOTE) {
            SourceInfo q_src = tok.src; _advance();
            Value datum = parse_expr();
            Value q_sym = make_symbol("quote");
            Value inner = alloc_cons(datum, NIL_VALUE, &q_src);
            return alloc_cons(q_sym, inner, &q_src);
        }
        if (kind == TokenKind::QUASIQUOTE) {
            SourceInfo q_src = tok.src; _advance();
            Value datum = parse_expr();
            Value q_sym = make_symbol("quasiquote");
            Value inner = alloc_cons(datum, NIL_VALUE, &q_src);
            return alloc_cons(q_sym, inner, &q_src);
        }
        if (kind == TokenKind::UNQUOTE) {
            SourceInfo q_src = tok.src; _advance();
            Value datum = parse_expr();
            Value q_sym = make_symbol("unquote");
            Value inner = alloc_cons(datum, NIL_VALUE, &q_src);
            return alloc_cons(q_sym, inner, &q_src);
        }
        if (kind == TokenKind::UNQUOTE_SPLICING) {
            SourceInfo q_src = tok.src; _advance();
            Value datum = parse_expr();
            Value q_sym = make_symbol("unquote-splicing");
            Value inner = alloc_cons(datum, NIL_VALUE, &q_src);
            return alloc_cons(q_sym, inner, &q_src);
        }

        // Errors
        if (kind == TokenKind::RPAREN)
            throw SchemeSyntaxError("unexpected ')'",   new SourceInfo(tok.src));
        if (kind == TokenKind::RBRACKET)
            throw SchemeSyntaxError("unexpected ']'",   new SourceInfo(tok.src));
        if (kind == TokenKind::DOT)
            throw SchemeSyntaxError("unexpected '.' outside of a list", new SourceInfo(tok.src));
        if (kind == TokenKind::END_OF_FILE)
            throw SchemeSyntaxError("unexpected end of input", new SourceInfo(tok.src));
        throw SchemeSyntaxError("unexpected token", new SourceInfo(tok.src));
    }
};

// ── Public entry points ───────────────────────────────────────────────────────
// Port of Parser.py parse, parse_one.

std::vector<Value> scheme_parse(const std::string& source, const std::string& filename) {
    return SchemeParser(scheme_tokenize(source, filename)).parse_program();
}

Value scheme_parse_one(const std::string& source, const std::string& filename) {
    return SchemeParser(scheme_tokenize(source, filename)).parse_one_expr();
}

Value scheme_parse_first(const std::string& source, const std::string& filename) {
    return SchemeParser(scheme_tokenize(source, filename)).parse_first_expr();
}
