#include "parser.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <limits>
#include <numeric>

// ── Construction / tokenization ───────────────────────────────────────────────

Parser::Parser(std::string_view source) : src_(source)
   {
   tokenize();
   }

bool Parser::at_end() const
   {
   return tok_pos_ >= tokens_.size() ||
          tokens_[tok_pos_].kind == TokKind::Eof;
   }

// ── Complex literal parsing ───────────────────────────────────────────────────

std::optional<Value> parse_complex_literal(const std::string& text)
   {
   if (text.empty() || text.back() != 'i')
      return std::nullopt;

   std::string body = text.substr(0, text.size() - 1);
   if (body.empty())
      return std::nullopt;

   auto parse_component = [](const std::string& s, double& out) -> bool
      {
      if (s.empty()) return false;
      if (s == "+inf.0") { out = +std::numeric_limits<double>::infinity(); return true; }
      if (s == "-inf.0") { out = -std::numeric_limits<double>::infinity(); return true; }
      if (s == "+nan.0" || s == "-nan.0")
         { out = std::numeric_limits<double>::quiet_NaN(); return true; }
      if (s == "+") { out =  1.0; return true; }
      if (s == "-") { out = -1.0; return true; }
      char* end = nullptr;
      out = std::strtod(s.c_str(), &end);
      return end != s.c_str() && end == s.c_str() + s.size();
      };

   int split_pos = -1;
   for (int i = static_cast<int>(body.size()) - 1; i >= 1; --i)
      {
      char c = body[i];
      if (c == '+' || c == '-')
         {
         char prev = body[i - 1];
         if (prev != 'e' && prev != 'E')
            {
            split_pos = i;
            break;
            }
         }
      }

   double real_part = 0.0;
   double imag_part = 0.0;

   if (split_pos >= 1)
      {
      std::string real_str = body.substr(0, split_pos);
      std::string imag_str = body.substr(split_pos);
      if (!parse_component(real_str, real_part)) return std::nullopt;
      if (!parse_component(imag_str, imag_part)) return std::nullopt;
      }
   else
      {
      if (body[0] != '+' && body[0] != '-')
         return std::nullopt;
      if (!parse_component(body, imag_part)) return std::nullopt;
      real_part = 0.0;
      }

   if (imag_part == 0.0)
      return make_flonum(real_part);
   return make_complex_val(real_part, imag_part);
   }

// ── Lexer ─────────────────────────────────────────────────────────────────────

void Parser::skip_whitespace_and_comments()
   {
   while (pos_ < src_.size())
      {
      char character = src_[pos_];
      if (std::isspace(static_cast<unsigned char>(character)))
         {
         ++pos_;
         continue;
         }
      if (character == ';')
         {
         while (pos_ < src_.size() && src_[pos_] != '\n')
            ++pos_;
         continue;
         }
      // Block comment #| ... |#
      if (character == '#' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '|')
         {
         pos_ += 2;
         int depth = 1;
         while (pos_ + 1 < src_.size() && depth > 0)
            {
            if (src_[pos_] == '#' && src_[pos_ + 1] == '|')
               {
               ++depth;
               pos_ += 2;
               }
            else if (src_[pos_] == '|' && src_[pos_ + 1] == '#')
               {
               --depth;
               pos_ += 2;
               }
            else
               {
               ++pos_;
               }
            }
         continue;
         }
      // Datum comment #;
      if (character == '#' && pos_ + 1 < src_.size() && src_[pos_ + 1] == ';')
         {
         pos_ += 2;
         tokens_.push_back({TokKind::Symbol, "#;", false});
         continue;
         }
      break;
      }
   }

