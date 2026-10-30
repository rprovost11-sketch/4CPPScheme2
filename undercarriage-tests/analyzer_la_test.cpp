// analyzer_la_test.cpp -- white-box unit tests for the Analyzer's lexical
// addressing (src/Analyzer.cpp, "Lexical addressing").
//
// Each test analyzes a small program and checks the LA address -- (depth,
// index), or GLOBAL -- that every variable reference carries, in tree order.
// The addresses must match the scopes the Evaluator builds at run time; that
// is proved end to end by running the battery under SCHEME_LA_CHECK=1 (see
// environment.cpp, "LA check").  These tests pin the frame model one form at
// a time, and check the rewrite's other promises: the input tree is never
// edited, unchanged subtrees are reused, a shared subtree gets each scope's
// own keys, re-analysis is stable, scope_of_env reads a live chain, and
// printers do not show the scope-names node.
//
// Cross-port partner: 3PyScheme/undercarriage-tests/analyzer_la_test.py
// (same test names).  Same tiny TEST/CHECK framework as environment_test.cpp.

#include "Analyzer.h"
#include "AST.h"
#include "Environment.h"
#include "Interpreter.h"
#include "PrettyPrinter.h"
#include "expander.h"
#include "gc.h"
#include "gc_test_api.h"
#include "parser.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// ── Tiny test framework (shared shape with environment_test.cpp) ──────────────

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

// ── Helpers ───────────────────────────────────────────────────────────────────

// One resolved reference: its display name and address, "G" for a global or
// "d,i" for a local.
using Address = std::pair<std::string, std::string>;
using Addresses = std::vector<Address>;

static const std::string G = "G";

static std::string L(int depth, int index)
   {
   return std::to_string(depth) + "," + std::to_string(index);
   }

static void walk(const Value& x, Addresses& out)
   {
   if (is_cons(x))
      {
      walk(car(x), out);
      walk(cdr(x), out);
      return;
      }
   int depth, index;
   if (is_symbol(x) && symbol_la_address(x, depth, index))
      out.push_back({gensym_display_name(as_symbol(x)),
                     depth == GLOBAL_DEPTH ? G : L(depth, index)});
   }

// Every resolved symbol in tree, in tree order.
static Addresses addresses(const Value& tree)
   {
   Addresses out;
   walk(tree, out);
   return out;
   }

static Value analyze_src(const std::string& src)
   {
   return analyze(expand(scheme_parse_one(src)));
   }

static std::string show(const Addresses& a)
   {
   std::string s = "[";
   for (size_t i = 0; i < a.size(); ++i)
      s += (i ? " " : "") + a[i].first + "@" + a[i].second;
   return s + "]";
   }

#define CHECK_ADDRESSES(SRC, ...)                                                  \
   do                                                                              \
      {                                                                            \
      Addresses got = addresses(analyze_src(SRC));                                 \
      Addresses want = __VA_ARGS__;                                                \
      if (got != want)                                                             \
         {                                                                         \
         std::fprintf(stderr, "  FAIL [%s] %s:%d: %s\n      got      %s\n"        \
                              "      expected %s\n",                               \
                      g_current_test, __FILE__, __LINE__, SRC, show(got).c_str(), \
                      show(want).c_str());                                         \
         ++g_failures;                                                             \
         }                                                                         \
      } while (0)

// The scope-names node of an analyzed let-family form, as names, or "-" if
// there is none.  Scopes are separated by '|', names by ' '.
static std::string scope_names_of(const Value& form)
   {
   Value node = car(cdr(form));
   if (!is_scope_names(node))
      return "-";
   std::string s;
   bool first_scope = true;
   for (const ScopeNames& scope : as_scope_names(node))
      {
      if (!first_scope)
         s += "|";
      first_scope = false;
      for (size_t i = 0; i < scope->size(); ++i)
         s += (i ? " " : "") + gensym_display_name(as_symbol(make_symbol_id((*scope)[i])));
      }
   return s;
   }

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST(top_level_reference_is_global)
   {
   CHECK_ADDRESSES("(car x)", {{"car", G}, {"x", G}});
   }

TEST(lambda_params_then_rest)
   {
   CHECK_ADDRESSES("(lambda (a b . r) (list r b a))",
                   {{"list", G}, {"r", L(0, 2)}, {"b", L(0, 1)}, {"a", L(0, 0)}});
   CHECK_ADDRESSES("(lambda args args)", {{"args", L(0, 0)}});
   }

TEST(nested_lambda_counts_depth)
   {
   CHECK_ADDRESSES("(lambda (a) (lambda (b) (lambda () (a b))))",
                   {{"a", L(2, 0)}, {"b", L(1, 0)}});
   }

