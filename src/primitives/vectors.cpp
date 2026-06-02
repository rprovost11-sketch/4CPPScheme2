// primitives/vectors.cpp -- vector primitives.
// Direct port of pyscheme/primitives/vectors.py.
#include "vectors.h"
#include "primitives.h"
#include "../AST.h"
#include "../Environment.h"
#include "../Evaluator.h"
#include "../gc.h"

static const char* CATEGORY = "vectors";

static SourceInfo* _src(const Value* a)
   {
   return a ? src_of(*a) : nullptr;
   }

static std::vector<Value>& _check_vector(const Value& v, const char* name, const Value* app, int idx = 1)
   {
   if (!is_vector(v))
      throw SchemeTypeError(
          std::string(name) + ": argument " + std::to_string(idx) + " must be a vector",
          _src(app));
   return as_vector_items(const_cast<Value&>(v));
   }

static int64_t _check_index(const Value& v, const char* name, size_t length, const Value* app)
   {
   if (!is_integer(v))
      throw SchemeTypeError(std::string(name) + ": index must be an integer", _src(app));
   int64_t k = as_integer(v);
   if (k < 0 || static_cast<size_t>(k) >= length)
      throw SchemeTypeError(
          std::string(name) + ": index " + std::to_string(k) + " out of range", _src(app));
   return k;
   }

static Value _prim_vector_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   return make_boolean(is_vector(args[0]));
   }

static Value _prim_make_vector(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_integer(args[0]))
      throw SchemeTypeError("make-vector: length must be an integer", _src(app));
   int64_t k = as_integer(args[0]);
   if (k < 0)
      throw SchemeTypeError("make-vector: length must be non-negative", _src(app));
   Value fill = args.size() >= 2 ? args[1] : VOID_VALUE;
   return make_vector(std::vector<Value>(static_cast<size_t>(k), fill));
   }

static Value _prim_vector(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   return make_vector(std::vector<Value>(args.begin(), args.end()));
   }

static Value _prim_vector_length(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   return make_integer(static_cast<int64_t>(_check_vector(args[0], "vector-length", app).size()));
   }

static Value _prim_vector_ref(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   auto& items = _check_vector(args[0], "vector-ref", app);
   int64_t k = _check_index(args[1], "vector-ref", items.size(), app);
   return items[static_cast<size_t>(k)];
   }

static Value _prim_vector_set(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   auto& items = _check_vector(args[0], "vector-set!", app);
   if (is_immutable(args[0]))
      throw SchemeTypeError("vector-set!: argument is an immutable literal", _src(app));
   int64_t k = _check_index(args[1], "vector-set!", items.size(), app);
   items[static_cast<size_t>(k)] = args[2];
   gc_write_barrier(gc_value_header(args[0]), gc_value_header(args[2]));
   return VOID_VALUE;
   }

static Value _prim_vector_to_list(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   auto& items = _check_vector(args[0], "vector->list", app);
   int64_t start = 0, end = static_cast<int64_t>(items.size());
   if (args.size() >= 2)
      {
      if (!is_integer(args[1]))
         throw SchemeTypeError("vector->list: start must be an integer", _src(app));
      start = as_integer(args[1]);
      }
   if (args.size() >= 3)
      {
      if (!is_integer(args[2]))
         throw SchemeTypeError("vector->list: end must be an integer", _src(app));
      end = as_integer(args[2]);
      }
   if (start < 0 || end > static_cast<int64_t>(items.size()) || start > end)
      throw SchemeTypeError("vector->list: range out of bounds", _src(app));
   Value result = NIL_VALUE;
   for (int64_t i = end - 1; i >= start; --i)
      result = alloc_cons(items[static_cast<size_t>(i)], result);
   return result;
   }

static Value _prim_list_to_vector(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   std::vector<Value> collected;
   Value cur = args[0];
   while (is_cons(cur))
      {
      collected.push_back(car(cur));
      cur = cdr(cur);
      }
   if (!is_nil(cur))
      throw SchemeTypeError("list->vector: argument must be a proper list", _src(app));
   return make_vector(std::move(collected));
   }

