// Analyzer.cpp -- semantic validator and lexical addresser.
// Direct port of pyscheme/Analyzer.py.
#include "Analyzer.h"
#include "Parser.h"   // SchemeSyntaxError
#include "expander.h" // lookup_macro
#include <algorithm>
#include <cctype>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

// R7RS §3.1: syntactic keywords may not be used as expressions.
// Mirrors pyscheme's _SYNTACTIC_KEYWORDS frozenset in Evaluator.py.
static const std::unordered_set<std::string> SYNTACTIC_KEYWORDS = {
    "and",
    "begin",
    "case",
    "case-lambda",
    "cond",
    "cond-expand",
    "define",
    "define-library",
    "define-record-type",
    "define-syntax",
    "define-values",
    "delay",
    "delay-force",
    "do",
    "guard",
    "if",
    "import",
    "include",
    "include-ci",
    "lambda",
    "let",
    "let*",
    "let*-values",
    "let-syntax",
    "let-values",
    "letrec",
    "letrec*",
    "letrec-syntax",
    "or",
    "parameterize",
    "quasiquote",
    "quote",
    "set!",
    "syntax-rules",
    "unless",
    "when",
};

// ── Primitive arities registry ────────────────────────────────────────────────

static StaticEnv g_primitive_arities;
static std::mutex g_prim_mutex;

void register_primitive_arity(const std::string& name, int lo, int hi)
   {
   std::lock_guard<std::mutex> lk(g_prim_mutex);
   g_primitive_arities[name] = std::make_pair(lo, hi);
   }

const StaticEnv& primitive_arities()
   {
   return g_primitive_arities;
   }

// ── Special-form dispatch ─────────────────────────────────────────────────────

using AnalysisHandler =
    std::function<Value(const Value&, const StaticEnv&, const CompileScope*)>;
static std::once_flag s_init_flag;
static std::unordered_map<std::string, AnalysisHandler> s_special_forms;

// Forward declarations for all special-form handlers
static Value analyze_lambda(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope);
static Value analyze_case_lambda(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope);
static Value analyze_define(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope);
static Value analyze_set(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope);
static Value analyze_if(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope);
static Value analyze_begin(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope);
static Value analyze_let(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope);
static Value analyze_let_star(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope);
static Value analyze_letrec(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope);
static Value analyze_letrec_star(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope);
static Value analyze_cond(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope);
static Value analyze_case(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope);
static Value analyze_do(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope);
static Value analyze_include(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope);
static Value analyze_cond_expand(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope);
static Value analyze_and(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope);
static Value analyze_or(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope);
static Value analyze_when(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope);
static Value analyze_unless(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope);
static Value analyze_quote(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope);
static Value analyze_delay(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope);
static Value analyze_library_form(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope);
static Value analyze_unquote_outside(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope);

static void init_special_forms()
   {
   s_special_forms["lambda"] = analyze_lambda;
   s_special_forms["case-lambda"] = analyze_case_lambda;
   s_special_forms["define"] = analyze_define;
   s_special_forms["set!"] = analyze_set;
   s_special_forms["if"] = analyze_if;
   s_special_forms["begin"] = analyze_begin;
   s_special_forms["let"] = analyze_let;
   s_special_forms["let*"] = analyze_let_star;
   s_special_forms["letrec"] = analyze_letrec;
   s_special_forms["letrec*"] = analyze_letrec_star;
   s_special_forms["cond"] = analyze_cond;
   s_special_forms["case"] = analyze_case;
   s_special_forms["do"] = analyze_do;
   s_special_forms["include"] = analyze_include;
   s_special_forms["include-ci"] = analyze_include;
   s_special_forms["cond-expand"] = analyze_cond_expand;
   s_special_forms["and"] = analyze_and;
   s_special_forms["or"] = analyze_or;
   s_special_forms["when"] = analyze_when;
   s_special_forms["unless"] = analyze_unless;
   s_special_forms["quote"] = analyze_quote;
   s_special_forms["unquote"] = analyze_unquote_outside;
   s_special_forms["unquote-splicing"] = analyze_unquote_outside;
   s_special_forms["delay"] = analyze_delay;
   s_special_forms["delay-force"] = analyze_delay;
   s_special_forms["define-library"] = analyze_library_form;
   s_special_forms["import"] = analyze_library_form;
   s_special_forms["export"] = analyze_library_form;
   s_special_forms["include-library-declarations"] = analyze_library_form;
   s_special_forms["define-syntax"] = analyze_library_form;
   s_special_forms["let-syntax"] = analyze_library_form;
   s_special_forms["letrec-syntax"] = analyze_library_form;
   s_special_forms["syntax-rules"] = analyze_library_form;
   }

// ── Helpers ───────────────────────────────────────────────────────────────────

static int proper_list_length(const Value& cell)
   {
   if (is_nil(cell))
      return 0;
   int n = 0;
   Value cur = cell;
   while (is_cons(cur))
      {
      n++;
      cur = cdr(cur);
      }
   return is_nil(cur) ? n : -1;
   }

static std::vector<Value> cons_to_list(const Value& cell)
   {
   std::vector<Value> items;
   Value cur = cell;
   while (is_cons(cur))
      {
      items.push_back(car(cur));
      cur = cdr(cur);
      }
   return items;
   }

static std::string display_name(const std::string& name)
   {
   // Gensym-stripped name for error messages (see AST gensym_display_name).
   return gensym_display_name(name);
   }

static std::string render(const Value& sexpr)
   {
   if (is_symbol(sexpr))
      return display_name(as_symbol(sexpr));
   if (is_nil(sexpr))
      return "()";
   if (is_cons(sexpr))
      {
      std::string result = "(";
      Value cur = sexpr;
      bool first = true;
      while (is_cons(cur))
         {
         if (is_scope_names(car(cur)))
            {
            cur = cdr(cur);
            continue;
            }
         if (!first)
            result += " ";
         result += render(car(cur));
         cur = cdr(cur);
         first = false;
         }
      if (!is_nil(cur))
         {
         result += " . ";
         result += render(cur);
         }
      return result + ")";
      }
   if (is_integer(sexpr))
      return std::to_string(as_integer(sexpr));
   if (is_real(sexpr))
      return std::to_string(as_real(sexpr));
   if (is_string(sexpr))
      return "\"" + as_string(sexpr) + "\"";
   if (is_boolean(sexpr))
      return as_boolean(sexpr) ? "#t" : "#f";
   if (is_rational(sexpr))
      return mpz_to_string(as_rational_num(sexpr)) + "/" +
             mpz_to_string(as_rational_den(sexpr));
   return "<value>";
   }

