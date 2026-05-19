#pragma once
#include "value.h"
#include "scheme_export.h"
#include "gc.h"
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <unordered_map>

// Parse a complex literal from text (e.g. "3+4i", "+i", "3.0-2.5i").
// Returns the Value if the text is a valid complex literal, or nullopt otherwise.
// Used by both the tokenizer and string->number.
SCHEME_API std::optional<Value> parse_complex_literal(const std::string& text);

class SCHEME_API Parser
   {
public:
   explicit Parser(std::string_view source);

   // Read and return the next expression, or nullopt at end-of-input.
   std::optional<Value> next();

   // True if there are no more tokens.
   bool at_end() const;

private:
   enum class TokKind
      {
      LParen, RParen, Dot, Quote, Quasiquote, Unquote, UnquoteSplicing,
      HashLParen,   // #(
      HashU8LParen, // #u8(
      Integer, Float, Bool, String, Char, Symbol, Complex, Rational,
      LabelDefine,  // #<n>=  — R7RS datum label definition
      LabelRef,     // #<n>#  — R7RS datum label back-reference
      Eof
      };

   struct Token
      {
      TokKind              kind;
      std::string          text;
      bool                 bool_val;
      std::optional<Value> complex_val;
      };

   std::string        src_;
   size_t             pos_     = 0;
   std::vector<Token> tokens_;
   size_t             tok_pos_ = 0;

   std::unordered_map<int, Value> datum_labels_;

   // Lexer
   void  tokenize();
   void  skip_whitespace_and_comments();
   Token read_token();
   Token read_string_token();
   Token read_hash_token();

   // Parser
   Value parse_expr();
   Value parse_list();
   Value parse_vector();
   Value parse_bytevector();

   const Token& peek() const;
   Token        consume();

   static Value list_from_vec(const std::vector<Value>& items);
   };