static Value _prim_vector_fill(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   auto& items = _check_vector(args[0], "vector-fill!", app);
   if (is_immutable(args[0]))
      throw SchemeTypeError("vector-fill!: argument is an immutable literal", _src(app));
   Value fill = args[1];
   int64_t start = 0, end = static_cast<int64_t>(items.size());
   if (args.size() >= 3)
      {
      if (!is_integer(args[2]))
         throw SchemeTypeError("vector-fill!: start must be an integer", _src(app));
      start = as_integer(args[2]);
      }
   if (args.size() >= 4)
      {
      if (!is_integer(args[3]))
         throw SchemeTypeError("vector-fill!: end must be an integer", _src(app));
      end = as_integer(args[3]);
      }
   if (start < 0 || end > static_cast<int64_t>(items.size()) || start > end)
      throw SchemeTypeError("vector-fill!: range out of bounds", _src(app));
   for (int64_t i = start; i < end; ++i)
      items[static_cast<size_t>(i)] = fill;
   return VOID_VALUE;
   }

static Value _prim_vector_copy(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   auto& items = _check_vector(args[0], "vector-copy", app);
   int64_t start = 0, end = static_cast<int64_t>(items.size());
   if (args.size() >= 2)
      {
      if (!is_integer(args[1]))
         throw SchemeTypeError("vector-copy: start must be an integer", _src(app));
      start = as_integer(args[1]);
      }
   if (args.size() >= 3)
      {
      if (!is_integer(args[2]))
         throw SchemeTypeError("vector-copy: end must be an integer", _src(app));
      end = as_integer(args[2]);
      }
   if (start < 0 || end > static_cast<int64_t>(items.size()) || start > end)
      throw SchemeTypeError("vector-copy: range out of bounds", _src(app));
   return make_vector(std::vector<Value>(items.begin() + start, items.begin() + end));
   }

static Value _prim_vector_copy_bang(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   auto& dst = _check_vector(args[0], "vector-copy!", app, 1);
   if (is_immutable(args[0]))
      throw SchemeTypeError("vector-copy!: destination is an immutable literal", _src(app));
   if (!is_integer(args[1]))
      throw SchemeTypeError("vector-copy!: at must be an integer", _src(app));
   int64_t at = as_integer(args[1]);
   auto& src = _check_vector(args[2], "vector-copy!", app, 3);
   int64_t start = 0, end = static_cast<int64_t>(src.size());
   if (args.size() >= 4)
      {
      if (!is_integer(args[3]))
         throw SchemeTypeError("vector-copy!: start must be an integer", _src(app));
      start = as_integer(args[3]);
      }
   if (args.size() >= 5)
      {
      if (!is_integer(args[4]))
         throw SchemeTypeError("vector-copy!: end must be an integer", _src(app));
      end = as_integer(args[4]);
      }
   int64_t count = end - start;
   if (at < 0 || start < 0 || end > static_cast<int64_t>(src.size()) || start > end || at + count > static_cast<int64_t>(dst.size()))
      throw SchemeTypeError("vector-copy!: range out of bounds", _src(app));
   // Copy with memmove semantics for aliased dst==src case.
   if (&dst == &src && at > start)
      {
      for (int64_t i = count - 1; i >= 0; --i)
         dst[static_cast<size_t>(at + i)] = src[static_cast<size_t>(start + i)];
      }
   else
      {
      for (int64_t i = 0; i < count; ++i)
         dst[static_cast<size_t>(at + i)] = src[static_cast<size_t>(start + i)];
      }
   return VOID_VALUE;
   }

static Value _prim_vector_append(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   std::vector<Value> result;
   for (size_t i = 0; i < args.size(); ++i)
      {
      auto& v = _check_vector(args[i], "vector-append", app, static_cast<int>(i + 1));
      result.insert(result.end(), v.begin(), v.end());
      }
   return make_vector(std::move(result));
   }