static StaticEnv shadow_env(const StaticEnv& env, const std::vector<std::string>& names)
   {
   StaticEnv result = env;
   for (const std::string& n : names)
      result[n] = std::nullopt;
   return result;
   }

static std::string require_symbol(const Value& sexpr, const std::string& ctx)
   {
   if (!is_symbol(sexpr))
      throw SchemeAnalysisError(
          "expected an identifier in " + ctx + ", got " + render(sexpr),
          src_of(sexpr));
   return as_symbol(sexpr);
   }

// ── Static arity helpers ──────────────────────────────────────────────────────

static std::pair<int, int> lambda_arity_from_cons(const Value& lam_cons)
   {
   Value params = car(cdr(lam_cons));
   if (is_symbol(params))
      return {0, -1};
   if (is_nil(params))
      return {0, 0};
   int n = 0;
   Value cur = params;
   while (is_cons(cur))
      {
      n++;
      cur = cdr(cur);
      }
   return is_nil(cur) ? std::make_pair(n, n) : std::make_pair(n, -1);
   }

static std::optional<std::pair<int, int>> peek_lambda_arity(const Value& sexpr)
   {
   if (!is_cons(sexpr))
      return std::nullopt;
   if (!is_symbol(car(sexpr)) || as_symbol(car(sexpr)) != "lambda")
      return std::nullopt;
   if (!is_cons(cdr(sexpr)))
      return std::nullopt;
   if (proper_list_length(sexpr) < 3)
      return std::nullopt;
   return lambda_arity_from_cons(sexpr);
   }

struct AppArity
   {
   std::string name;
   std::pair<int, int> arity;
   };

static std::optional<AppArity> app_operator_arity(const Value& fn, const StaticEnv& senv)
   {
   if (is_cons(fn) && is_symbol(car(fn)) && as_symbol(car(fn)) == "lambda")
      {
      auto a = peek_lambda_arity(fn);
      if (!a)
         return std::nullopt;
      return AppArity{"", *a};
      }
   if (is_symbol(fn))
      {
      std::string name = as_symbol(fn);
      auto it = senv.find(name);
      if (it == senv.end() || !it->second)
         return std::nullopt;
      return AppArity{name, *it->second};
      }
   return std::nullopt;
   }

static void check_app_arity(const Value& fn, const std::vector<Value>& args,
                            const StaticEnv& senv, const Value& app)
   {
   // Statically enforce arity ONLY for syntactic keywords -- special forms /
   // macros (e.g. define-record-type) that reach application analysis without a
   // dedicated handler.  Those have no runtime apply step, so a wrong argument
   // count is a syntax error and must be caught here (it is how, for instance,
   // the R6RS `(define-record-type name (fields ...))` shape is rejected).
   //
   // For ordinary PROCEDURE calls (primitives, intrinsics, closures) arity is
   // left to the evaluator's apply path, which raises an identical but
   // *catchable* Scheme condition (same message text).  R7RS treats a
   // procedure-call arity mismatch as a runtime error, so (test-error (apply +))
   // and (guard (e (#t ...)) (car)) must be able to catch it -- they could not
   // when this threw during analysis, before any runtime handler is installed.
   if (!(is_symbol(fn) && SYNTACTIC_KEYWORDS.count(as_symbol(fn))))
      return;
   auto info = app_operator_arity(fn, senv);
   if (!info)
      return;
   int lo = info->arity.first;
   int hi = info->arity.second;
   int n = (int)args.size();
   if (n < lo || (hi >= 0 && n > hi))
      throw SchemeArityError(arity_mismatch_msg(info->name, lo, hi, n), src_of(app));
   }

// ── extend_static_env_with_define ─────────────────────────────────────────────

void extend_static_env_with_define(StaticEnv& senv, const Value& sexpr)
   {
   if (!is_cons(sexpr))
      return;
   if (!is_symbol(car(sexpr)) || as_symbol(car(sexpr)) != "define")
      return;
   if (!is_cons(cdr(sexpr)) || !is_cons(cdr(cdr(sexpr))))
      return;
   Value name_sexpr = car(cdr(sexpr));
   if (!is_symbol(name_sexpr))
      return;
   std::string name = as_symbol(name_sexpr);
   Value value = car(cdr(cdr(sexpr)));
   if (is_cons(value) && is_symbol(car(value)) && as_symbol(car(value)) == "lambda")
      {
      auto a = peek_lambda_arity(value);
      if (a)
         {
         senv[name] = a;
         return;
         }
      senv.erase(name);
      return;
      }
   if (is_cons(value) && is_symbol(car(value)) && as_symbol(car(value)) == "case-lambda")
      {
      senv[name] = std::make_pair(0, -1);
      return;
      }
   if (is_symbol(value))
      {
      auto it = senv.find(as_symbol(value));
      if (it != senv.end() && it->second)
         {
         senv[name] = it->second;
         return;
         }
      senv.erase(name);
      return;
      }
   senv.erase(name);
   }

// ── Lexical addressing ────────────────────────────────────────────────────────
// Port of Analyzer.py's lexical-addressing section; see the table there of
// the scopes the Evaluator builds, which these handlers push in step.  A
// scope chain is nullptr when the code runs in a dict scope, else one local
// scope's names and its outer chain (CompileScope, in Analyzer.h).  The
// handlers keep each CompileScope on the C++ stack for the duration of the
// recursion into the subforms that run in it.

