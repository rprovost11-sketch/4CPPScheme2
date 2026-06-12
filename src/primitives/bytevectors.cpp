// primitives/bytevectors.cpp -- bytevector primitives.
// Direct port of pyscheme/primitives/bytevectors.py.
#include "bytevectors.h"
#include "primitives.h"
#include "../AST.h"
#include "../Environment.h"
#include <cstring>
#include <stdexcept>

static const char* CATEGORY = "bytevectors";

static SourceInfo* _src(const Value* a)
   {
   return a ? src_of(*a) : nullptr;
   }

static std::vector<uint8_t>& _check_bv(const Value& v, const char* name, const Value* app, int idx = 1)
   {
   if (!is_bytevector(v))
      throw SchemeTypeError(
          std::string(name) + ": argument " + std::to_string(idx) + " must be a bytevector",
          _src(app));
   return as_bytevector_items(const_cast<Value&>(v));
   }

static uint8_t _check_u8(const Value& v, const char* name, const Value* app)
   {
   if (!is_integer(v))
      throw SchemeTypeError(std::string(name) + ": byte must be an integer", _src(app));
   int64_t n = as_integer(v);
   if (n < 0 || n > 255)
      throw SchemeTypeError(
          std::string(name) + ": byte " + std::to_string(n) + " out of u8 range (0..255)",
          _src(app));
   return static_cast<uint8_t>(n);
   }

static Value _prim_bytevector_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   return make_boolean(is_bytevector(args[0]));
   }

static Value _prim_make_bytevector(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_integer(args[0]))
      throw SchemeTypeError("make-bytevector: length must be an integer", _src(app));
   int64_t k = as_integer(args[0]);
   if (k < 0)
      throw SchemeTypeError("make-bytevector: length must be non-negative", _src(app));
   uint8_t fill = args.size() >= 2 ? _check_u8(args[1], "make-bytevector", app) : 0;
   return make_bytevector(std::vector<uint8_t>(static_cast<size_t>(k), fill));
   }

static Value _prim_bytevector(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   std::vector<uint8_t> bs;
   bs.reserve(args.size());
   for (size_t i = 0; i < args.size(); ++i)
      bs.push_back(_check_u8(args[i], "bytevector", app));
   return make_bytevector(std::move(bs));
   }

static Value _prim_bytevector_length(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   return make_integer(static_cast<int64_t>(_check_bv(args[0], "bytevector-length", app).size()));
   }

static Value _prim_bytevector_u8_ref(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   auto& bs = _check_bv(args[0], "bytevector-u8-ref", app);
   int64_t k = check_index(args[1], "bytevector-u8-ref", static_cast<int64_t>(bs.size()), app);
   return make_integer(static_cast<int64_t>(bs[static_cast<size_t>(k)]));
   }

static Value _prim_bytevector_u8_set(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   auto& bs = _check_bv(args[0], "bytevector-u8-set!", app);
   if (is_immutable(args[0]))
      throw SchemeTypeError("bytevector-u8-set!: argument is an immutable literal", _src(app));
   int64_t k = check_index(args[1], "bytevector-u8-set!", static_cast<int64_t>(bs.size()), app);
   bs[static_cast<size_t>(k)] = _check_u8(args[2], "bytevector-u8-set!", app);
   return VOID_VALUE;
   }

static Value _prim_bytevector_copy(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   auto& bs = _check_bv(args[0], "bytevector-copy", app);
   auto [start, end] = parse_start_end(args, 1, static_cast<int64_t>(bs.size()),
                                       "bytevector-copy", app, "range out of bounds");
   return make_bytevector(std::vector<uint8_t>(bs.begin() + start, bs.begin() + end));
   }

static Value _prim_bytevector_copy_bang(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   auto& dst = _check_bv(args[0], "bytevector-copy!", app, 1);
   if (is_immutable(args[0]))
      throw SchemeTypeError("bytevector-copy!: destination is an immutable literal", _src(app));
   if (!is_integer(args[1]))
      throw SchemeTypeError("bytevector-copy!: at must be an integer", _src(app));
   int64_t at = as_integer(args[1]);
   auto& src = _check_bv(args[2], "bytevector-copy!", app, 3);
   auto [start, end] = parse_start_end(args, 3, static_cast<int64_t>(src.size()),
                                       "bytevector-copy!", app, "range out of bounds");
   int64_t count = end - start;
   if (at < 0 || at + count > static_cast<int64_t>(dst.size()))
      throw SchemeTypeError("bytevector-copy!: range out of bounds", _src(app));
   // Use memmove for potential overlap (dst and src may be same bytevector)
   std::memmove(dst.data() + at, src.data() + start, static_cast<size_t>(count));
   return VOID_VALUE;
   }

static Value _prim_bytevector_append(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   std::vector<uint8_t> result;
   for (size_t i = 0; i < args.size(); ++i)
      {
      auto& bs = _check_bv(args[i], "bytevector-append", app, static_cast<int>(i + 1));
      result.insert(result.end(), bs.begin(), bs.end());
      }
   return make_bytevector(std::move(result));
   }

