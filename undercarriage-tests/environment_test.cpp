// environment_test.cpp -- white-box unit tests for the lexical-addressing
// scope classes in src/Environment.h / environment.cpp.
//
// Scheme is not involved: these drive Environment / LocalEnvironment /
// GlobalEnvironment directly with LA keys, the way the Evaluator and the
// by-name callers will after integration.  The differential test builds
// random scope chains both ways and checks that resolve + lookup on the new
// classes answers exactly as a by-name walk does; the walk is a small
// reference model (RefScope) of today's Environment::lookup_id / set_id.
//
// Cross-port partner: 3PyScheme/undercarriage-tests/environment_test.py
// (same test names).  Modeled on rat_test.cpp (same tiny TEST/CHECK framework).

#include "Environment.h"
#include "AST.h"
#include "gc.h"
#include "gc_test_api.h"
#include "Interpreter.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// ── Tiny test framework (shared shape with rat_test.cpp) ──────────────────────

struct TestCase
   {
   const char* name;
   void (*fn)();
   };

static std::vector<TestCase>& tests()
   {
   static std::vector<TestCase> v;
   return v;
   }

static int g_failures = 0;
static const char* g_current_test = nullptr;

struct TestRegistrar
   {
   TestRegistrar(const char* name, void (*fn)()) { tests().push_back({name, fn}); }
   };

#define TEST(NAME)                                \
   static void NAME();                            \
   static TestRegistrar _reg_##NAME(#NAME, NAME); \
   static void NAME()