Parser::Token Parser::read_string_token()
   {
   std::string result;
   while (pos_ < src_.size())
      {
      char character = src_[pos_++];
      if (character == '"')
         return {TokKind::String, result, false};
      if (character == '\\')
         {
         if (pos_ >= src_.size())
            throw SchemeParseError("unterminated string escape");
         char escape_char = src_[pos_++];
         switch (escape_char)
            {
            case 'n':  result += '\n'; break;
            case 'r':  result += '\r'; break;
            case 't':  result += '\t'; break;
            case '"':  result += '"';  break;
            case '\\': result += '\\'; break;
            case 'a':  result += '\a'; break;
            case 'b':  result += '\b'; break;
            case '0':  result += '\0'; break;
            case '|':  result += '|';  break;
            case 'x':
               {
               std::string hex;
               while (pos_ < src_.size() && src_[pos_] != ';')
                  {
                  char hc = src_[pos_];
                  if (!std::isxdigit(static_cast<unsigned char>(hc)))
                     throw SchemeParseError("invalid character in \\x escape");
                  hex += hc;
                  ++pos_;
                  }
               if (pos_ >= src_.size() || src_[pos_] != ';')
                  throw SchemeParseError("missing ';' after \\x escape");
               ++pos_;
               if (hex.empty())
                  throw SchemeParseError("empty \\x escape");
               uint32_t cp = static_cast<uint32_t>(std::stoul(hex, nullptr, 16));
               if (cp < 0x80)
                  result += static_cast<char>(cp);
               else if (cp < 0x800)
                  {
                  result += static_cast<char>(0xC0 | (cp >> 6));
                  result += static_cast<char>(0x80 | (cp & 0x3F));
                  }
               else if (cp < 0x10000)
                  {
                  result += static_cast<char>(0xE0 | (cp >> 12));
                  result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                  result += static_cast<char>(0x80 | (cp & 0x3F));
                  }
               else
                  {
                  result += static_cast<char>(0xF0 | (cp >> 18));
                  result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                  result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                  result += static_cast<char>(0x80 | (cp & 0x3F));
                  }
               break;
               }
            default:   result += escape_char; break;
            }
         continue;
         }
      result += character;
      }
   throw SchemeParseError("unterminated string literal");
   }

