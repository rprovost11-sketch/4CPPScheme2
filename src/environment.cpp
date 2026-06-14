// Environment.cpp -- lexical environment and Scheme runtime-error hierarchy.
// Direct port of pyscheme/Environment.py.
#include "Environment.h"
#include "PrettyPrinter.h"

// ── Module-private helpers ────────────────────────────────────────────────────
// Port of Environment.py _display_name (delegates to AST gensym_display_name).

static std::string display_name(uint32_t sid)
   {
   // Gensym-stripped name for error messages (see AST gensym_display_name).
   return gensym_display_name(symbol_name(sid));
   }

// ── PositionedSchemeError ─────────────────────────────────────────────────────

PositionedSchemeError::PositionedSchemeError(std::string message, SourceInfo* source)
    : msg(std::move(message)), src(source ? new SourceInfo(*source) : nullptr) {}

PositionedSchemeError::~PositionedSchemeError()
   {
   delete src;
   }

PositionedSchemeError::PositionedSchemeError(const PositionedSchemeError& o)
    : std::exception(), msg(o.msg),
      src(o.src ? new SourceInfo(*o.src) : nullptr),
      call_stack(o.call_stack) {}

PositionedSchemeError& PositionedSchemeError::operator=(const PositionedSchemeError& o)
   {
   if (this != &o)
      {
      msg = o.msg;
      delete src;
      src = o.src ? new SourceInfo(*o.src) : nullptr;
      call_stack = o.call_stack;
      }
   return *this;
   }

PositionedSchemeError::PositionedSchemeError(PositionedSchemeError&& o) noexcept
    : std::exception(), msg(std::move(o.msg)), src(o.src), call_stack(o.call_stack)
   {
   o.src = nullptr;
   }

PositionedSchemeError& PositionedSchemeError::operator=(PositionedSchemeError&& o) noexcept
   {
   if (this != &o)
      {
      msg = std::move(o.msg);
      delete src;
      src = o.src;
      o.src = nullptr;
      call_stack = o.call_stack;
      }
   return *this;
   }

const char* PositionedSchemeError::what() const noexcept
   {
   return msg.c_str();
   }

std::string PositionedSchemeError::str() const
   {
   return format_with_caret(msg, src);
   }

// ── SchemeRaised ──────────────────────────────────────────────────────────────

SchemeRaised::SchemeRaised(Value val, SourceInfo* source, bool cont)
    : PositionedSchemeError(scheme_pretty_print(val), source),
      value(std::move(val)),
      continuable(cont) {}

SchemeRaised::SchemeRaised(std::string prebuilt_msg, Value val,
                           SourceInfo* source, bool cont)
    : PositionedSchemeError(std::move(prebuilt_msg), source),
      value(std::move(val)),
      continuable(cont) {}

// ── SchemeFileError ───────────────────────────────────────────────────────────
// Port of Environment.py SchemeFileError: bypasses SchemeRaised.__init__,
// calls _PositionedSchemeError.__init__(self, message, src) directly.

SchemeFileError::SchemeFileError(const std::string& message, SourceInfo* source)
    : SchemeRaised(message, make_file_error_object(message, {}), source, false) {}

// ── SchemeUserError ───────────────────────────────────────────────────────────
// Port of Environment.py SchemeUserError: bypasses SchemeRaised.__init__,
// builds display = message + ' ' + pp(irr[0]) + ' ' + pp(irr[1]) + ...

static std::string build_user_error_display(const std::string& message,
                                            const std::vector<Value>& irritants)
   {
   std::string d = message;
   for (const Value& irr : irritants)
      d += ' ' + scheme_pretty_print(irr);
   return d;
   }

SchemeUserError::SchemeUserError(const std::string& message,
                                 std::vector<Value> irritants,
                                 SourceInfo* source)
    : SchemeRaised(build_user_error_display(message, irritants),
                   make_error_object(message, irritants),
                   source, false) {}

// ── arity_mismatch_msg ────────────────────────────────────────────────────────

std::string arity_mismatch_msg(const std::string& name,
                               int lo, int hi, int n_provided)
   {
   std::string provided = (n_provided == 1)
                              ? "1 argument provided"
                              : std::to_string(n_provided) + " arguments provided";
   std::string expected;
   if (hi == -1)
      expected = "at least " + std::to_string(lo) + " expected";
   else if (hi == lo)
      expected = std::to_string(lo) + " expected";
   else if (hi == lo + 1)
      expected = std::to_string(lo) + " or " + std::to_string(hi) + " expected";
   else
      expected = std::to_string(lo) + " to " + std::to_string(hi) + " expected";
   if (!name.empty())
      return name + ": " + provided + "; " + expected;
   return provided + "; " + expected;
   }

// ── Environment ───────────────────────────────────────────────────────────────