static LAKey resolve_in(const CompileScope* scope, uint32_t sid)
   {
   int depth = 0;
   while (scope != nullptr)
      {
      const std::vector<uint32_t>& names = *scope->names;
      for (size_t i = 0; i < names.size(); i++)
         if (names[i] == sid)
            return LAKey{depth, (int)i, sid};
      scope = scope->outer;
      depth++;
      }
   return global_la_key(sid);
   }

// sym resolved against scope: the same symbol carrying its LA address.
static Value ref(const Value& sym, const CompileScope* scope)
   {
   LAKey k = resolve_in(scope, as_symbol_id(sym));
   return resolve_symbol(sym, k.depth, k.index);
   }

const CompileScope* scope_of_env(Environment* env, std::vector<CompileScope>& links)
   {
   links.clear();
   for (Environment* e = env; dynamic_cast<LocalEnvironment*>(e) != nullptr;
        e = e->getParent())
      {
      std::vector<uint32_t> names;
      e->for_each_binding([&](uint32_t sid, const Value&)
                          { names.push_back(sid); });
      links.push_back(CompileScope{
          std::make_shared<const std::vector<uint32_t>>(std::move(names)), nullptr});
      }
   // links is complete, so the addresses are stable from here on.
   for (size_t i = 0; i + 1 < links.size(); i++)
      links[i].outer = &links[i + 1];
   return links.empty() ? nullptr : &links[0];
   }

// ── Rebuilding ────────────────────────────────────────────────────────────────
// The Analyzer returns a rewritten copy of the tree, never edits it: macro
// expansion can share one subtree between two expansions in different
// scopes.  A subtree nothing changed in is reused as it is.  Analysis never
// triggers a collection, so the fresh cells need no rooting here.

// cell itself if neither field changed, else a fresh cell with its src.
static Value rebuild(const Value& cell, const Value& car_v, const Value& cdr_v)
   {
   if (value_identical(car_v, car(cell)) && value_identical(cdr_v, cdr(cell)))
      return cell;
   return alloc_cons(car_v, cdr_v, src_of(cell));
   }

// The proper list cell with its i-th element replaced by items[i].
static Value rebuild_list(const Value& cell, const std::vector<Value>& items)
   {
   std::vector<Value> cells;
   Value cur = cell;
   while (is_cons(cur))
      {
      cells.push_back(cur);
      cur = cdr(cur);
      }
   Value result = cur;
   for (size_t i = cells.size(); i-- > 0;)
      result = rebuild(cells[i], items[i], result);
   return result;
   }

// Analyze each element of a list as an expression, in order; return the
// rebuilt list.  An improper tail is kept as it is.
static Value analyze_seq(const Value& cell, const StaticEnv& senv, const CompileScope* scope)
   {
   std::vector<Value> items;
   Value cur = cell;
   while (is_cons(cur))
      {
      items.push_back(analyze(car(cur), senv, scope));
      cur = cdr(cur);
      }
   return rebuild_list(cell, items);
   }

// An already analyzed let-family form with its scope-names node dropped, so
// analyzing it again builds a fresh node.
static Value strip_scope_names(const Value& sexpr)
   {
   if (is_cons(cdr(sexpr)) && is_scope_names(car(cdr(sexpr))))
      return alloc_cons(car(sexpr), cdr(cdr(sexpr)), src_of(sexpr));
   return sexpr;
   }

// (head <scope-names> . rest) for the let-family form sexpr.
static Value with_scope_names(const Value& sexpr, std::vector<ScopeNames> scopes,
                              const Value& rest)
   {
   return alloc_cons(car(sexpr),
                     alloc_cons(make_scope_names(std::move(scopes)), rest),
                     src_of(sexpr));
   }

// The binding list with each (name init) rebuilt around inits[i].
static Value rebuild_bindings(const Value& bindings_sexpr, const std::vector<Value>& inits)
   {
   std::vector<Value> new_bindings;
   Value cur = bindings_sexpr;
   size_t i = 0;
   while (is_cons(cur))
      {
      Value b = car(cur);
      new_bindings.push_back(rebuild(b, car(b), rebuild(cdr(b), inits[i], cdr(cdr(b)))));
      cur = cdr(cur);
      i++;
      }
   return rebuild_list(bindings_sexpr, new_bindings);
   }

static ScopeNames name_sids(const std::vector<std::pair<std::string, Value>>& pairs)
   {
   std::vector<uint32_t> sids;
   sids.reserve(pairs.size());
   for (const auto& p : pairs)
      sids.push_back(intern_symbol(p.first));
   return std::make_shared<const std::vector<uint32_t>>(std::move(sids));
   }

// ── Lambda shape validator ────────────────────────────────────────────────────

