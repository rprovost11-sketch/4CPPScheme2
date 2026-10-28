// Environment.cpp -- lexical environment and Scheme runtime-error hierarchy.
// Direct port of pyscheme/Environment.py.
#include "Environment.h"
#include "PrettyPrinter.h"
#include <cassert>

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

// ── Scope classes ─────────────────────────────────────────────────────────────

static SchemeUnboundError unassigned_error(uint32_t sid)
   {
   return SchemeUnboundError(
       "variable used before its initialization: " + display_name(sid));
   }

// Live value of an alias cell's target in its def_env (always a dict
// scope), else the def-time copy.
static Value alias_read(const AliasCell* ac)
   {
   std::optional<Value> v = ac->def_env->lookup_optional(global_la_key(ac->target));
   return v ? *v : ac->copy;
   }

static void alias_write(AliasCell* ac, Value value)
   {
   LAKey la_key = global_la_key(ac->target);
   if (ac->def_env->lookup_optional(la_key))
      ac->def_env->set(la_key, value);
   else
      {
      ac->copy = value;
      gc_write_barrier(&ac->header, gc_value_header(value));
      }
   }

// ── Environment ───────────────────────────────────────────────────────────────

Environment::Environment(Environment* parent)
    : _parent(parent), _global_env(parent ? parent->_global_env : this),
      _dict_env(parent ? parent->_dict_env : this) {}

GlobalEnvironment* Environment::getGlobalEnv() const
   {
   return static_cast<GlobalEnvironment*>(_global_env);
   }

Environment* Environment::getParent() const
   {
   return _parent;
   }

Environment* Environment::from_header(GcHeader* header)
   {
   return static_cast<Environment*>(reinterpret_cast<EnvironmentGcBase*>(header));
   }

// ── LocalEnvironment ──────────────────────────────────────────────────────────

LocalEnvironment::LocalEnvironment(Environment* parent, ScopeNames names,
                                   std::vector<Value> values)
    : Environment(parent), _names(std::move(names)), _values(std::move(values)) {}

// The Analyzer guarantees every scope within depth hops is local.
const LocalEnvironment* LocalEnvironment::owner(int depth) const
   {
   const LocalEnvironment* scope = this;
   while (depth > 0)
      {
      scope = static_cast<const LocalEnvironment*>(scope->_parent);
      --depth;
      }
   return scope;
   }

LocalEnvironment* LocalEnvironment::owner(int depth)
   {
   LocalEnvironment* scope = this;
   while (depth > 0)
      {
      scope = static_cast<LocalEnvironment*>(scope->_parent);
      --depth;
      }
   return scope;
   }

Value LocalEnvironment::lookup(const LAKey& la_key) const
   {
   if (la_key.depth == GLOBAL_DEPTH)
      return _dict_env->lookup(la_key);
   const Value& v = owner(la_key.depth)->_values[la_key.index];
   if (is_unassigned(v))
      throw unassigned_error(la_key.sid);
   return v;
   }

std::optional<Value> LocalEnvironment::lookup_optional(const LAKey& la_key) const
   {
   if (la_key.depth == GLOBAL_DEPTH)
      return _dict_env->lookup_optional(la_key);
   return owner(la_key.depth)->_values[la_key.index];
   }

Value LocalEnvironment::set(const LAKey& la_key, Value value)
   {
   if (la_key.depth == GLOBAL_DEPTH)
      return _dict_env->set(la_key, value);
   LocalEnvironment* scope = owner(la_key.depth);
   scope->_values[la_key.index] = value;
   gc_write_barrier(&scope->header, gc_value_header(value));
   return value;
   }

// A global LA key here can only come from a define typed at a debugger
// prompt paused in this scope: a local scope has no room for a new name,
// so that is an error.
Value LocalEnvironment::bind(const LAKey& la_key, Value value)
   {
   if (la_key.depth == GLOBAL_DEPTH)
      throw SchemeTypeError("cannot define '" + display_name(la_key.sid) + "' in a local scope");
   assert(la_key.depth == 0); // bind is always into this scope
   _values[la_key.index] = value;
   gc_write_barrier(&header, gc_value_header(value));
   return value;
   }

LAKey LocalEnvironment::resolve(uint32_t sid, int depth) const
   {
   const std::vector<uint32_t>& names = *_names;
   for (size_t i = 0; i < names.size(); ++i)
      {
      if (names[i] == sid)
         return LAKey{depth, static_cast<int>(i), sid};
      }
   return _parent->resolve(sid, depth + 1);
   }

void LocalEnvironment::for_each_binding(const BindingVisitor& fn) const
   {
   const std::vector<uint32_t>& names = *_names;
   for (size_t i = 0; i < names.size(); ++i)
      fn(names[i], _values[i]);
   }

void LocalEnvironment::gc_trace_children()
   {
   for (auto& val : _values)
      gc_trace_value(val);
   gc_trace_environment(_parent);
   gc_trace_environment(_global_env);
   gc_trace_environment(_dict_env);
   }