Parser::Token Parser::read_hash_token()
   {
   if (pos_ >= src_.size())
      throw SchemeParseError("unexpected end after '#'");
   char character = src_[pos_];

   if (character == 't')
      {
      ++pos_;
      if (pos_ < src_.size() &&
          !std::isspace(static_cast<unsigned char>(src_[pos_])) &&
          src_[pos_] != ')' && src_[pos_] != '(' && src_[pos_] != '"')
         {
         if (src_.substr(pos_, 3) == "rue")
            pos_ += 3;
         }
      return {TokKind::Bool, "t", true};
      }
   if (character == 'f')
      {
      ++pos_;
      if (pos_ < src_.size() &&
          !std::isspace(static_cast<unsigned char>(src_[pos_])) &&
          src_[pos_] != ')' && src_[pos_] != '(' && src_[pos_] != '"')
         {
         if (src_.substr(pos_, 4) == "alse")
            pos_ += 4;
         }
      return {TokKind::Bool, "f", false};
      }

   if (character == 'u' && pos_ + 2 < src_.size() &&
       src_[pos_ + 1] == '8' && src_[pos_ + 2] == '(')
      {
      pos_ += 3;
      return {TokKind::HashU8LParen, "", false};
      }

   if (character == '(')
      {
      ++pos_;
      return {TokKind::HashLParen, "", false};
      }

   if (character == '\\')
      {
      ++pos_;
      if (pos_ >= src_.size())
         throw SchemeParseError("unterminated character literal");
      size_t start_pos = pos_;
      if (pos_ < src_.size() &&
          !std::isspace(static_cast<unsigned char>(src_[pos_])) &&
          src_[pos_] != ')' && src_[pos_] != '(')
         {
         ++pos_;
         while (pos_ < src_.size() &&
                !std::isspace(static_cast<unsigned char>(src_[pos_])) &&
                src_[pos_] != ')' &&
                src_[pos_] != '(')
            ++pos_;
         }
      return {TokKind::Char, std::string(src_.substr(start_pos, pos_ - start_pos)), false};
      }

   char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
   if (lc == 'b' || lc == 'o' || lc == 'd' || lc == 'x' ||
       lc == 'e' || lc == 'i')
      {
      ++pos_;
      std::string digits;
      if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-'))
         digits += src_[pos_++];
      while (pos_ < src_.size())
         {
         char c = src_[pos_];
         if (std::isspace(static_cast<unsigned char>(c)) ||
             c == '(' || c == ')' || c == '"' || c == ';')
            break;
         digits += c;
         ++pos_;
         }
      if (digits.empty() || digits == "+" || digits == "-")
         throw SchemeParseError(std::string("malformed number after #") + character);

      int base = 10;
      bool force_inexact = false;
      bool force_exact   = false;
      if      (lc == 'b') base = 2;
      else if (lc == 'o') base = 8;
      else if (lc == 'd') base = 10;
      else if (lc == 'x') base = 16;
      else if (lc == 'i') force_inexact = true;
      else if (lc == 'e') force_exact   = true;

      char* end_ptr = nullptr;
      long long ival = std::strtoll(digits.c_str(), &end_ptr, base);
      if (end_ptr == digits.c_str() + digits.size())
         {
         if (force_inexact)
            return {TokKind::Float, std::to_string(static_cast<double>(ival)), false};
         return {TokKind::Integer, std::to_string(ival), false};
         }
      double fval = std::strtod(digits.c_str(), &end_ptr);
      if (end_ptr == digits.c_str() + digits.size())
         {
         if (force_exact)
            return {TokKind::Integer, std::to_string(static_cast<long long>(fval)), false};
         return {TokKind::Float, digits, false};
         }
      throw SchemeParseError(std::string("malformed number after #") + character + ": " + digits);
      }

   if (std::isdigit(static_cast<unsigned char>(character)))
      {
      int n = character - '0';
      while (pos_ < src_.size()
             && std::isdigit(static_cast<unsigned char>(src_[pos_])))
         n = n * 10 + (src_[pos_++] - '0');
      if (pos_ >= src_.size())
         throw SchemeParseError("unexpected end after datum label number");
      char suffix = src_[pos_++];
      if (suffix == '=')
         return {TokKind::LabelDefine, std::to_string(n), false};
      if (suffix == '#')
         return {TokKind::LabelRef,    std::to_string(n), false};
      throw SchemeParseError(std::string("expected '=' or '#' after datum label, got '")
                             + suffix + "'");
      }

   throw SchemeParseError(std::string("unknown # syntax: #") + character);
   }