// Validate the formals + body of a lambda-shaped form and return the analyzed
// body.
static Value analyze_lambda_shape(const Value& params_sexpr, const Value& body_cons,
                                  const std::string& form_name, SourceInfo* outer_src,
                                  const StaticEnv& senv, const CompileScope* scope)
   {
   struct ParamEntry
      {
      std::string name;
      uint32_t id;
      SourceInfo* src;
      };
   std::vector<ParamEntry> fixed;
   std::string rest_name;
   uint32_t rest_id = UINT32_MAX;
   SourceInfo* rest_src = nullptr;
   bool has_rest = false;

   if (is_symbol(params_sexpr))
      {
      has_rest = true;
      rest_name = as_symbol(params_sexpr);
      rest_id = as_symbol_id(params_sexpr);
      rest_src = src_of(params_sexpr);
      }
   else if (is_nil(params_sexpr))
      {
      // no params - fine
      }
   else if (is_cons(params_sexpr))
      {
      Value cur = params_sexpr;
      while (is_cons(cur))
         {
         Value p = car(cur);
         if (!is_symbol(p))
            throw SchemeAnalysisError(
                "expected an identifier in " + form_name +
                    " parameter list, got " + render(p),
                src_of(p));
         fixed.push_back({as_symbol(p), as_symbol_id(p), src_of(p)});
         cur = cdr(cur);
         }
      if (is_nil(cur))
         {
         // proper list
         }
      else if (is_symbol(cur))
         {
         has_rest = true;
         rest_name = as_symbol(cur);
         rest_id = as_symbol_id(cur);
         rest_src = src_of(cur);
         }
      else
         {
         throw SchemeAnalysisError(
             "expected an identifier in " + form_name +
                 " rest parameter, got " + render(cur),
             src_of(cur));
         }
      }
   else
      {
      throw SchemeAnalysisError(
          form_name + " parameter list must be a list or identifier, got " +
              render(params_sexpr),
          src_of(params_sexpr));
      }

   std::unordered_set<std::string> seen;
   for (const auto& pe : fixed)
      {
      if (seen.count(pe.name))
         throw SchemeAnalysisError(
             "duplicate parameter name in " + form_name + ": " + pe.name, pe.src);
      seen.insert(pe.name);
      }
   if (has_rest && seen.count(rest_name))
      throw SchemeAnalysisError(
          "rest parameter name conflicts with fixed parameter: " + rest_name, rest_src);

   std::vector<std::string> shadowed;
   std::vector<uint32_t> fixed_ids;
   shadowed.reserve(fixed.size() + (has_rest ? 1 : 0));
   for (const auto& pe : fixed)
      {
      shadowed.push_back(pe.name);
      fixed_ids.push_back(pe.id);
      }
   if (has_rest)
      shadowed.push_back(rest_name);
   StaticEnv body_env = shadow_env(senv, shadowed);

   if (proper_list_length(body_cons) <= 0)
      throw SchemeAnalysisError(form_name + " body cannot be empty", outer_src);

   CompileScope body_scope{closure_scope_names(fixed_ids, rest_id), scope};
   return analyze_seq(body_cons, body_env, &body_scope);
   }

// ── Let-binding helper ────────────────────────────────────────────────────────

static std::vector<std::pair<std::string, Value>>
parse_let_bindings(const Value& bindings_sexpr, const std::string& form_name)
   {
   if (!is_cons(bindings_sexpr) && !is_nil(bindings_sexpr))
      throw SchemeAnalysisError(
          form_name + " bindings must be a list, got " + render(bindings_sexpr),
          src_of(bindings_sexpr));
   if (proper_list_length(bindings_sexpr) < 0)
      throw SchemeAnalysisError(
          form_name + " bindings must be a proper list", src_of(bindings_sexpr));
   std::vector<std::pair<std::string, Value>> pairs;
   Value cur = bindings_sexpr;
   while (is_cons(cur))
      {
      Value b = car(cur);
      if (proper_list_length(b) != 2)
         throw SchemeAnalysisError(
             form_name + " binding must be (name value), got " + render(b), src_of(b));
      std::string var = require_symbol(car(b), form_name + " binding");
      pairs.push_back({var, car(cdr(b))});
      cur = cdr(cur);
      }
   return pairs;
   }

static void check_unique_let_names(const std::vector<std::pair<std::string, Value>>& pairs,
                                   const std::string& form_name, const Value& sexpr)
   {
   std::unordered_set<std::string> seen;
   for (const auto& p : pairs)
      {
      if (seen.count(p.first))
         throw SchemeAnalysisError(
             "duplicate variable name in " + form_name + " bindings: " + p.first,
             src_of(sexpr));
      seen.insert(p.first);
      }
   }

// ── Special-form handler implementations ─────────────────────────────────────
// Each handler validates shape, recurses into sub-expressions with the scope
// chain they will run in, and returns the rewritten form.

static Value analyze_lambda(const Value& sexpr, const StaticEnv& senv,
                            const CompileScope* scope)
   {
   if (proper_list_length(sexpr) < 3)
      throw SchemeAnalysisError(
          "lambda requires a parameter list and at least one body expression",
          src_of(sexpr));
   Value body = analyze_lambda_shape(car(cdr(sexpr)), cdr(cdr(sexpr)), "lambda",
                                     src_of(sexpr), senv, scope);
   return rebuild(sexpr, car(sexpr), rebuild(cdr(sexpr), car(cdr(sexpr)), body));
   }

static Value analyze_case_lambda(const Value& sexpr, const StaticEnv& senv,
                                 const CompileScope* scope)
   {
   // Zero clauses is permitted by R7RS §4.2.9; the resulting procedure raises
   // a SchemeArityError on any call (no matching clause).
   std::vector<Value> new_clauses;
   Value cur = cdr(sexpr);
   while (is_cons(cur))
      {
      Value clause = car(cur);
      if (!is_cons(clause))
         throw SchemeAnalysisError(
             "case-lambda clause must be a list, got " + render(clause), src_of(clause));
      if (proper_list_length(clause) < 2)
         throw SchemeAnalysisError(
             "case-lambda clause must have formals and a non-empty body, got " +
                 render(clause),
             src_of(clause));
      Value body = analyze_lambda_shape(car(clause), cdr(clause), "case-lambda",
                                        src_of(clause), senv, scope);
      new_clauses.push_back(rebuild(clause, car(clause), body));
      cur = cdr(cur);
      }
   return rebuild(sexpr, car(sexpr), rebuild_list(cdr(sexpr), new_clauses));
   }

static Value analyze_define(const Value& sexpr, const StaticEnv& senv,
                            const CompileScope* scope)
   {
   int n = proper_list_length(sexpr) - 1;
   if (n != 2)
      throw SchemeAnalysisError(
          "define requires a name and a value (got " + std::to_string(n) + " arguments)",
          src_of(sexpr));
   Value name_sexpr = car(cdr(sexpr));
   Value value_sexpr = car(cdr(cdr(sexpr)));
   std::string name = require_symbol(name_sexpr, "define");
   // Pre-bind name's arity so recursive references in lambda body can be checked.
   auto arity = peek_lambda_arity(value_sexpr);
   Value value;
   if (arity)
      {
      StaticEnv value_env = senv;
      value_env[name] = arity;
      value = analyze(value_sexpr, value_env, scope);
      }
   else
      {
      value = analyze(value_sexpr, senv, scope);
      }
   // The name is not resolved: define always binds into the dict scope.
   Value tail = cdr(cdr(sexpr));
   return rebuild(sexpr, car(sexpr),
                  rebuild(cdr(sexpr), name_sexpr, rebuild(tail, value, cdr(tail))));
   }