static Value _prim_vector_to_string(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   auto& items = _check_vector(args[0], "vector->string", app);
   int64_t start = 0, end = static_cast<int64_t>(items.size());
   if (args.size() >= 2)
      {
      if (!is_integer(args[1]))
         throw SchemeTypeError("vector->string: start must be an integer", _src(app));
      start = as_integer(args[1]);
      }
   if (args.size() >= 3)
      {
      if (!is_integer(args[2]))
         throw SchemeTypeError("vector->string: end must be an integer", _src(app));
      end = as_integer(args[2]);
      }
   if (start < 0 || end > static_cast<int64_t>(items.size()) || start > end)
      throw SchemeTypeError("vector->string: range out of bounds", _src(app));
   std::string result;
   for (int64_t i = start; i < end; ++i)
      {
      if (!is_character(items[static_cast<size_t>(i)]))
         throw SchemeTypeError("vector->string: vector elements must be characters", _src(app));
      char32_t c = as_character(items[static_cast<size_t>(i)]);
      // UTF-8 encode the character
      if (c < 0x80)
         {
         result += static_cast<char>(c);
         }
      else if (c < 0x800)
         {
         result += static_cast<char>(0xC0 | (c >> 6));
         result += static_cast<char>(0x80 | (c & 0x3F));
         }
      else if (c < 0x10000)
         {
         result += static_cast<char>(0xE0 | (c >> 12));
         result += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
         result += static_cast<char>(0x80 | (c & 0x3F));
         }
      else
         {
         result += static_cast<char>(0xF0 | (c >> 18));
         result += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
         result += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
         result += static_cast<char>(0x80 | (c & 0x3F));
         }
      }
   return make_string(result);
   }

static Value _prim_string_to_vector(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_string(args[0]))
      throw SchemeTypeError("string->vector: argument must be a string", _src(app));
   const std::string& s = as_string(args[0]);
   int64_t start = 0, end = static_cast<int64_t>(s.size());
   if (args.size() >= 2)
      {
      if (!is_integer(args[1]))
         throw SchemeTypeError("string->vector: start must be an integer", _src(app));
      start = as_integer(args[1]);
      }
   if (args.size() >= 3)
      {
      if (!is_integer(args[2]))
         throw SchemeTypeError("string->vector: end must be an integer", _src(app));
      end = as_integer(args[2]);
      }
   if (start < 0 || end > static_cast<int64_t>(s.size()) || start > end)
      throw SchemeTypeError("string->vector: range out of bounds", _src(app));
   std::vector<Value> chars;
   for (int64_t i = start; i < end; ++i)
      chars.push_back(make_character(static_cast<char32_t>(static_cast<uint8_t>(s[static_cast<size_t>(i)]))));
   return make_vector(std::move(chars));
   }

static Value _prim_vector_for_each(Context* ctx, Environment* env, std::vector<Value>& args, const Value* app)
   {
   if (args.size() < 2)
      throw SchemeArityError(arity_mismatch_msg("vector-for-each", 2, -1, static_cast<int>(args.size())), _src(app));
   GcRootVec args_root(args); // keep SchemeVector objects alive across apply_scheme_proc calls
   GcRootGuard proc(args[0]);
   std::vector<std::vector<Value>*> vecs;
   for (size_t i = 1; i < args.size(); ++i)
      vecs.push_back(&_check_vector(args[i], "vector-for-each", app, static_cast<int>(i + 1)));
   size_t shortest = vecs[0]->size();
   for (size_t vi = 1; vi < vecs.size(); ++vi)
      if (vecs[vi]->size() < shortest)
         shortest = vecs[vi]->size();
   for (size_t i = 0; i < shortest; ++i)
      {
      std::vector<Value> row;
      GcRootVec row_root(row);
      for (auto* v : vecs)
         row.push_back((*v)[i]);
      apply_scheme_proc(proc.val, std::move(row), ctx, env, app);
      }
   return VOID_VALUE;
   }