Parser::Token Parser::read_token()
   {
   skip_whitespace_and_comments();
   if (pos_ >= src_.size())
      return {TokKind::Eof, "", false};

   char character = src_[pos_++];

   if (character == '(')  return {TokKind::LParen,     "", false};
   if (character == ')')  return {TokKind::RParen,     "", false};
   if (character == '\'') return {TokKind::Quote,      "", false};
   if (character == '`')  return {TokKind::Quasiquote, "", false};
   if (character == ',')
      {
      if (pos_ < src_.size() && src_[pos_] == '@')
         {
         ++pos_;
         return {TokKind::UnquoteSplicing, "", false};
         }
      return {TokKind::Unquote, "", false};
      }
   if (character == '"') return read_string_token();
   if (character == '#') return read_hash_token();

   // |escaped symbol| — R7RS §2.1
   if (character == '|')
      {
      std::string result;
      while (pos_ < src_.size())
         {
         char c = src_[pos_++];
         if (c == '|')
            return {TokKind::Symbol, result, false};
         if (c == '\\' && pos_ < src_.size())
            {
            char esc = src_[pos_++];
            switch (esc)
               {
               case '|':  result += '|';  break;
               case '\\': result += '\\'; break;
               case 'a':  result += '\a'; break;
               case 'b':  result += '\b'; break;
               case 't':  result += '\t'; break;
               case 'n':  result += '\n'; break;
               case 'r':  result += '\r'; break;
               default:   result += esc;  break;
               }
            continue;
            }
         result += c;
         }
      throw SchemeParseError("unterminated |symbol| literal");
      }

   if (character == '.' &&
       (pos_ >= src_.size() || std::isspace(static_cast<unsigned char>(src_[pos_])) ||
        src_[pos_] == ')'))
      return {TokKind::Dot, ".", false};

   size_t start_pos = pos_ - 1;
   while (pos_ < src_.size())
      {
      char next_char = src_[pos_];
      if (std::isspace(static_cast<unsigned char>(next_char)) ||
          next_char == '(' || next_char == ')' ||
          next_char == '"' || next_char == ';')
         break;
      ++pos_;
      }
   std::string text = src_.substr(start_pos, pos_ - start_pos);

   // Try integer parse
      {
      char*     end_ptr     = nullptr;
      long long integer_val = std::strtoll(text.c_str(), &end_ptr, 10);
      (void)integer_val;
      if (end_ptr == text.c_str() + text.size())
         return {TokKind::Integer, text, false};
      if (end_ptr > text.c_str() && *end_ptr == '/')
         {
         const char* dp = end_ptr + 1;
         char*       de = nullptr;
         std::strtoll(dp, &de, 10);
         if (de > dp && *de == '\0')
            return {TokKind::Rational, text, false};
         }
      }
   // Try float parse
      {
      char* end_ptr = nullptr;
      std::strtod(text.c_str(), &end_ptr);
      if (end_ptr == text.c_str() + text.size() && text != "." && end_ptr != text.c_str())
         return {TokKind::Float, text, false};
      }
   if (text == "+inf.0" || text == "-inf.0" || text == "+nan.0" || text == "-nan.0")
      return {TokKind::Float, text, false};

   // Complex literals (e.g. 3+4i, +i, -2.5i)
      {
      auto cv = parse_complex_literal(text);
      if (cv.has_value())
         {
         Token tok;
         tok.kind        = TokKind::Complex;
         tok.text        = text;
         tok.bool_val    = false;
         tok.complex_val = cv;
         return tok;
         }
      }

   return {TokKind::Symbol, text, false};
   }

void Parser::tokenize()
   {
   while (true)
      {
      Token token = read_token();
      tokens_.push_back(token);
      if (token.kind == TokKind::Eof)
         break;
      }
   }

// ── Token navigation ──────────────────────────────────────────────────────────

const Parser::Token& Parser::peek() const
   {
   if (tok_pos_ >= tokens_.size())
      {
      static Token eof_token{TokKind::Eof, "", false};
      return eof_token;
      }
   return tokens_[tok_pos_];
   }

Parser::Token Parser::consume()
   {
   if (tok_pos_ >= tokens_.size())
      return {TokKind::Eof, "", false};
   return tokens_[tok_pos_++];
   }

// ── Parser ────────────────────────────────────────────────────────────────────

Value Parser::list_from_vec(const std::vector<Value>& items)
   {
   Value result = make_nil();
   for (int index = static_cast<int>(items.size()) - 1; index >= 0; --index)
      {
      auto* cell = gc_alloc_cons();
      cell->car  = items[index];
      cell->cdr  = result;
      result     = make_cons(cell);
      }
   return result;
   }