TEST(case_lambda_clause_is_one_scope)
   {
   CHECK_ADDRESSES("(case-lambda ((x) x) ((x y . z) z))", {{"x", L(0, 0)}, {"z", L(0, 2)}});
   }

TEST(let_inits_outside_body_inside)
   {
   CHECK_ADDRESSES("(lambda (x) (let ((y x) (z 1)) (list x y z)))",
                   {{"x", L(0, 0)},
                    {"list", G}, {"x", L(1, 0)}, {"y", L(0, 0)}, {"z", L(0, 1)}});
   }

TEST(empty_let_builds_no_scope)
   {
   CHECK_ADDRESSES("(lambda (x) (let () x))", {{"x", L(0, 0)}});
   CHECK_ADDRESSES("(lambda (x) (let* () x))", {{"x", L(0, 0)}});
   CHECK_ADDRESSES("(lambda (x) (letrec () x))", {{"x", L(0, 0)}});
   }

TEST(let_star_one_scope_per_binding)
   {
   CHECK_ADDRESSES("(lambda (x) (let* ((a x) (b a) (c b)) (list a b c x)))",
                   {{"x", L(0, 0)}, {"a", L(0, 0)}, {"b", L(0, 0)},
                    {"list", G}, {"a", L(2, 0)}, {"b", L(1, 0)}, {"c", L(0, 0)},
                    {"x", L(3, 0)}});
   }

TEST(letrec_inits_inside)
   {
   CHECK_ADDRESSES("(letrec ((f (lambda () g)) (g 1)) (f))", {{"g", L(1, 1)}, {"f", L(0, 0)}});
   }

TEST(named_let_inits_run_in_the_loop_scope)
   {
   // The Evaluator runs the inits inside the scope that holds the loop name,
   // so n is one scope further out.  They still cannot see the loop name
   // (R7RS 4.2.4): the Expander renames the binding, so the init's `loop` is
   // another identifier and resolves outside -- here, to a global.
   CHECK_ADDRESSES("(lambda (n) (let loop ((i n) (acc loop)) (loop i acc)))",
                   {{"n", L(1, 0)}, {"loop", G},
                    {"loop", L(1, 0)}, {"i", L(0, 0)}, {"acc", L(0, 1)}});
   }

TEST(delay_runs_in_an_empty_scope)
   {
   CHECK_ADDRESSES("(lambda (x) (delay x))", {{"x", L(1, 0)}});
   CHECK_ADDRESSES("(lambda (x) (delay-force x))", {{"x", L(1, 0)}});
   }

TEST(set_target_is_resolved)
   {
   CHECK_ADDRESSES("(lambda (x) (set! x 1))", {{"x", L(0, 0)}});
   CHECK_ADDRESSES("(lambda (x) (set! y x))", {{"y", G}, {"x", L(0, 0)}});
   }

TEST(define_name_stays_plain)
   {
   Value tree = analyze_src("(define f (lambda (x) x))");
   int depth, index;
   CHECK(!symbol_la_address(car(cdr(tree)), depth, index));
   CHECK(addresses(tree) == Addresses({{"x", L(0, 0)}}));
   }

TEST(quote_and_case_datums_stay_plain)
   {
   CHECK_ADDRESSES("(lambda (x) '(x y))", {});
   CHECK_ADDRESSES("(lambda (x) (case x ((x) x) (else x)))",
                   {{"x", L(0, 0)}, {"x", L(0, 0)}, {"x", L(0, 0)}});
   }

TEST(cond_arrow_and_else)
   {
   CHECK_ADDRESSES("(lambda (x f) (cond (x => f) (else x)))",
                   {{"x", L(0, 0)}, {"f", L(0, 1)}, {"x", L(0, 0)}});
   }

TEST(scope_names_node_holds_the_scopes)
   {
   CHECK(scope_names_of(analyze_src("(let ((a 1) (b 2)) a)")) == "a b");
   CHECK(scope_names_of(analyze_src("(let* ((a 1) (b 2)) a)")) == "a|b");
   CHECK(scope_names_of(analyze_src("(letrec ((a 1)) a)")) == "a");
   CHECK(scope_names_of(analyze_src("(let lp ((a 1)) a)")) == "lp|a");
   CHECK(scope_names_of(analyze_src("(let () 1)")) == "");
   }

TEST(input_tree_is_not_edited)
   {
   Value tree = expand(scheme_parse_one("(lambda (x) (let ((y x)) (if y (quote (a b)) x)))"));
   std::string before = scheme_pretty_print(tree);
   analyze(tree);
   CHECK(scheme_pretty_print(tree) == before);
   CHECK(addresses(tree).empty());
   }