static Value analyze_set(const Value& sexpr, const StaticEnv& senv,
                         const CompileScope* scope)
   {
   int n = proper_list_length(sexpr) - 1;
   if (n != 2)
      throw SchemeAnalysisError(
          "set! requires a name and a value (got " + std::to_string(n) + " arguments)",
          src_of(sexpr));
   Value name_sexpr = car(cdr(sexpr));
   require_symbol(name_sexpr, "set!");
   Value tail = cdr(cdr(sexpr));
   Value value = analyze(car(tail), senv, scope);
   return rebuild(sexpr, car(sexpr),
                  rebuild(cdr(sexpr), ref(name_sexpr, scope),
                          rebuild(tail, value, cdr(tail))));
   }

static Value analyze_if(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope)
   {
   int n = proper_list_length(sexpr) - 1;
   if (n != 2 && n != 3)
      throw SchemeAnalysisError(
          "if requires 2 or 3 arguments (test, then, optional else), got " + std::to_string(n),
          src_of(sexpr));
   return rebuild(sexpr, car(sexpr), analyze_seq(cdr(sexpr), senv, scope));
   }

static Value analyze_begin(const Value& sexpr, const StaticEnv& senv,
                           const CompileScope* scope)
   {
   return rebuild(sexpr, car(sexpr), analyze_seq(cdr(sexpr), senv, scope));
   }

static Value analyze_named_let(const Value& sexpr, const StaticEnv& senv,
                               const CompileScope* scope)
   {
   if (proper_list_length(sexpr) < 4)
      throw SchemeAnalysisError(
          "named let requires a name, a binding list, and at least one body expression",
          src_of(sexpr));
   Value name_sym = car(cdr(sexpr));
   std::string name = as_symbol(name_sym);
   Value bindings_sexpr = car(cdr(cdr(sexpr)));
   auto pairs = parse_let_bindings(bindings_sexpr, "named let");
   std::vector<std::string> params;
   for (const auto& p : pairs)
      params.push_back(p.first);
   std::unordered_set<std::string> seen;
   for (const auto& p : params)
      {
      if (seen.count(p))
         throw SchemeAnalysisError(
             "duplicate parameter name in named let: " + p,
             src_of(car(cdr(cdr(sexpr)))));
      seen.insert(p);
      }
   // The Evaluator evaluates the inits inside the scope that holds the loop
   // name, so they are resolved there.  They cannot see the name itself: the
   // Expander renames the binding (R7RS 4.2.4).  Arity-wise they are still
   // checked against the enclosing static env, as before.
   ScopeNames loop_sids =
       std::make_shared<const std::vector<uint32_t>>(1, as_symbol_id(name_sym));
   CompileScope loop_scope{loop_sids, scope};
   std::vector<Value> inits;
   for (const auto& p : pairs)
      inits.push_back(analyze(p.second, senv, &loop_scope));
   // Body sees name with fixed arity and params shadowed.
   StaticEnv name_env = senv;
   name_env[name] = std::make_pair((int)params.size(), (int)params.size());
   StaticEnv body_env = shadow_env(name_env, params);
   Value body_cons = cdr(cdr(cdr(sexpr)));
   if (proper_list_length(body_cons) <= 0)
      throw SchemeAnalysisError("named let body cannot be empty", src_of(sexpr));
   ScopeNames param_sids = name_sids(pairs);
   CompileScope body_scope{param_sids, &loop_scope};
   Value body = analyze_seq(body_cons, body_env, &body_scope);
   return with_scope_names(
       sexpr, {loop_sids, param_sids},
       alloc_cons(name_sym,
                  alloc_cons(rebuild_bindings(bindings_sexpr, inits), body,
                             src_of(cdr(cdr(sexpr)))),
                  src_of(cdr(sexpr))));
   }

static Value analyze_let(const Value& sexpr_in, const StaticEnv& senv,
                         const CompileScope* scope)
   {
   Value sexpr = strip_scope_names(sexpr_in);
   if (proper_list_length(sexpr) < 3)
      throw SchemeAnalysisError(
          "let requires a binding list and at least one body expression", src_of(sexpr));
   if (is_symbol(car(cdr(sexpr))))
      return analyze_named_let(sexpr, senv, scope);
   Value bindings_sexpr = car(cdr(sexpr));
   auto pairs = parse_let_bindings(bindings_sexpr, "let");
   check_unique_let_names(pairs, "let", sexpr);
   std::vector<Value> inits;
   for (const auto& p : pairs)
      inits.push_back(analyze(p.second, senv, scope));
   std::vector<std::string> names;
   for (const auto& p : pairs)
      names.push_back(p.first);
   StaticEnv body_env = shadow_env(senv, names);
   Value body_cons = cdr(cdr(sexpr));
   if (proper_list_length(body_cons) <= 0)
      throw SchemeAnalysisError("let body cannot be empty", src_of(sexpr));
   std::vector<ScopeNames> scopes;
   CompileScope body_scope{nullptr, scope};
   const CompileScope* inner = scope;
   if (!pairs.empty())
      {
      body_scope.names = name_sids(pairs);
      scopes.push_back(body_scope.names);
      inner = &body_scope;
      }
   Value body = analyze_seq(body_cons, body_env, inner);
   return with_scope_names(
       sexpr, std::move(scopes),
       alloc_cons(rebuild_bindings(bindings_sexpr, inits), body, src_of(cdr(sexpr))));
   }