Value Parser::parse_expr()
   {
   const Token& token = peek();

   if (token.kind == TokKind::Symbol && token.text == "#;")
      {
      consume();
      parse_expr();
      return parse_expr();
      }

   if (token.kind == TokKind::Eof)
      throw SchemeParseError("unexpected end of input");

   if (token.kind == TokKind::LParen)
      {
      consume();
      return parse_list();
      }

   if (token.kind == TokKind::HashLParen)
      {
      consume();
      return parse_vector();
      }

   if (token.kind == TokKind::HashU8LParen)
      {
      consume();
      return parse_bytevector();
      }

   if (token.kind == TokKind::RParen)
      throw SchemeParseError("unexpected ')'");

   if (token.kind == TokKind::Quote)
      {
      consume();
      Value inner = parse_expr();
      auto* cell1 = gc_alloc_cons();
      auto* cell2 = gc_alloc_cons();
      cell2->car  = inner;
      cell2->cdr  = make_nil();
      cell1->car  = make_symbol("quote");
      cell1->cdr  = make_cons(cell2);
      return make_cons(cell1);
      }

   if (token.kind == TokKind::Quasiquote)
      {
      consume();
      Value inner = parse_expr();
      auto* cell1 = gc_alloc_cons();
      auto* cell2 = gc_alloc_cons();
      cell2->car  = inner;
      cell2->cdr  = make_nil();
      cell1->car  = make_symbol("quasiquote");
      cell1->cdr  = make_cons(cell2);
      return make_cons(cell1);
      }

   if (token.kind == TokKind::Unquote)
      {
      consume();
      Value inner = parse_expr();
      auto* cell1 = gc_alloc_cons();
      auto* cell2 = gc_alloc_cons();
      cell2->car  = inner;
      cell2->cdr  = make_nil();
      cell1->car  = make_symbol("unquote");
      cell1->cdr  = make_cons(cell2);
      return make_cons(cell1);
      }

   if (token.kind == TokKind::UnquoteSplicing)
      {
      consume();
      Value inner = parse_expr();
      auto* cell1 = gc_alloc_cons();
      auto* cell2 = gc_alloc_cons();
      cell2->car  = inner;
      cell2->cdr  = make_nil();
      cell1->car  = make_symbol("unquote-splicing");
      cell1->cdr  = make_cons(cell2);
      return make_cons(cell1);
      }

   if (token.kind == TokKind::LabelRef)
      {
      consume();
      int n = std::stoi(token.text);
      auto it = datum_labels_.find(n);
      if (it == datum_labels_.end())
         throw SchemeParseError("datum label #" + token.text + "# used before definition");
      return it->second;
      }

   if (token.kind == TokKind::LabelDefine)
      {
      consume();
      int n = std::stoi(token.text);
      if (peek().kind == TokKind::LParen)
         {
         auto* head = gc_alloc_cons();
         datum_labels_[n] = make_cons(head);
         consume();
         Value body = parse_list();
         if (is_cons(body))
            {
            ConsCell* first = as_cons(body);
            head->car = first->car;
            head->cdr = first->cdr;
            }
         else
            {
            datum_labels_[n] = body;
            return body;
            }
         return make_cons(head);
         }
      Value v = parse_expr();
      datum_labels_[n] = v;
      return v;
      }

   // Atoms
   consume();

   if (token.kind == TokKind::Rational)
      {
      auto slash = token.text.find('/');
      int64_t n = std::strtoll(token.text.c_str(), nullptr, 10);
      int64_t d = std::strtoll(token.text.c_str() + slash + 1, nullptr, 10);
      if (d == 0) throw SchemeParseError("rational literal with zero denominator");
      if (d < 0) { n = -n; d = -d; }
      int64_t g = std::gcd(n < 0 ? -n : n, d);
      n /= g; d /= g;
      if (d == 1) return make_fixnum(n);
      return make_rational(gc_alloc_rational(n, d));
      }

   if (token.kind == TokKind::Bool)
      return make_bool(token.bool_val);

   if (token.kind == TokKind::Integer)
      {
      long long integer_val = std::strtoll(token.text.c_str(), nullptr, 10);
      return make_fixnum(static_cast<int64_t>(integer_val));
      }

   if (token.kind == TokKind::Float)
      {
      if (token.text == "+inf.0") return make_flonum( std::numeric_limits<double>::infinity());
      if (token.text == "-inf.0") return make_flonum(-std::numeric_limits<double>::infinity());
      if (token.text == "+nan.0" || token.text == "-nan.0")
         return make_flonum(std::numeric_limits<double>::quiet_NaN());
      double float_val = std::strtod(token.text.c_str(), nullptr);
      return make_flonum(float_val);
      }

   if (token.kind == TokKind::String)
      return make_string(gc_alloc_string(token.text));

   if (token.kind == TokKind::Char)
      {
      const std::string& char_name = token.text;
      char character;
      if      (char_name == "space")                        character = ' ';
      else if (char_name == "newline")                      character = '\n';
      else if (char_name == "tab")                          character = '\t';
      else if (char_name == "return")                       character = '\r';
      else if (char_name == "null" || char_name == "nul")   character = '\0';
      else if (char_name == "alarm")                        character = '\a';
      else if (char_name == "backspace")                    character = '\b';
      else if (char_name == "escape" || char_name == "esc") character = '\x1b';
      else if (char_name == "delete" || char_name == "del") character = '\x7f';
      else if (char_name.size() == 1)                       character = char_name[0];
      else if (char_name.size() >= 2 && (char_name[0] == 'x' || char_name[0] == 'X') &&
               std::all_of(char_name.begin() + 1, char_name.end(),
                  [](char c){ return std::isxdigit(static_cast<unsigned char>(c)); }))
         {
         uint32_t cp = static_cast<uint32_t>(std::stoul(char_name.substr(1), nullptr, 16));
         return make_char(static_cast<char32_t>(cp));
         }
      else throw SchemeParseError("unknown character name: " + char_name);
      return make_char(static_cast<char32_t>(static_cast<unsigned char>(character)));
      }

   if (token.kind == TokKind::Complex)
      return *token.complex_val;

   if (token.kind == TokKind::Symbol)
      return make_symbol(token.text);

   if (token.kind == TokKind::Dot)
      throw SchemeParseError("unexpected '.' outside list");

   throw SchemeParseError("unexpected token: " + token.text);
   }