#define CHECK(COND)                                               \
   do                                                             \
      {                                                           \
      if (!(COND))                                                \
         {                                                        \
         std::fprintf(stderr, "  FAIL [%s] %s:%d: %s\n",          \
                      g_current_test, __FILE__, __LINE__, #COND); \
         ++g_failures;                                            \
         }                                                        \
      } while (0)

// EXPR must throw EXC whose what() is exactly MESSAGE.
#define CHECK_THROWS(EXC, MESSAGE, EXPR)                                         \
   do                                                                            \
      {                                                                          \
      try                                                                        \
         {                                                                       \
         (void)(EXPR);                                                           \
         std::fprintf(stderr, "  FAIL [%s] %s:%d: nothing thrown by %s\n",      \
                      g_current_test, __FILE__, __LINE__, #EXPR);                \
         ++g_failures;                                                           \
         }                                                                       \
      catch (const EXC& _e)                                                      \
         {                                                                       \
         if (std::string(_e.what()) != (MESSAGE))                                \
            {                                                                    \
            std::fprintf(stderr, "  FAIL [%s] %s:%d: message \"%s\" want \"%s\"\n", \
                         g_current_test, __FILE__, __LINE__, _e.what(), MESSAGE); \
            ++g_failures;                                                        \
            }                                                                    \
         }                                                                       \
      catch (const std::exception& _e)                                           \
         {                                                                       \
         std::fprintf(stderr, "  FAIL [%s] %s:%d: wrong exception \"%s\"\n",    \
                      g_current_test, __FILE__, __LINE__, _e.what());            \
         ++g_failures;                                                           \
         }                                                                       \
      } while (0)

// ── Helpers ───────────────────────────────────────────────────────────────────

static uint32_t S(const char* name)
   {
   return intern_symbol(name);
   }

static LAKey G(const char* name)
   {
   return global_la_key(S(name));
   }

static LAKey K(int depth, int index, const char* name)
   {
   return LAKey{depth, index, S(name)};
   }

static bool same_key(const LAKey& a, const LAKey& b)
   {
   return a.depth == b.depth && a.index == b.index && a.sid == b.sid;
   }

static Value I(int64_t n)
   {
   return make_integer(n);
   }

static bool is_int(const Value& v, int64_t n)
   {
   return is_integer(v) && as_integer(v) == n;
   }

static bool is_int(const std::optional<Value>& v, int64_t n)
   {
   return v && is_int(*v, n);
   }

static ScopeNames names_of(std::vector<uint32_t> sids)
   {
   return std::make_shared<const std::vector<uint32_t>>(std::move(sids));
   }

static LocalEnvironment* local(Environment* parent, std::vector<uint32_t> sids,
                               std::vector<Value> values)
   {
   return gc_alloc_local_environment(parent, names_of(std::move(sids)),
                                         std::move(values));
   }

static GlobalEnvironment* global(Environment* parent = nullptr)
   {
   return gc_alloc_global_environment(parent);
   }

using Seen = std::vector<std::pair<uint32_t, Value>>;

static Seen bindings_of(const Environment* e)
   {
   Seen seen;
   e->for_each_binding([&](uint32_t sid, const Value& v) { seen.push_back({sid, v}); });
   return seen;
   }

// ── GlobalEnvironment ─────────────────────────────────────────────────────────

TEST(global_bind_and_lookup)
   {
   GlobalEnvironment* g = global();
   CHECK(is_int(g->bind(G("x"), I(10)), 10)); // bind returns the value
   CHECK(is_int(g->lookup(G("x")), 10));
   g->bind(G("x"), I(11));
   CHECK(is_int(g->lookup(G("x")), 11)); // bind overwrites
   }

TEST(global_ignores_depth_and_index)
   {
   GlobalEnvironment* g = global();
   g->bind(G("x"), I(10));
   CHECK(is_int(g->lookup(K(5, 7, "x")), 10)); // a global scope reads only the name
   }

TEST(global_unbound_lookup_raises)
   {
   GlobalEnvironment* g = global();
   CHECK_THROWS(SchemeUnboundError, "unbound variable: nosuch", g->lookup(G("nosuch")));
   }

TEST(global_lookup_optional_unbound_is_none)
   {
   GlobalEnvironment* g = global();
   CHECK(!g->lookup_optional(G("nosuch")));
   g->bind(G("x"), I(0));
   CHECK(is_int(g->lookup_optional(G("x")), 0)); // a zero value is not "unbound"
   }

TEST(global_set)
   {
   GlobalEnvironment* g = global();
   g->bind(G("x"), I(1));
   CHECK(is_int(g->set(G("x"), I(2)), 2)); // set returns the value
   CHECK(is_int(g->lookup(G("x")), 2));
   CHECK_THROWS(SchemeUnboundError, "set! on unbound variable: nosuch",
                g->set(G("nosuch"), I(0)));
   }

TEST(global_unassigned_lookup_raises)
   {
   GlobalEnvironment* g = global();
   g->bind(G("u"), UNASSIGNED_VALUE);
   CHECK_THROWS(SchemeUnboundError, "variable used before its initialization: u",
                g->lookup(G("u")));
   }

TEST(global_child_falls_through_to_parent)
   {
   GlobalEnvironment* root = global();
   root->bind(G("x"), I(1));
   GlobalEnvironment* child = global(root);
   CHECK(child->getParent() == root);
   CHECK(child->getGlobalEnv() == root); // a child dict scope caches the root
   CHECK(is_int(child->lookup(G("x")), 1));
   CHECK(is_int(child->lookup_optional(G("x")), 1));
   child->set(G("x"), I(2));
   CHECK(is_int(root->lookup(G("x")), 2)); // set falls through to the owner
   child->bind(G("x"), I(3));
   CHECK(is_int(child->lookup(G("x")), 3) && is_int(root->lookup(G("x")), 2)); // bind shadows
   CHECK_THROWS(SchemeUnboundError, "unbound variable: nosuch", child->lookup(G("nosuch")));
   CHECK_THROWS(SchemeUnboundError, "set! on unbound variable: nosuch",
                child->set(G("nosuch"), I(0)));
   }

TEST(global_freeze)
   {
   GlobalEnvironment* g = global();
   g->bind(G("x"), I(1));
   g->freeze();
   g->freeze(); // idempotent
   CHECK(is_int(g->lookup(G("x")), 1)); // a frozen scope still reads
   CHECK_THROWS(SchemeTypeError, "cannot define 'y' in a frozen environment",
                g->bind(G("y"), I(0)));
   CHECK_THROWS(SchemeTypeError, "set! on 'x' in a frozen environment", g->set(G("x"), I(0)));
   CHECK_THROWS(SchemeUnboundError, "set! on unbound variable: nosuch",
                g->set(G("nosuch"), I(0)));
   }

TEST(global_child_of_frozen)
   {
   GlobalEnvironment* frozen = global();
   frozen->bind(G("x"), I(1));
   frozen->freeze();
   GlobalEnvironment* child = global(frozen);
   child->bind(G("y"), I(2));
   CHECK(is_int(child->lookup(G("y")), 2)); // a child of a frozen scope can bind
   CHECK_THROWS(SchemeTypeError, "set! on 'x' in a frozen environment",
                child->set(G("x"), I(0)));
   child->bind(G("x"), I(5));
   child->set(G("x"), I(6));
   CHECK(is_int(child->lookup(G("x")), 6) && is_int(frozen->lookup(G("x")), 1));
   }

TEST(global_reroot_as_global)
   {
   GlobalEnvironment* root = global();
   GlobalEnvironment* top = global(root);
   top->reroot_as_global();
   CHECK(top->getGlobalEnv() == top);
   CHECK(top->getParent() == root); // the parent chain is kept
   LocalEnvironment* loc = local(top, {}, {});
   CHECK(loc->getGlobalEnv() == top); // a scope built later caches the new root
   }

// ── Alias cells ───────────────────────────────────────────────────────────────

TEST(alias_reads_live_target)
   {
   GlobalEnvironment* g = global();
   GlobalEnvironment* d = global();
   d->bind(G("target"), I(1));
   g->register_alias(S("gs1"), S("target"), d, I(99));
   CHECK(is_int(g->lookup(G("gs1")), 1));
   d->set(G("target"), I(2));
   CHECK(is_int(g->lookup(G("gs1")), 2)); // later mutation of the target is seen
   CHECK(is_int(g->lookup_optional(G("gs1")), 2));
   }

TEST(alias_writes_through)
   {
   GlobalEnvironment* g = global();
   GlobalEnvironment* d = global();
   d->bind(G("target"), I(1));
   g->register_alias(S("gs2"), S("target"), d, I(99));
   CHECK(is_int(g->set(G("gs2"), I(7)), 7));
   CHECK(is_int(d->lookup(G("target")), 7)); // set! through the alias reaches the target
   }

TEST(alias_falls_back_to_copy)
   {
   GlobalEnvironment* g = global();
   GlobalEnvironment* d = global(); // does not bind the target: a transient scope
   g->register_alias(S("gs3"), S("target"), d, I(99));
   CHECK(is_int(g->lookup(G("gs3")), 99)); // reads the def-time copy
   g->set(G("gs3"), I(100));
   CHECK(is_int(g->lookup(G("gs3")), 100)); // a write lands in the copy
   CHECK(!d->lookup_optional(G("target"))); // and not in def_env
   }

TEST(register_alias_binds_in_global_env)
   {
   GlobalEnvironment* root = global();
   GlobalEnvironment* child = global(root);
   GlobalEnvironment* d = global();
   d->bind(G("target"), I(1));
   child->register_alias(S("gs4"), S("target"), d, I(0));
   Seen seen = bindings_of(root);
   CHECK(seen.size() == 1 && seen[0].first == S("gs4")); // the cell lives in the root
   CHECK(seen.size() == 1 && is_alias_cell(seen[0].second)); // passed as stored
   CHECK(bindings_of(child).empty());
   }

// ── LocalEnvironment ──────────────────────────────────────────────────────────

// root (x=100) <- outer (a=1, b=2) <- inner (c=3)
struct Chain
   {
   GlobalEnvironment* root;
   LocalEnvironment* outer;
   LocalEnvironment* inner;
   };

static Chain chain()
   {
   Chain c;
   c.root = global();
   c.root->bind(G("x"), I(100));
   c.outer = local(c.root, {S("a"), S("b")}, {I(1), I(2)});
   c.inner = local(c.outer, {S("c")}, {I(3)});
   return c;
   }

TEST(local_lookup_by_depth_and_index)
   {
   Chain c = chain();
   CHECK(is_int(c.inner->lookup(K(0, 0, "c")), 3));
   CHECK(is_int(c.inner->lookup(K(1, 0, "a")), 1));
   CHECK(is_int(c.inner->lookup(K(1, 1, "b")), 2));
   CHECK(is_int(c.inner->lookup(K(1, 1, "zzz")), 2)); // a local scope ignores the name
   CHECK(is_int(c.inner->lookup_optional(K(1, 0, "a")), 1));
   }

TEST(local_global_key_forwards_to_global)
   {
   Chain c = chain();
   CHECK(c.inner->getGlobalEnv() == c.root);
   CHECK(is_int(c.inner->lookup(G("x")), 100));
   CHECK(is_int(c.inner->lookup_optional(G("x")), 100));
   CHECK(!c.inner->lookup_optional(G("nosuch")));
   CHECK_THROWS(SchemeUnboundError, "unbound variable: nosuch", c.inner->lookup(G("nosuch")));
   c.inner->set(G("x"), I(101));
   CHECK(is_int(c.root->lookup(G("x")), 101)); // a global set from a local scope
   }

TEST(local_set_writes_the_owner_only)
   {
   Chain c = chain();
   CHECK(is_int(c.inner->set(K(1, 1, "b"), I(20)), 20));
   CHECK(is_int(c.outer->lookup(K(0, 1, "b")), 20));
   CHECK(is_int(c.inner->lookup(K(0, 0, "c")), 3)); // inner slot 0 untouched
   CHECK(is_int(c.outer->lookup(K(0, 0, "a")), 1)); // outer slot 0 untouched
   }

TEST(local_unassigned)
   {
   GlobalEnvironment* root = global();
   LocalEnvironment* loc = local(root, {S("u")}, {UNASSIGNED_VALUE});
   CHECK_THROWS(SchemeUnboundError, "variable used before its initialization: u",
                loc->lookup(K(0, 0, "u")));
   // Same as today's Environment::lookup_optional_id: the marker is
   // returned as stored.
   std::optional<Value> v = loc->lookup_optional(K(0, 0, "u"));
   CHECK(v && is_unassigned(*v));
   loc->set(K(0, 0, "u"), I(5));
   CHECK(is_int(loc->lookup(K(0, 0, "u")), 5)); // letrec init clears it
   }

TEST(local_bind)
   {
   GlobalEnvironment* root = global();
   LocalEnvironment* loc = local(root, {S("p"), S("q")}, {I(1), UNASSIGNED_VALUE});
   CHECK(is_int(loc->bind(K(0, 1, "q"), I(2)), 2));
   CHECK(is_int(loc->lookup(K(0, 1, "q")), 2));
   CHECK_THROWS(SchemeTypeError, "cannot define 'newname' in a local scope",
                loc->bind(G("newname"), I(0)));
   CHECK(!root->lookup_optional(G("newname"))); // nothing leaked to the global
   }

// local_bind_asserts_depth_zero: Python only.  The C++ check is an assert(),
// which aborts in a Debug build and is compiled out in Release.

TEST(local_shared_names_independent_values)
   {
   GlobalEnvironment* root = global();
   ScopeNames names = names_of({S("n")});
   LocalEnvironment* one = gc_alloc_local_environment(root, names, {I(1)});
   LocalEnvironment* two = gc_alloc_local_environment(root, names, {I(2)});
   one->set(K(0, 0, "n"), I(10));
   CHECK(is_int(two->lookup(K(0, 0, "n")), 2)); // two calls of one lambda share names only
   }

TEST(resolve)
   {
   Chain c = chain();
   CHECK(same_key(c.inner->resolve(S("c"), 0), K(0, 0, "c")));
   CHECK(same_key(c.inner->resolve(S("a"), 0), K(1, 0, "a")));
   CHECK(same_key(c.inner->resolve(S("b"), 0), K(1, 1, "b")));
   CHECK(same_key(c.inner->resolve(S("x"), 0), K(GLOBAL_DEPTH, 0, "x"))); // a global name
   // An unbound name resolves as a global, as the Analyzer would emit.
   CHECK(same_key(c.inner->resolve(S("nosuch"), 0), K(GLOBAL_DEPTH, 0, "nosuch")));
   CHECK(same_key(c.root->resolve(S("a"), 0), G("a")));
   }

TEST(resolve_innermost_shadow_wins)
   {
   GlobalEnvironment* root = global();
   root->bind(G("v"), I(0));
   LocalEnvironment* outer = local(root, {S("v")}, {I(1)});
   LocalEnvironment* inner = local(outer, {S("w"), S("v")}, {I(9), I(2)});
   LAKey k = inner->resolve(S("v"), 0);
   CHECK(same_key(k, K(0, 1, "v")));
   CHECK(is_int(inner->lookup(k), 2));
   CHECK(is_int(outer->lookup(outer->resolve(S("v"), 0)), 1));
   }

TEST(for_each_binding)
   {
   Chain c = chain();
   Seen seen = bindings_of(c.outer);
   CHECK(seen.size() == 2); // local: in slot order
   CHECK(seen.size() == 2 && seen[0].first == S("a") && is_int(seen[0].second, 1));
   CHECK(seen.size() == 2 && seen[1].first == S("b") && is_int(seen[1].second, 2));
   seen = bindings_of(c.root);
   CHECK(seen.size() == 1 && seen[0].first == S("x") && is_int(seen[0].second, 100));
   CHECK(bindings_of(local(c.root, {}, {})).empty());
   }

// ── Reference model: a by-name walk ───────────────────────────────────────────
// Today's Environment::lookup_id / lookup_optional_id / set_id, a map per
// scope.  Scopes are owned by the vector the test builds them in.

struct RefScope
   {
   RefScope* parent;
   std::unordered_map<uint32_t, Value> b;

   RefScope* owner(uint32_t sid)
      {
      for (RefScope* s = this; s; s = s->parent)
         if (s->b.count(sid))
            return s;
      return nullptr;
      }

   Value lookup(uint32_t sid)
      {
      RefScope* s = owner(sid);
      if (!s)
         throw SchemeUnboundError("unbound variable: " + gensym_display_name(symbol_name(sid)));
      if (is_unassigned(s->b[sid]))
         throw SchemeUnboundError("variable used before its initialization: "
                                  + gensym_display_name(symbol_name(sid)));
      return s->b[sid];
      }

   std::optional<Value> lookup_optional(uint32_t sid)
      {
      RefScope* s = owner(sid);
      if (!s)
         return std::nullopt;
      return s->b[sid];
      }

   void set(uint32_t sid, Value v)
      {
      RefScope* s = owner(sid);
      if (!s)
         throw SchemeUnboundError("set! on unbound variable: "
                                  + gensym_display_name(symbol_name(sid)));
      s->b[sid] = v;
      }
   };

// ok + integer value, or the error text; unassigned compares as -1.
struct Outcome
   {
   bool ok;
   int64_t value;
   std::string err;

   bool operator==(const Outcome& o) const
      {
      return ok == o.ok && (ok ? value == o.value : err == o.err);
      }
   };

template <typename F>
static Outcome outcome(F fn)
   {
   try
      {
      return Outcome{true, as_integer(fn()), ""};
      }
   catch (const SchemeUnboundError& e)
      {
      return Outcome{false, 0, e.what()};
      }
   }

static int64_t opt_code(const std::optional<Value>& v)
   {
   if (!v)
      return -2;
   if (is_unassigned(*v))
      return -1;
   return as_integer(*v);
   }

TEST(local_under_child_dict_scope_finds_it)
   {
   // A global LA key in a local scope goes to the nearest dict scope above
   // it, not straight to the root, so a dict scope BETWEEN the local and
   // the root (make-environment, an Expander macro scope) is consulted, as
   // a by-name walk would.
   GlobalEnvironment* root = global();
   root->bind(G("r"), I(1));
   GlobalEnvironment* macro_scope = global(root);
   macro_scope->bind(G("m"), I(7));
   LocalEnvironment* outer = local(macro_scope, {S("a")}, {I(2)});
   LocalEnvironment* loc = local(outer, {}, {});
   CHECK(loc->getGlobalEnv() == root); // getGlobalEnv is still the root
   CHECK(is_int(loc->lookup(loc->resolve(S("m"), 0)), 7)); // found two scopes up
   CHECK(is_int(loc->lookup(loc->resolve(S("r"), 0)), 1)); // the root through it
   loc->set(loc->resolve(S("m"), 0), I(8));
   CHECK(is_int(macro_scope->lookup(G("m")), 8)); // set reaches it
   RefScope ref_root{nullptr, {}};
   RefScope ref_macro{&ref_root, {}};
   ref_macro.b[S("m")] = I(7);
   RefScope ref_loc{&ref_macro, {}};
   CHECK(is_int(ref_loc.lookup_optional(S("m")), 7)); // a by-name walk agrees
   }

TEST(make_environment_shape)
   {
   // (define e (make-environment)) (eval '(define z 1) e)
   // (eval '((lambda () z)) e)  -- the lambda's call scope is under e.
   GlobalEnvironment* root = global();
   GlobalEnvironment* e = global(root);
   e->bind(G("z"), I(1));
   LocalEnvironment* call = local(e, {}, {});
   CHECK(is_int(call->lookup(call->resolve(S("z"), 0)), 1));
   CHECK(!root->lookup_optional(G("z"))); // z stayed in e
   }

TEST(differential_against_by_name_walk)
   {
   std::mt19937 rng(20261028);
   auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
   std::vector<uint32_t> pool;
   for (int i = 0; i < 12; ++i)
      pool.push_back(S(("d" + std::to_string(i)).c_str()));
   auto sample = [&](int n)
      {
      std::vector<uint32_t> p = pool;
      std::shuffle(p.begin(), p.end(), rng);
      p.resize(n);
      return p;
      };

   for (int trial = 0; trial < 2000; ++trial)
      {
      std::vector<std::unique_ptr<RefScope>> owned;
      owned.push_back(std::make_unique<RefScope>(RefScope{nullptr, {}}));
      RefScope* old = owned.back().get();
      Environment* now = global();
      for (uint32_t sid : sample(pick(0, 6)))
         {
         Value v = I(pick(0, 999));
         old->b[sid] = v;
         now->bind(global_la_key(sid), v);
         }
      if (pick(0, 1))
         {
         // a dict scope under the root (make-environment, a macro scope)
         owned.push_back(std::make_unique<RefScope>(RefScope{old, {}}));
         old = owned.back().get();
         now = global(now);
         for (uint32_t sid : sample(pick(0, 4)))
            {
            Value v = I(pick(0, 999));
            old->b[sid] = v;
            now->bind(global_la_key(sid), v);
            }
         }
      int depth = pick(0, 5);
      for (int level = 0; level < depth; ++level)
         {
         std::vector<uint32_t> names = sample(pick(0, 4));
         std::vector<Value> values;
         for (size_t i = 0; i < names.size(); ++i)
            values.push_back(pick(0, 1) ? I(pick(0, 999)) : UNASSIGNED_VALUE);
         owned.push_back(std::make_unique<RefScope>(RefScope{old, {}}));
         old = owned.back().get();
         for (size_t i = 0; i < names.size(); ++i)
            old->b[names[i]] = values[i];
         now = local(now, names, values);
         }
      for (uint32_t sid : pool)
         {
         LAKey k = now->resolve(sid, 0);
         Outcome o = outcome([&] { return old->lookup(sid); });
         Outcome n = outcome([&] { return now->lookup(k); });
         CHECK(o == n);
         CHECK(opt_code(old->lookup_optional(sid)) == opt_code(now->lookup_optional(k)));
         if (o.ok || o.err.rfind("variable used", 0) == 0)
            {
            Value w = I(pick(1000, 1999));
            old->set(sid, w);
            now->set(k, w);
            CHECK(as_integer(old->lookup(sid)) == as_integer(w));
            CHECK(as_integer(now->lookup(k)) == as_integer(w));
            }
         }
      }
   }

// ── GC: write barrier and survival ────────────────────────────────────────────
// C++ only (Python has no collector of its own).  Sibling of gc_test.cpp's
// write_barrier_via_env_bind, once per subclass and per writer.

static Value make_test_cons()
   {
   ConsCell* c = gc_alloc_cons();
   c->car = Value{};
   c->cdr = Value{};
   c->src = nullptr;
   return Value{Value::Repr(c)};
   }

// Promote env to the old generation, store a fresh nursery value with
// store(), and check the barrier remembered env and the value survives.
template <typename Store, typename Load>
static void check_barrier(Environment* env, Store store, Load load)
   {
   gc_env_root_push(&env);
   gc_test_force_minor();
   CHECK((int)env->header.gen == 1);
   Value fresh = make_test_cons();
   GcHeader* fresh_hdr = gc_value_header(fresh);
   store(env, fresh);
   CHECK((int)fresh_hdr->gen == 0);
   CHECK(gc_test_in_remembered_set(&env->header));
   gc_test_force_minor();
   GcHeader* survived = gc_value_header(load(env));
   CHECK(gc_test_in_heap(survived));
   CHECK((int)survived->gen == 1);
   gc_env_root_pop(&env);
   }

TEST(gc_barrier_global_bind_and_set)
   {
   gc_test_reset();
   check_barrier(global(),
                 [](Environment* e, Value v) { e->bind(G("x"), v); },
                 [](Environment* e) { return e->lookup(G("x")); });
   gc_test_reset();
   GlobalEnvironment* g = global();
   g->bind(G("y"), I(0));
   check_barrier(g,
                 [](Environment* e, Value v) { e->set(G("y"), v); },
                 [](Environment* e) { return e->lookup(G("y")); });
   }

TEST(gc_barrier_local_bind_and_set)
   {
   gc_test_reset();
   Environment* root = global();
   gc_env_root_push(&root);
   check_barrier(local(root, {S("p")}, {UNASSIGNED_VALUE}),
                 [](Environment* e, Value v) { e->bind(K(0, 0, "p"), v); },
                 [](Environment* e) { return e->lookup(K(0, 0, "p")); });
   check_barrier(local(root, {S("q")}, {I(0)}),
                 [](Environment* e, Value v) { e->set(K(0, 0, "q"), v); },
                 [](Environment* e) { return e->lookup(K(0, 0, "q")); });
   gc_env_root_pop(&root);
   }

TEST(gc_set_at_depth_barriers_the_owner)
   {
   // set() at depth 1 stores into the OUTER scope; the barrier must
   // remember that scope, not the one set() was called on.
   gc_test_reset();
   GlobalEnvironment* root = global();
   Environment* outer = local(root, {S("a")}, {I(0)});
   Environment* inner = local(outer, {}, {});
   gc_env_root_push(&inner);
   gc_test_force_minor();
   outer = inner->getParent(); // the minor GC may have moved it
   CHECK((int)outer->header.gen == 1);
   Value fresh = make_test_cons();
   inner->set(K(1, 0, "a"), fresh);
   CHECK(gc_test_in_remembered_set(&outer->header));
   gc_test_force_minor();
   CHECK(gc_test_in_heap(gc_value_header(inner->lookup(K(1, 0, "a")))));
   gc_env_root_pop(&inner);
   }

TEST(gc_chain_survives_major)
   {
   // Rooting only the innermost scope keeps the whole chain (parents,
   // global env, slot values, dict values) alive through a major GC.
   gc_test_reset();
   GlobalEnvironment* root = global();
   root->bind(G("gv"), make_test_cons());
   Environment* outer = local(root, {S("a")}, {make_test_cons()});
   Environment* inner = local(outer, {S("c")}, {make_test_cons()});
   gc_env_root_push(&inner);
   gc_test_force_major();
   CHECK(gc_test_in_heap(gc_value_header(inner->lookup(K(0, 0, "c")))));
   CHECK(gc_test_in_heap(gc_value_header(inner->lookup(K(1, 0, "a")))));
   CHECK(gc_test_in_heap(gc_value_header(inner->lookup(G("gv")))));
   gc_env_root_pop(&inner);
   }

int main(int argc, char** argv)
   {
   const char* filter = nullptr;
   for (int i = 1; i < argc; ++i)
      if (std::strncmp(argv[i], "--filter=", 9) == 0)
         filter = argv[i] + 9;

   // One interpreter ensures every subsystem (symbol pool, GC, object pools)
   // is fully initialized, as in normal runs.  The non-GC tests leave their
   // scopes unrooted, so no incidental collection may run during them.
   Interpreter interp;
   gc_test_set_young_threshold(1000000);

   std::printf("Running %zu environment tests%s%s...\n", tests().size(),
               filter ? " filtered by " : "", filter ? filter : "");
   for (const auto& tc : tests())
      {
      if (filter && !std::strstr(tc.name, filter))
         continue;
      g_current_test = tc.name;
      int before = g_failures;
      try
         {
         tc.fn();
         }
      catch (const std::exception& e)
         {
         std::fprintf(stderr, "  FAIL [%s] uncaught: %s\n", tc.name, e.what());
         ++g_failures;
         }
      std::printf("  [%s] %s\n", (g_failures == before) ? "ok" : "FAIL", tc.name);
      }
   std::printf("\n%d failure(s)\n", g_failures);
   return g_failures == 0 ? 0 : 1;
   }