static Value analyze_let_star(const Value& sexpr_in, const StaticEnv& senv,
                              const CompileScope* scope)
   {
   Value sexpr = strip_scope_names(sexpr_in);
   if (proper_list_length(sexpr) < 3)
      throw SchemeAnalysisError(
          "let* requires a binding list and at least one body expression", src_of(sexpr));
   Value bindings_sexpr = car(cdr(sexpr));
   auto pairs = parse_let_bindings(bindings_sexpr, "let*");
   StaticEnv current_env = senv;
   // One scope per binding; a deque keeps each one's address stable while
   // the later ones point at it.
   std::deque<CompileScope> chain;
   const CompileScope* current_scope = scope;
   std::vector<Value> inits;
   std::vector<ScopeNames> scopes;
   for (const auto& p : pairs)
      {
      inits.push_back(analyze(p.second, current_env, current_scope));
      current_env = shadow_env(current_env, {p.first});
      ScopeNames sids = std::make_shared<const std::vector<uint32_t>>(1, intern_symbol(p.first));
      scopes.push_back(sids);
      chain.push_back(CompileScope{sids, current_scope});
      current_scope = &chain.back();
      }
   Value body_cons = cdr(cdr(sexpr));
   if (proper_list_length(body_cons) <= 0)
      throw SchemeAnalysisError("let* body cannot be empty", src_of(sexpr));
   Value body = analyze_seq(body_cons, current_env, current_scope);
   return with_scope_names(
       sexpr, std::move(scopes),
       alloc_cons(rebuild_bindings(bindings_sexpr, inits), body, src_of(cdr(sexpr))));
   }

static Value analyze_letrec_family(const Value& sexpr_in, const StaticEnv& senv,
                                   const std::string& name, const CompileScope* scope)
   {
   Value sexpr = strip_scope_names(sexpr_in);
   if (proper_list_length(sexpr) < 3)
      throw SchemeAnalysisError(
          name + " requires a binding list and at least one body expression", src_of(sexpr));
   Value bindings_sexpr = car(cdr(sexpr));
   auto pairs = parse_let_bindings(bindings_sexpr, name);
   check_unique_let_names(pairs, name, sexpr);
   std::vector<std::string> names;
   for (const auto& p : pairs)
      names.push_back(p.first);
   StaticEnv inner_env = shadow_env(senv, names);
   std::vector<ScopeNames> scopes;
   CompileScope inner_scope{nullptr, scope};
   const CompileScope* inner = scope;
   if (!pairs.empty())
      {
      inner_scope.names = name_sids(pairs);
      scopes.push_back(inner_scope.names);
      inner = &inner_scope;
      }
   std::vector<Value> inits;
   for (const auto& p : pairs)
      inits.push_back(analyze(p.second, inner_env, inner));
   Value body_cons = cdr(cdr(sexpr));
   if (proper_list_length(body_cons) <= 0)
      throw SchemeAnalysisError(name + " body cannot be empty", src_of(sexpr));
   Value body = analyze_seq(body_cons, inner_env, inner);
   return with_scope_names(
       sexpr, std::move(scopes),
       alloc_cons(rebuild_bindings(bindings_sexpr, inits), body, src_of(cdr(sexpr))));
   }

static Value analyze_letrec(const Value& sexpr, const StaticEnv& senv,
                            const CompileScope* scope)
   {
   return analyze_letrec_family(sexpr, senv, "letrec", scope);
   }
static Value analyze_letrec_star(const Value& sexpr, const StaticEnv& senv,
                                 const CompileScope* scope)
   {
   return analyze_letrec_family(sexpr, senv, "letrec*", scope);
   }

static Value analyze_cond(const Value& sexpr, const StaticEnv& senv,
                          const CompileScope* scope)
   {
   if (proper_list_length(sexpr) < 2)
      throw SchemeAnalysisError("cond must have at least one clause", src_of(sexpr));
   auto clauses = cons_to_list(cdr(sexpr));
   int total = (int)clauses.size();
   std::vector<Value> new_clauses;
   for (int i = 0; i < total; i++)
      {
      Value clause = clauses[i];
      int clen = proper_list_length(clause);
      if (clen <= 0)
         throw SchemeAnalysisError(
             "cond clause must be a non-empty list, got " + render(clause), src_of(clause));
      Value head = car(clause);
      bool head_is_else = is_symbol(head) && as_symbol(head) == "else" &&
                          senv.count("else") == 0;
      bool head_is_arrow = clen == 3 && is_symbol(car(cdr(clause))) &&
                           as_symbol(car(cdr(clause))) == "=>" &&
                           senv.count("=>") == 0;
      if (head_is_else)
         {
         if (i != total - 1)
            throw SchemeAnalysisError(
                "cond 'else' clause must be the last clause", src_of(clause));
         Value body_cons = cdr(clause);
         if (proper_list_length(body_cons) <= 0)
            throw SchemeAnalysisError(
                "cond 'else' clause must have at least one expression", src_of(clause));
         new_clauses.push_back(rebuild(clause, head, analyze_seq(body_cons, senv, scope)));
         }
      else if (head_is_arrow)
         {
         Value test = analyze(car(clause), senv, scope);
         Value arrow_cell = cdr(clause);
         Value proc_cell = cdr(arrow_cell);
         Value proc = analyze(car(proc_cell), senv, scope);
         new_clauses.push_back(rebuild(
             clause, test,
             rebuild(arrow_cell, car(arrow_cell), rebuild(proc_cell, proc, cdr(proc_cell)))));
         }
      else
         {
         // (<test>) or (<test> <expr>...)
         new_clauses.push_back(analyze_seq(clause, senv, scope));
         }
      }
   return rebuild(sexpr, car(sexpr), rebuild_list(cdr(sexpr), new_clauses));
   }

// The clause rebuilt around its analyzed body: (=> proc) when arrow_ok and
// the body starts with =>, else a sequence of expressions.
static Value analyze_case_body(const Value& clause, const Value& body_cons,
                               const StaticEnv& senv, const CompileScope* scope,
                               bool arrow_ok, const char* msg)
   {
   if (arrow_ok && is_cons(body_cons) && is_symbol(car(body_cons)) &&
       as_symbol(car(body_cons)) == "=>")
      {
      if (!is_cons(cdr(body_cons)) || !is_nil(cdr(cdr(body_cons))))
         throw SchemeAnalysisError(msg, src_of(clause));
      Value proc_cell = cdr(body_cons);
      Value proc = analyze(car(proc_cell), senv, scope);
      return rebuild(clause, car(clause),
                     rebuild(body_cons, car(body_cons),
                             rebuild(proc_cell, proc, cdr(proc_cell))));
      }
   return rebuild(clause, car(clause), analyze_seq(body_cons, senv, scope));
   }