Value Parser::parse_list()
   {
   std::vector<Value> items;

   while (true)
      {
      const Token& token = peek();
      if (token.kind == TokKind::Eof)
         throw SchemeParseError("unterminated list");
      if (token.kind == TokKind::RParen)
         {
         consume();
         break;
         }

      if (token.kind == TokKind::Dot)
         {
         consume();
         Value tail = parse_expr();
         if (peek().kind != TokKind::RParen)
            throw SchemeParseError("expected ')' after dotted pair tail");
         consume();
         Value result = tail;
         for (int index = static_cast<int>(items.size()) - 1; index >= 0; --index)
            {
            auto* cell = gc_alloc_cons();
            cell->car  = items[index];
            cell->cdr  = result;
            result     = make_cons(cell);
            }
         return result;
         }

      items.push_back(parse_expr());
      }

   return list_from_vec(items);
   }

Value Parser::parse_vector()
   {
   std::vector<Value> items;
   while (peek().kind != TokKind::RParen)
      {
      if (peek().kind == TokKind::Eof)
         throw SchemeParseError("unterminated vector");
      items.push_back(parse_expr());
      }
   consume();
   auto* vec = gc_alloc_vector(items.size());
   for (size_t index = 0; index < items.size(); ++index)
      vec->elements[index] = items[index];
   return make_vector(vec);
   }

Value Parser::parse_bytevector()
   {
   std::vector<uint8_t> bytes;
   while (peek().kind != TokKind::RParen)
      {
      if (peek().kind == TokKind::Eof)
         throw SchemeParseError("unterminated bytevector");
      Token tok = consume();
      if (tok.kind != TokKind::Integer)
         throw SchemeParseError("bytevector element must be an exact integer 0..255");
      long long val = std::stoll(tok.text);
      if (val < 0 || val > 255)
         throw SchemeParseError("bytevector element out of range 0..255");
      bytes.push_back(static_cast<uint8_t>(val));
      }
   consume();
   auto* bv = gc_alloc_bytevector(bytes.size());
   for (size_t i = 0; i < bytes.size(); ++i)
      bv->data[i] = bytes[i];
   return make_bytevector(bv);
   }

std::optional<Value> Parser::next()
   {
   if (at_end())
      return std::nullopt;
   return parse_expr();
   }