void LocalEnvironment::gc_forward_children()
   {
   for (auto& val : _values)
      gc_copy_forward_value(val);
   gc_copy_forward_env(_parent);
   gc_copy_forward_env(_global_env);
   gc_copy_forward_env(_dict_env);
   }

// ── GlobalEnvironment ─────────────────────────────────────────────────────────

GlobalEnvironment::GlobalEnvironment(Environment* parent)
    : Environment(parent), _is_immutable(false)
   {
   _dict_env = this;
   }

Value GlobalEnvironment::lookup(const LAKey& la_key) const
   {
   auto it = _bindings.find(la_key.sid);
   if (it == _bindings.end())
      {
      if (_parent)
         return _parent->lookup(la_key);
      throw SchemeUnboundError("unbound variable: " + display_name(la_key.sid));
      }
   if (is_unassigned(it->second))
      throw unassigned_error(la_key.sid);
   if (is_alias_cell(it->second))
      return alias_read(as_alias_cell(it->second));
   return it->second;
   }

std::optional<Value> GlobalEnvironment::lookup_optional(const LAKey& la_key) const
   {
   auto it = _bindings.find(la_key.sid);
   if (it == _bindings.end())
      {
      if (_parent)
         return _parent->lookup_optional(la_key);
      return std::nullopt;
      }
   if (is_alias_cell(it->second))
      return alias_read(as_alias_cell(it->second));
   return it->second;
   }

Value GlobalEnvironment::set(const LAKey& la_key, Value value)
   {
   auto it = _bindings.find(la_key.sid);
   if (it == _bindings.end())
      {
      if (_parent)
         return _parent->set(la_key, value);
      throw SchemeUnboundError("set! on unbound variable: " + display_name(la_key.sid));
      }
   if (_is_immutable)
      throw SchemeTypeError("set! on '" + display_name(la_key.sid) + "' in a frozen environment");
   if (is_alias_cell(it->second))
      {
      alias_write(as_alias_cell(it->second), value);
      return value;
      }
   it->second = value;
   gc_write_barrier(&header, gc_value_header(value));
   return value;
   }

Value GlobalEnvironment::bind(const LAKey& la_key, Value value)
   {
   if (_is_immutable)
      throw SchemeTypeError("cannot define '" + display_name(la_key.sid) + "' in a frozen environment");
   _bindings[la_key.sid] = value;
   gc_write_barrier(&header, gc_value_header(value));
   return value;
   }

LAKey GlobalEnvironment::resolve(uint32_t sid, int /*depth*/) const
   {
   return global_la_key(sid);
   }

void GlobalEnvironment::for_each_binding(const BindingVisitor& fn) const
   {
   for (const auto& [sid, val] : _bindings)
      fn(sid, val);
   }

void GlobalEnvironment::gc_trace_children()
   {
   for (auto& [sid, val] : _bindings)
      gc_trace_value(val);
   gc_trace_environment(_parent);
   gc_trace_environment(_global_env);
   }

void GlobalEnvironment::gc_forward_children()
   {
   for (auto& [sid, val] : _bindings)
      gc_copy_forward_value(val);
   gc_copy_forward_env(_parent);
   gc_copy_forward_env(_global_env);
   }

void GlobalEnvironment::freeze()
   {
   _is_immutable = true;
   }

void GlobalEnvironment::reroot_as_global()
   {
   _global_env = this;
   }

void GlobalEnvironment::register_alias(uint32_t gs, uint32_t target,
                                       Environment* def_env, Value copy_value)
   {
   _global_env->bind(global_la_key(gs),
                     make_alias_cell(target, def_env, std::move(copy_value)));
   }

// ── GC allocators ─────────────────────────────────────────────────────────────
// Allocate on the heap, register with the GC young list.

LocalEnvironment* gc_alloc_local_environment(Environment* parent, ScopeNames names,
                                             std::vector<Value> values)
   {
   auto* e = new LocalEnvironment(parent, std::move(names), std::move(values));
   gc_register_young(&e->header);
   return e;
   }

GlobalEnvironment* gc_alloc_global_environment(Environment* parent)
   {
   auto* e = new GlobalEnvironment(parent);
   gc_register_young(&e->header);
   return e;
   }

EnvBox* gc_alloc_env_box(Environment* env)
   {
   auto* b = new EnvBox{env};
   gc_register_young(&b->header);
   return b;
   }

// ── GC hooks ──────────────────────────────────────────────────────────────────
// Called by gc_gen.cpp, which sees an Environment only through its header:
// trace / forward its children (parent, global env, values).  The trace
// helpers are GC-phase-aware; minor_only is handled internally via
// g_minor_gc_active in gc_gen.cpp.

void gc_trace_environment_children(Environment* env, bool /*minor_only*/)
   {
   env->gc_trace_children();
   }

void gc_forward_environment_children(Environment* env)
   {
   env->gc_forward_children();
   }