static Value _prim_vector_map(Context* ctx, Environment* env, std::vector<Value>& args, const Value* app)
   {
   if (args.size() < 2)
      throw SchemeArityError(arity_mismatch_msg("vector-map", 2, -1, static_cast<int>(args.size())), _src(app));
   GcRootVec args_root(args); // keep SchemeVector objects alive across apply_scheme_proc calls
   GcRootGuard proc(args[0]);
   std::vector<std::vector<Value>*> vecs;
   for (size_t i = 1; i < args.size(); ++i)
      vecs.push_back(&_check_vector(args[i], "vector-map", app, static_cast<int>(i + 1)));
   size_t shortest = vecs[0]->size();
   for (size_t vi = 1; vi < vecs.size(); ++vi)
      if (vecs[vi]->size() < shortest)
         shortest = vecs[vi]->size();
   std::vector<Value> collected;
   collected.reserve(shortest);
   GcRootVec collected_root(collected);
   for (size_t i = 0; i < shortest; ++i)
      {
      std::vector<Value> row;
      GcRootVec row_root(row);
      for (auto* v : vecs)
         row.push_back((*v)[i]);
      collected.push_back(apply_scheme_proc(proc.val, std::move(row), ctx, env, app));
      }
   return make_vector(std::move(collected));
   }

void register_vectors()
   {
   register_primitive("vector?", 1, 1, _prim_vector_p, "", "Return #t if obj is a vector.  R7RS 6.8.", CATEGORY);
   register_primitive("make-vector", 1, 2, _prim_make_vector, "",
                      "(make-vector k [fill]) returns a vector of length k filled with fill (default unspecified).  R7RS 6.8.", CATEGORY);
   register_primitive("vector", 0, -1, _prim_vector, "", "Return a vector containing the arguments.  R7RS 6.8.", CATEGORY);
   register_primitive("vector-length", 1, 1, _prim_vector_length, "", "Return the number of elements in vector.  R7RS 6.8.", CATEGORY);
   register_primitive("vector-ref", 2, 2, _prim_vector_ref, "", "(vector-ref vec k) returns the kth element.  R7RS 6.8.", CATEGORY);
   register_primitive("vector-set!", 3, 3, _prim_vector_set, "", "(vector-set! vec k obj) stores obj at index k; returns void.  R7RS 6.8.", CATEGORY);
   register_primitive("vector->list", 1, 3, _prim_vector_to_list, "",
                      "(vector->list vec [start [end]]) returns a list of the vector's (sliced) elements.  R7RS 6.8.", CATEGORY);
   register_primitive("list->vector", 1, 1, _prim_list_to_vector, "", "Return a vector built from the list.  R7RS 6.8.", CATEGORY);
   register_primitive("vector-fill!", 2, 4, _prim_vector_fill, "",
                      "(vector-fill! vec fill [start [end]]) sets each element of the (sliced) vector to fill.  R7RS 6.8.", CATEGORY);
   register_primitive("vector-copy", 1, 3, _prim_vector_copy, "",
                      "(vector-copy vec [start [end]]) returns a new vector containing a copy of the (sliced) elements.  R7RS 6.8.", CATEGORY);
   register_primitive("vector-copy!", 3, 5, _prim_vector_copy_bang, "",
                      "(vector-copy! dst at src [start [end]]) copies elements from src[start..end] into dst starting at index at.  R7RS 6.8.", CATEGORY);
   register_primitive("vector-append", 0, -1, _prim_vector_append, "", "Return a vector that is the concatenation of its arguments.  R7RS 6.8.", CATEGORY);
   register_primitive("vector->string", 1, 3, _prim_vector_to_string, "", "Return a string built from the (sliced) vector's character elements.  R7RS 6.8.", CATEGORY);
   register_primitive("string->vector", 1, 3, _prim_string_to_vector, "", "Return a vector of the characters in the (sliced) string.  R7RS 6.8.", CATEGORY);
   register_primitive("vector-for-each", 2, -1, _prim_vector_for_each, "",
                      "(vector-for-each proc vec1 vec2 ...) applies proc element-wise across the vectors for effect.  R7RS 6.8.", CATEGORY);
   register_primitive("vector-map", 2, -1, _prim_vector_map, "",
                      "(vector-map proc vec1 vec2 ...) applies proc element-wise across the vectors and returns a vector of results.  R7RS 6.8.", CATEGORY);
   }