static Value analyze_case(const Value& sexpr, const StaticEnv& senv,
                          const CompileScope* scope)
   {
   if (proper_list_length(sexpr) < 3)
      throw SchemeAnalysisError("case requires a key and at least one clause", src_of(sexpr));
   Value key = analyze(car(cdr(sexpr)), senv, scope);
   auto clauses = cons_to_list(cdr(cdr(sexpr)));
   int total = (int)clauses.size();
   std::vector<Value> new_clauses;
   for (int i = 0; i < total; i++)
      {
      Value clause = clauses[i];
      int clen = proper_list_length(clause);
      if (clen < 2)
         throw SchemeAnalysisError(
             "case clause must be (<datum-list> <expr>...) or (else <expr>...), got " +
                 render(clause),
             src_of(clause));
      Value head = car(clause);
      if (is_symbol(head) && as_symbol(head) == "else" && senv.count("else") == 0)
         {
         if (i != total - 1)
            throw SchemeAnalysisError(
                "case 'else' clause must be the last clause", src_of(clause));
         // (else => proc-expr) or (else <expr>...)
         new_clauses.push_back(analyze_case_body(
             clause, cdr(clause), senv, scope, senv.count("=>") == 0,
             "case 'else =>' clause must have exactly one expression"));
         }
      else
         {
         if (!is_cons(head) && !is_nil(head))
            throw SchemeAnalysisError(
                "case clause head must be a list of datums, got " + render(head), src_of(head));
         if (proper_list_length(head) < 0)
            throw SchemeAnalysisError("case datum list must be a proper list", src_of(head));
         // ((<datum>...) => proc-expr) or ((<datum>...) <expr>...)
         new_clauses.push_back(analyze_case_body(
             clause, cdr(clause), senv, scope, true,
             "case '=>' clause must have exactly one expression"));
         }
      }
   return rebuild(sexpr, car(sexpr),
                  rebuild(cdr(sexpr), key, rebuild_list(cdr(cdr(sexpr)), new_clauses)));
   }

static Value analyze_do(const Value& sexpr, const StaticEnv& /*senv*/,
                        const CompileScope* /*scope*/)
   {
   if (proper_list_length(sexpr) < 3)
      throw SchemeAnalysisError("do requires a binding list and a test clause", src_of(sexpr));
   Value bindings_sexpr = car(cdr(sexpr));
   Value test_sexpr = car(cdr(cdr(sexpr)));
   if (proper_list_length(bindings_sexpr) < 0)
      throw SchemeAnalysisError(
          "do bindings must be a proper list, got " + render(bindings_sexpr),
          src_of(bindings_sexpr));
   std::unordered_set<std::string> seen;
   Value cur = bindings_sexpr;
   while (is_cons(cur))
      {
      Value b = car(cur);
      int blen = proper_list_length(b);
      if (blen != 2 && blen != 3)
         throw SchemeAnalysisError(
             "do binding must be (var init) or (var init step), got " + render(b), src_of(b));
      std::string name = require_symbol(car(b), "do binding");
      if (seen.count(name))
         throw SchemeAnalysisError(
             "duplicate variable name in do bindings: " + name, src_of(car(b)));
      seen.insert(name);
      cur = cdr(cur);
      }
   if (!is_cons(test_sexpr))
      throw SchemeAnalysisError(
          "do test clause must be a list starting with a test expression", src_of(test_sexpr));
   if (proper_list_length(test_sexpr) < 1)
      throw SchemeAnalysisError("do test clause must be a proper list", src_of(test_sexpr));
   return sexpr;
   }

static Value analyze_include(const Value& sexpr, const StaticEnv& /*senv*/,
                             const CompileScope* /*scope*/)
   {
   int n = proper_list_length(sexpr) - 1;
   if (n < 1)
      throw SchemeAnalysisError(
          "include requires at least one filename string", src_of(sexpr));
   Value cur = cdr(sexpr);
   while (is_cons(cur))
      {
      Value arg = car(cur);
      if (!is_string(arg))
         throw SchemeAnalysisError(
             "include arguments must be string literals, got " + render(arg), src_of(arg));
      cur = cdr(cur);
      }
   return sexpr;
   }

static Value analyze_cond_expand(const Value& sexpr, const StaticEnv& /*senv*/,
                                 const CompileScope* /*scope*/)
   {
   if (proper_list_length(sexpr) < 2)
      throw SchemeAnalysisError("cond-expand requires at least one clause", src_of(sexpr));
   auto clauses = cons_to_list(cdr(sexpr));
   for (const Value& clause : clauses)
      if (proper_list_length(clause) < 1)
         throw SchemeAnalysisError(
             "cond-expand clause must be a non-empty list", src_of(clause));
   throw SchemeAnalysisError("cond-expand: no clause matched", src_of(sexpr));
   }

static Value analyze_and(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope)
   {
   return rebuild(sexpr, car(sexpr), analyze_seq(cdr(sexpr), senv, scope));
   }

static Value analyze_or(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope)
   {
   return rebuild(sexpr, car(sexpr), analyze_seq(cdr(sexpr), senv, scope));
   }

static Value analyze_when_unless(const Value& sexpr, const StaticEnv& senv,
                                 const std::string& name, const CompileScope* scope)
   {
   if (proper_list_length(sexpr) < 3)
      throw SchemeAnalysisError(
          name + " requires a test and at least one body expression", src_of(sexpr));
   return rebuild(sexpr, car(sexpr), analyze_seq(cdr(sexpr), senv, scope));
   }

static Value analyze_when(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope)
   {
   return analyze_when_unless(sexpr, senv, "when", scope);
   }
static Value analyze_unless(const Value& sexpr, const StaticEnv& senv,
                            const CompileScope* scope)
   {
   return analyze_when_unless(sexpr, senv, "unless", scope);
   }