// Validate that [data, data+len) is well-formed UTF-8 (proper lead and
// continuation bytes, no overlong forms, no surrogates, in range).  Mirrors
// the check Python's bytes.decode('utf-8') performs in bytevectors.py's
// utf8->string, which raises on invalid input (C++ strings are already the
// raw UTF-8 bytes, so here we validate then copy rather than decode).
static bool _is_valid_utf8(const uint8_t* data, size_t len)
   {
   size_t i = 0;
   while (i < len)
      {
      uint8_t b = data[i];
      size_t need;
      uint32_t cp;
      uint32_t lo;
      if (b < 0x80)
         { ++i; continue; }
      else if ((b & 0xE0) == 0xC0)
         { need = 1; cp = b & 0x1F; lo = 0x80; }
      else if ((b & 0xF0) == 0xE0)
         { need = 2; cp = b & 0x0F; lo = 0x800; }
      else if ((b & 0xF8) == 0xF0)
         { need = 3; cp = b & 0x07; lo = 0x10000; }
      else
         return false;
      if (i + need >= len)
         return false;
      for (size_t k = 1; k <= need; ++k)
         {
         uint8_t c = data[i + k];
         if ((c & 0xC0) != 0x80)
            return false;
         cp = (cp << 6) | (c & 0x3F);
         }
      if (cp < lo || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
         return false;
      i += need + 1;
      }
   return true;
   }

static Value _prim_utf8_to_string(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   auto& bs = _check_bv(args[0], "utf8->string", app);
   auto [start, end] = parse_start_end(args, 1, static_cast<int64_t>(bs.size()),
                                       "utf8->string", app, "range out of bounds");
   if (!_is_valid_utf8(bs.data() + start, static_cast<size_t>(end - start)))
      throw SchemeTypeError("utf8->string: invalid UTF-8", _src(app));
   return make_string(std::string(reinterpret_cast<const char*>(bs.data()) + start,
                                  static_cast<size_t>(end - start)));
   }

static Value _prim_string_to_utf8(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_string(args[0]))
      throw SchemeTypeError("string->utf8: argument must be a string", _src(app));
   const std::string& s = as_string(args[0]);
   auto [start, end] = parse_start_end(args, 1, static_cast<int64_t>(s.size()),
                                       "string->utf8", app, "range out of bounds");
   const uint8_t* p = reinterpret_cast<const uint8_t*>(s.data()) + start;
   return make_bytevector(std::vector<uint8_t>(p, p + (end - start)));
   }

void register_bytevectors()
   {
   register_primitive("bytevector?", 1, 1, _prim_bytevector_p, "", "Return #t if obj is a bytevector.  R7RS 6.9.", CATEGORY);
   register_primitive("make-bytevector", 1, 2, _prim_make_bytevector, "",
                      "(make-bytevector k [byte]) returns a bytevector of length k filled with byte (default 0).  R7RS 6.9.", CATEGORY);
   register_primitive("bytevector", 0, -1, _prim_bytevector, "", "Return a bytevector containing the integer arguments.  R7RS 6.9.", CATEGORY);
   register_primitive("bytevector-length", 1, 1, _prim_bytevector_length, "", "Return the number of bytes in the bytevector.  R7RS 6.9.", CATEGORY);
   register_primitive("bytevector-u8-ref", 2, 2, _prim_bytevector_u8_ref, "", "(bytevector-u8-ref bv k) returns the kth byte as an integer.  R7RS 6.9.", CATEGORY);
   register_primitive("bytevector-u8-set!", 3, 3, _prim_bytevector_u8_set, "", "(bytevector-u8-set! bv k byte) stores byte at index k.  R7RS 6.9.", CATEGORY);
   register_primitive("bytevector-copy", 1, 3, _prim_bytevector_copy, "",
                      "(bytevector-copy bv [start [end]]) returns a copy of (a slice of) the bytevector.  R7RS 6.9.", CATEGORY);
   register_primitive("bytevector-copy!", 3, 5, _prim_bytevector_copy_bang, "",
                      "(bytevector-copy! dst at src [start [end]]) copies bytes from src[start..end] into dst starting at index at.  R7RS 6.9.", CATEGORY);
   register_primitive("bytevector-append", 0, -1, _prim_bytevector_append, "", "Concatenate bytevector arguments into a new bytevector.  R7RS 6.9.", CATEGORY);
   register_primitive("utf8->string", 1, 3, _prim_utf8_to_string, "",
                      "(utf8->string bv [start [end]]) decodes UTF-8 bytes to a string.  R7RS 6.9.", CATEGORY);
   register_primitive("string->utf8", 1, 3, _prim_string_to_utf8, "",
                      "(string->utf8 string [start [end]]) encodes a string as a UTF-8 bytevector.  R7RS 6.9.", CATEGORY);
   }