Environment::Environment(Environment* parent,
                         std::unordered_map<std::string, Value> initial_bindings)
    : _parent(parent), _is_immutable(false)
   {
   for (auto& [k, v] : initial_bindings)
      _bindings[intern_symbol(k)] = std::move(v);
   _global_env = (parent == nullptr) ? this : parent->_global_env;
   }

Value Environment::bind(const std::string& key, Value value)
   {
   uint32_t sid = intern_symbol(key);
   return bind_id(sid, std::move(value));
   }

Value Environment::bind_id(uint32_t sid, Value value)
   {
   if (_is_immutable)
      throw SchemeTypeError("cannot define '" + display_name(sid) + "' in a frozen environment");
   _bindings[sid] = value;
   gc_write_barrier(&header, gc_value_header(value));
   return value;
   }

void Environment::freeze()
   {
   _is_immutable = true;
   }

void Environment::register_alias(uint32_t gs, uint32_t target,
                                 Environment* def_env, Value copy_value)
   {
   Value cell = make_alias_cell(target, def_env, std::move(copy_value));
   Environment* g = _global_env;
   g->_bindings[gs] = cell;
   gc_write_barrier(&g->header, gc_value_header(cell));
   }

Environment* Environment::getGlobalEnv() const
   {
   return _global_env;
   }

Value Environment::lookup(const std::string& key) const
   {
   return lookup_id(intern_symbol(key));
   }

Value Environment::lookup_id(uint32_t sid) const
   {
   const Environment* scope = this;
   while (scope)
      {
      auto it = scope->_bindings.find(sid);
      if (it != scope->_bindings.end())
         {
         if (is_alias_cell(it->second))
            {
            AliasCell* ac = as_alias_cell(it->second);
            auto r = ac->def_env->lookup_optional_id(ac->target);
            return r ? *r : ac->copy;
            }
         return it->second;
         }
      scope = scope->_parent;
      }
   throw SchemeUnboundError("unbound variable: " + display_name(sid));
   }

std::optional<Value> Environment::lookup_optional(const std::string& key) const
   {
   return lookup_optional_id(intern_symbol(key));
   }

std::optional<Value> Environment::lookup_optional_id(uint32_t sid) const
   {
   const Environment* scope = this;
   while (scope)
      {
      auto it = scope->_bindings.find(sid);
      if (it != scope->_bindings.end())
         {
         if (is_alias_cell(it->second))
            {
            AliasCell* ac = as_alias_cell(it->second);
            auto r = ac->def_env->lookup_optional_id(ac->target);
            return r ? *r : ac->copy;
            }
         return it->second;
         }
      scope = scope->_parent;
      }
   return std::nullopt;
   }

Value Environment::set(const std::string& key, Value value)
   {
   return set_id(intern_symbol(key), std::move(value));
   }

Value Environment::set_id(uint32_t sid, Value value)
   {
   Environment* scope = this;
   while (scope)
      {
      auto it = scope->_bindings.find(sid);
      if (it != scope->_bindings.end())
         {
         if (scope->_is_immutable)
            throw SchemeTypeError("set! on '" + display_name(sid) + "' in a frozen environment");
         if (is_alias_cell(it->second))
            {
            AliasCell* ac = as_alias_cell(it->second);
            if (ac->def_env->lookup_optional_id(ac->target))
               ac->def_env->set_id(ac->target, value);
            else
               {
               ac->copy = value;
               gc_write_barrier(&ac->header, gc_value_header(value));
               }
            return value;
            }
         it->second = value;
         gc_write_barrier(&scope->header, gc_value_header(value));
         return value;
         }
      scope = scope->_parent;
      }
   throw SchemeUnboundError("set! on unbound variable: " + display_name(sid));
   }

// ── GC allocator ─────────────────────────────────────────────────────────────
// Phase 1 stub replaced: allocate on the heap, register with GC young list.

Environment* gc_alloc_environment(Environment* parent)
   {
   auto* e = new Environment{parent};
   gc_register_young(&e->header);
   return e;
   }

EnvBox* gc_alloc_env_box(Environment* env)
   {
   auto* b = new EnvBox{env};
   gc_register_young(&b->header);
   return b;
   }

// ── GC trace hook ─────────────────────────────────────────────────────────────
// Called by gc_gen.cpp's mark_environment to walk Environment's children.
// gc_trace_value / gc_trace_environment are GC-phase-aware; minor_only is
// handled internally via g_minor_gc_active in gc_gen.cpp.

void gc_trace_environment_children(Environment* env, bool /*minor_only*/)
   {
   for (auto& [sid, val] : env->_bindings)
      gc_trace_value(val);
   gc_trace_environment(env->_parent);
   gc_trace_environment(env->_global_env);
   }

// ── GC forward hook ───────────────────────────────────────────────────────────
// Called during minor GC pointer-update pass to forward stale nursery pointers.

void gc_forward_environment_children(Environment* env)
   {
   for (auto& [sid, val] : env->_bindings)
      gc_copy_forward_value(val);
   gc_copy_forward_env(env->_parent);
   gc_copy_forward_env(env->_global_env);
   }