static Value analyze_quote(const Value& sexpr, const StaticEnv& /*senv*/,
                           const CompileScope* /*scope*/)
   {
   int n = proper_list_length(sexpr) - 1;
   if (n != 1)
      throw SchemeAnalysisError(
          "quote requires exactly 1 argument, got " + std::to_string(n), src_of(sexpr));
   // Quoted data is literal - do NOT walk or analyze its contents.
   return sexpr;
   }

static Value analyze_delay(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope)
   {
   std::string name = as_symbol(car(sexpr));
   int n = proper_list_length(sexpr) - 1;
   if (n != 1)
      throw SchemeAnalysisError(
          name + " requires exactly 1 argument, got " + std::to_string(n), src_of(sexpr));
   // The expression runs when the promise's thunk (no parameters) is called:
   // in an empty scope under this one.
   CompileScope thunk_scope{std::make_shared<const std::vector<uint32_t>>(), scope};
   Value expr = analyze(car(cdr(sexpr)), senv, &thunk_scope);
   return rebuild(sexpr, car(sexpr), rebuild(cdr(sexpr), expr, cdr(cdr(sexpr))));
   }

static Value analyze_library_form(const Value& sexpr, const StaticEnv& /*senv*/,
                                  const CompileScope* /*scope*/)
   {
   return sexpr;
   }

static Value analyze_unquote_outside(const Value& sexpr, const StaticEnv& /*senv*/,
                                     const CompileScope* /*scope*/)
   {
   std::string head_name = as_symbol(car(sexpr));
   throw SchemeAnalysisError(
       head_name + " is only valid inside a quasiquote template", src_of(sexpr));
   }

// ── Public analyze ────────────────────────────────────────────────────────────

// Definition context (R7RS 5.1 / 5.3).  A define may appear only at the top
// level -- including inside a top-level begin -- or at the start of a body.
// Body-leading defines never reach the Analyzer (the Expander rewrites them
// into letrec*), so any define the Analyzer meets below the top level sits
// in an expression context, e.g. (if c (define y 1)): conditionally creating
// a variable.  That is rejected.  s_def_ctx is true while the form being
// analyzed is in a definition context; each analyze() call clears it for its
// subforms (a top-level begin passes it through) and restores it on exit, so
// every external caller, which analyzes a top-level form, starts with true.
static thread_local bool s_def_ctx = true;

static Value analyze_form(const Value& sexpr, const StaticEnv& senv, bool def_ok,
                          const CompileScope* scope);

Value analyze(const Value& sexpr, const StaticEnv& senv, const CompileScope* scope)
   {
   bool def_ok = s_def_ctx;
   s_def_ctx = def_ok && is_cons(sexpr) && is_symbol(car(sexpr)) &&
               as_symbol(car(sexpr)) == "begin";
   struct Restore
      {
      bool v;
      ~Restore()
         {
         s_def_ctx = v;
         }
      };
   Restore restore{def_ok};
   return analyze_form(sexpr, senv, def_ok, scope);
   }

static Value analyze_form(const Value& sexpr, const StaticEnv& senv, bool def_ok,
                          const CompileScope* scope)
   {
   std::call_once(s_init_flag, init_special_forms);

   if (is_nil(sexpr))
      throw SchemeAnalysisError(
          "empty list () is not a valid expression; use (quote ()) for the empty list",
          src_of(sexpr));
   if (!is_cons(sexpr))
      {
      // R7RS §3.1: keyword used as expression is an error, unless locally
      // rebound (local names are stored as nullopt in senv; global stubs
      // have a real arity pair).
      if (is_symbol(sexpr))
         {
         const std::string& name = as_symbol(sexpr);
         auto it = senv.find(name);
         // A FREE reference (absent, or a global stub with a real arity) vs a
         // locally rebound name (stored as nullopt by the shadow mechanism).
         bool free_ref = (it == senv.end() || it->second.has_value());
         if (SYNTACTIC_KEYWORDS.count(name) && free_ref)
            throw SchemeSyntaxError(
                "keyword used as expression: " + name, src_of(sexpr));
         // A user-defined macro keyword (define-syntax / let-syntax) used as
         // an expression is likewise an error (R7RS 4.3.1).  Only a free
         // reference that resolves to a transformer fires, so lexical
         // shadowing keeps working; quoted data never reaches here.
         if (free_ref && is_syntax_transformer(lookup_macro(sexpr)))
            throw SchemeSyntaxError(
                "keyword used as expression: " + name, src_of(sexpr));
         return ref(sexpr, scope);
         }
      return sexpr;
      }

   Value head = car(sexpr);
   if (is_symbol(head))
      {
      auto it = s_special_forms.find(as_symbol(head));
      if (it != s_special_forms.end())
         {
         if (!def_ok && it->first == "define")
            throw SchemeSyntaxError(
                "definition not allowed in an expression context",
                src_of(sexpr));
         return it->second(sexpr, senv, scope);
         }
      }

   // Application
   if (proper_list_length(sexpr) < 0)
      throw SchemeAnalysisError("application must be a proper list", src_of(sexpr));
   Value fn_sexpr = car(sexpr);
   std::vector<Value> args;
   Value cur = cdr(sexpr);
   while (is_cons(cur))
      {
      args.push_back(car(cur));
      cur = cdr(cur);
      }
   // Don't fire keyword check for the fn/head position: a keyword in head
   // position is a malformed special form; the arity check gives a better error.
   // Keywords in arg positions are still checked.
   bool fn_is_global_kw = is_symbol(fn_sexpr) &&
                          SYNTACTIC_KEYWORDS.count(as_symbol(fn_sexpr)) &&
                          [&]
   { auto it = senv.find(as_symbol(fn_sexpr));
             return it == senv.end() || it->second.has_value(); }();
   Value new_fn = fn_sexpr;
   if (!fn_is_global_kw)
      new_fn = analyze(fn_sexpr, senv, scope);
   Value new_args = analyze_seq(cdr(sexpr), senv, scope);
   check_app_arity(fn_sexpr, args, senv, sexpr);
   return rebuild(sexpr, new_fn, new_args);
   }

Value analyze(const Value& sexpr)
   {
   return analyze(sexpr, primitive_arities(), nullptr);
   }
