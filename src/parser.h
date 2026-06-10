#pragma once
// Parser.h -- Scheme source reader.
// Direct port of pyscheme/Parser.py.
#include "AST.h"
#include "Environment.h"
#include <string>
#include <vector>

// ── TokenKind ─────────────────────────────────────────────────────────────────
// Port of Parser.py TOK_* constants.

enum class TokenKind
   {
   LPAREN,
   RPAREN,
   LBRACKET,
   RBRACKET,
   VECTOR_LPAREN,
   BYTEVECTOR_LPAREN,
   QUOTE,
   QUASIQUOTE,
   UNQUOTE,
   UNQUOTE_SPLICING,
   DOT,
   INT,
   REAL,
   RATIONAL,
   COMPLEX,
   EXACT_COMPLEX,
   STRING,
   CHAR,
   BOOL,
   IDENT,
   END_OF_FILE,
   DATUM_COMMENT,
   LABEL_DEF,
   LABEL_REF,
   };

// ── Token ─────────────────────────────────────────────────────────────────────
// Port of Parser.py Token class.
// src is stored by value (Token owns it — no heap ownership issues).
// Payload fields are set only for the relevant kind.

struct CPPSCHEME2_API Token
   {
   TokenKind kind;
   SourceInfo src;

   // Numeric / label payloads
   int64_t int_val = 0;   // INT, LABEL_DEF, LABEL_REF, RATIONAL numerator
   int64_t int_val2 = 0;  // RATIONAL denominator
   double dbl_val = 0.0;  // REAL, COMPLEX real part
   double dbl_val2 = 0.0; // COMPLEX imaginary part

   // Other payloads
   bool bool_val = false; // BOOL
   char32_t char_val = 0; // CHAR
   std::string str_val;   // STRING content, IDENT name

   // EXACT_COMPLEX components (Scheme Values)
   Value val;  // real part
   Value val2; // imaginary part

   Token(TokenKind k, SourceInfo s)
       : kind(k), src(std::move(s)) {}
   };

// ── SchemeSyntaxError ──────────────────────────────────────────────────────────
// Port of Parser.py SchemeSyntaxError.

class CPPSCHEME2_API SchemeSyntaxError : public PositionedSchemeError
   {
 public:
   using PositionedSchemeError::PositionedSchemeError;
   };

// ── Public API ────────────────────────────────────────────────────────────────
// Port of Parser.py parse, parse_one, tokenize.
// filename: empty string = no filename (Python None).

CPPSCHEME2_API std::vector<Value> scheme_parse(const std::string& source,
                                               const std::string& filename = "");

CPPSCHEME2_API Value scheme_parse_one(const std::string& source,
                                      const std::string& filename = "");

CPPSCHEME2_API Value scheme_parse_first(const std::string& source,
                                        const std::string& filename = "");

// Overload: also reports the token following the first datum, so a caller (the
// read primitive) can advance an input port past one datum using the parser's
// own end position rather than re-walking nested structure.  Sets at_eof when
// the next token is end-of-input; otherwise copies its position into next_src.
CPPSCHEME2_API Value scheme_parse_first(const std::string& source,
                                        const std::string& filename,
                                        bool& at_eof, SourceInfo& next_src);

CPPSCHEME2_API std::vector<Token> scheme_tokenize(const std::string& source,
                                                  const std::string& filename = "");
