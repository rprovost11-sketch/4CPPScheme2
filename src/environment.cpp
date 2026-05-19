#include "environment.h"
#include "gc.h"
#include "symbol.h"
#include <cctype>


// ── display_name_of ───────────────────────────────────────────────────────────

static const char   GENSYM_PFX[]    = "\x01h.";
static const size_t GENSYM_PFX_LEN  = 3;

std::string display_name_of(uint32_t sid)
   {
   const std::string& name = symbol_name(sid);
   if (name.size() < GENSYM_PFX_LEN
         || name.compare(0, GENSYM_PFX_LEN, GENSYM_PFX) != 0)
      return name;
   std::string rest = name.substr(GENSYM_PFX_LEN);
   size_t dot = rest.rfind('.');
   if (dot != std::string::npos)
      {
      std::string tail = rest.substr(dot + 1);
      bool all_digits = !tail.empty();
      for (char c : tail)
         if (!std::isdigit(static_cast<unsigned char>(c))) { all_digits = false; break; }
      if (all_digits)
         return rest.substr(0, dot);
      }
   return rest;
   }


// ── arity_mismatch_msg ────────────────────────────────────────────────────────

std::string arity_mismatch_msg(std::string_view name, int lo, int hi, int n_provided)
   {
   std::string provided = (n_provided == 1) ? "1 argument provided"
                                            : std::to_string(n_provided) + " arguments provided";
   std::string expected;
   if (hi < 0)
      expected = "at least " + std::to_string(lo) + " expected";
   else if (hi == lo)
      expected = std::to_string(lo) + " expected";
   else if (hi == lo + 1)
      expected = std::to_string(lo) + " or " + std::to_string(hi) + " expected";
   else
      expected = std::to_string(lo) + " to " + std::to_string(hi) + " expected";
   if (name.empty())
      return provided + "; " + expected;
   return std::string(name) + ": " + provided + "; " + expected;
   }


// ── Environment ───────────────────────────────────────────────────────────────

Environment::Environment(Environment* parent_env)
   : parent(parent_env)
   , global(parent_env ? parent_env->global : this)
   {}


Value Environment::lookup_id(uint32_t sid) const
   {
   const Environment* scope = this;
   while (scope)
      {
      auto it = scope->bindings_.find(sid);
      if (it != scope->bindings_.end())
         return it->second;
      scope = scope->parent;
      }
   throw SchemeUnboundError(display_name_of(sid));
   }


std::optional<Value> Environment::lookup_optional_id(uint32_t sid) const
   {
   const Environment* scope = this;
   while (scope)
      {
      auto it = scope->bindings_.find(sid);
      if (it != scope->bindings_.end())
         return it->second;
      scope = scope->parent;
      }
   return std::nullopt;
   }


void Environment::bind_id(uint32_t sid, Value val)
   {
   if (is_frozen)
      throw SchemeTypeError(
         "cannot define '" + display_name_of(sid) + "' in a frozen environment");
   gc_write_barrier(&header, gc_value_header(val));
   bindings_[sid] = val;
   }


void Environment::set_id(uint32_t sid, Value val)
   {
   Environment* scope = this;
   while (scope)
      {
      auto it = scope->bindings_.find(sid);
      if (it != scope->bindings_.end())
         {
         if (scope->is_frozen)
            throw SchemeTypeError(
               "set! on '" + display_name_of(sid) + "' in a frozen environment");
         gc_write_barrier(&scope->header, gc_value_header(val));
         it->second = val;
         return;
         }
      scope = scope->parent;
      }
   throw SchemeUnboundError("set! on unbound variable: " + display_name_of(sid));
   }


void Environment::freeze()
   {
   is_frozen = true;
   }


Value Environment::lookup(std::string_view name) const
   {
   return lookup_id(intern_symbol(name));
   }


std::optional<Value> Environment::lookup_optional(std::string_view name) const
   {
   return lookup_optional_id(intern_symbol(name));
   }


void Environment::bind(std::string_view name, Value val)
   {
   bind_id(intern_symbol(name), val);
   }


void Environment::set(std::string_view name, Value val)
   {
   set_id(intern_symbol(name), val);
   }
