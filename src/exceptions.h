#pragma once
#include "scheme_export.h"
#include <stdexcept>
#include <string>
#include <string_view>

// ── Scheme-level errors ──────────────────────────────────────────────────────

class SCHEME_API SchemeError : public std::runtime_error
   {
public:
   explicit SchemeError(const std::string& message)
      : std::runtime_error(message)
      {}
   };

class SCHEME_API SchemeTypeError : public SchemeError
   {
public:
   SchemeTypeError(const std::string& expected, const std::string& got)
      : SchemeError("type error: expected " + expected + ", got " + got)
      {}
   explicit SchemeTypeError(const std::string& message)
      : SchemeError(message)
      {}
   };

class SCHEME_API SchemeArityError : public SchemeError
   {
public:
   explicit SchemeArityError(const std::string& message)
      : SchemeError("arity error: " + message)
      {}
   };

class SCHEME_API SchemeUnboundError : public SchemeError
   {
public:
   explicit SchemeUnboundError(const std::string& variable_name)
      : SchemeError("unbound variable: " + variable_name)
      {}
   };

class SCHEME_API SchemeSyntaxError : public SchemeError
   {
public:
   explicit SchemeSyntaxError(const std::string& message)
      : SchemeError("syntax error: " + message)
      {}
   };

class SCHEME_API SchemeParseError : public SchemeError
   {
public:
   explicit SchemeParseError(const std::string& message)
      : SchemeError("parse error: " + message)
      {}
   };

class SCHEME_API SchemeDivisionError : public SchemeError
   {
public:
   SchemeDivisionError() : SchemeError("division by zero") {}
   };

class SCHEME_API SchemeFileError : public SchemeError
   {
public:
   explicit SchemeFileError(const std::string& message)
      : SchemeError("file error: " + message)
      {}
   };

class SCHEME_API SchemeReadError : public SchemeError
   {
public:
   explicit SchemeReadError(const std::string& message)
      : SchemeError("read error: " + message)
      {}
   };