TEST(unchanged_subtree_is_reused)
   {
   Value tree = expand(scheme_parse_one("(lambda (x) (quote (a b)) x)"));
   Value out = analyze(tree);
   CHECK(value_identical(car(cdr(cdr(out))), car(cdr(cdr(tree)))));
   CHECK(!value_identical(out, tree));
   // A cons owns its SourceInfo (alloc_cons clones it), so compare positions.
   CHECK(src_of(out) != nullptr && src_of(tree) != nullptr &&
         src_of(out)->line == src_of(tree)->line && src_of(out)->col == src_of(tree)->col);
   }

TEST(shared_subtree_gets_each_scopes_keys)
   {
   // One cons (x), placed by hand in two lambdas whose x sits in different
   // slots -- the way macro expansion can share a subtree.
   Value shared = alloc_cons(make_symbol("x"), NIL_VALUE);
   Value lam1 = scheme_parse_one("(lambda (x) 0)");
   Value lam2 = scheme_parse_one("(lambda (y x) 0)");
   Value cdr1 = cdr(lam1);
   Value cdr2 = cdr(lam2);
   set_cdr(cdr1, alloc_cons(shared, NIL_VALUE));
   set_cdr(cdr2, alloc_cons(shared, NIL_VALUE));
   Value prog = alloc_cons(make_symbol("list"),
                           alloc_cons(lam1, alloc_cons(lam2, NIL_VALUE)));
   Addresses got = addresses(analyze(prog));
   CHECK(got == Addresses({{"list", G}, {"x", L(0, 0)}, {"x", L(0, 1)}}));
   int depth, index;
   CHECK(!symbol_la_address(car(shared), depth, index));
   }

TEST(reanalysis_is_stable)
   {
   Value once = analyze_src(
       "(lambda (n) (let loop ((i n)) (let* ((a i)) (letrec ((b a)) (loop b)))))");
   Value twice = analyze(once);
   CHECK(addresses(twice) == addresses(once));
   CHECK(scheme_pretty_print(twice) == scheme_pretty_print(once));
   }

TEST(scope_of_env_reads_the_live_chain)
   {
   GlobalEnvironment* g = gc_alloc_global_environment();
   uint32_t a = intern_symbol("a"), b = intern_symbol("b"), c = intern_symbol("c");
   LocalEnvironment* outer = gc_alloc_local_environment(
       g, std::make_shared<const std::vector<uint32_t>>(std::vector<uint32_t>{a, b}),
       {VOID_VALUE, VOID_VALUE});
   LocalEnvironment* inner = gc_alloc_local_environment(
       outer, std::make_shared<const std::vector<uint32_t>>(std::vector<uint32_t>{c}),
       {VOID_VALUE});
   std::vector<CompileScope> links;
   CHECK(scope_of_env(g, links) == nullptr);
   const CompileScope* scope = scope_of_env(inner, links);
   CHECK(scope != nullptr && *scope->names == std::vector<uint32_t>{c});
   CHECK(scope != nullptr && scope->outer != nullptr &&
         *scope->outer->names == (std::vector<uint32_t>{a, b}) &&
         scope->outer->outer == nullptr);
   Addresses got = addresses(
       analyze(expand(scheme_parse_one("(list a b c d)")), primitive_arities(), scope));
   CHECK(got == Addresses({{"list", G}, {"a", L(1, 0)}, {"b", L(1, 1)}, {"c", L(0, 0)},
                           {"d", G}}));
   }

TEST(printers_skip_the_scope_names_node)
   {
   for (const char* src : {"(let ((a 1)) a)", "(let* ((a 1) (b a)) b)",
                           "(letrec ((a 1)) a)", "(let lp ((a 1)) a)", "(let () 1)"})
      {
      Value tree = expand(scheme_parse_one(src));
      if (scheme_pretty_print(analyze(tree)) != scheme_pretty_print(tree))
         {
         std::fprintf(stderr, "  FAIL [%s] %s:%d: %s\n", g_current_test, __FILE__, __LINE__,
                      src);
         ++g_failures;
         }
      }
   }

int main(int argc, char** argv)
   {
   const char* filter = nullptr;
   for (int i = 1; i < argc; ++i)
      if (std::strncmp(argv[i], "--filter=", 9) == 0)
         filter = argv[i] + 9;

   // One interpreter ensures every subsystem (symbol pool, GC, macro tables) is
   // fully initialized, as in normal runs.  The trees here are unrooted, so no
   // incidental collection may run during the tests.
   Interpreter interp;
   gc_test_set_young_threshold(1000000);

   std::printf("Running %zu analyzer LA tests%s%s...\n", tests().size(),
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
