// Analyzer.cpp -- semantic validator.
// Direct port of pyscheme/Analyzer.py.
#include "Analyzer.h"
#include "Parser.h"   // SchemeSyntaxError
#include "expander.h" // lookup_macro
#include <algorithm>
#include <cctype>
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

using AnalysisHandler = std::function<void(const Value&, const StaticEnv&)>;
static std::once_flag s_init_flag;
static std::unordered_map<std::string, AnalysisHandler> s_special_forms;

// Forward declarations for all special-form handlers
static void analyze_lambda(const Value& sexpr, const StaticEnv& senv);
static void analyze_case_lambda(const Value& sexpr, const StaticEnv& senv);
static void analyze_define(const Value& sexpr, const StaticEnv& senv);
static void analyze_set(const Value& sexpr, const StaticEnv& senv);
static void analyze_if(const Value& sexpr, const StaticEnv& senv);
static void analyze_begin(const Value& sexpr, const StaticEnv& senv);
static void analyze_let(const Value& sexpr, const StaticEnv& senv);
static void analyze_let_star(const Value& sexpr, const StaticEnv& senv);
static void analyze_letrec(const Value& sexpr, const StaticEnv& senv);
static void analyze_letrec_star(const Value& sexpr, const StaticEnv& senv);
static void analyze_cond(const Value& sexpr, const StaticEnv& senv);
static void analyze_case(const Value& sexpr, const StaticEnv& senv);
static void analyze_do(const Value& sexpr, const StaticEnv& senv);
static void analyze_include(const Value& sexpr, const StaticEnv& senv);
static void analyze_cond_expand(const Value& sexpr, const StaticEnv& senv);
static void analyze_and(const Value& sexpr, const StaticEnv& senv);
static void analyze_or(const Value& sexpr, const StaticEnv& senv);
static void analyze_when(const Value& sexpr, const StaticEnv& senv);
static void analyze_unless(const Value& sexpr, const StaticEnv& senv);
static void analyze_quote(const Value& sexpr, const StaticEnv& senv);
static void analyze_delay(const Value& sexpr, const StaticEnv& senv);
static void analyze_library_form(const Value& sexpr, const StaticEnv& senv);
static void analyze_unquote_outside(const Value& sexpr, const StaticEnv& senv);

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
      return std::to_string(as_rational_num(sexpr)) + "/" +
             std::to_string(as_rational_den(sexpr));
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

// ── Lambda shape validator ────────────────────────────────────────────────────

static void analyze_lambda_shape(const Value& params_sexpr, const Value& body_cons,
                                 const std::string& form_name, SourceInfo* outer_src,
                                 const StaticEnv& senv)
   {
   struct ParamEntry
      {
      std::string name;
      SourceInfo* src;
      };
   std::vector<ParamEntry> fixed;
   std::string rest_name;
   SourceInfo* rest_src = nullptr;
   bool has_rest = false;

   if (is_symbol(params_sexpr))
      {
      has_rest = true;
      rest_name = as_symbol(params_sexpr);
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
         fixed.push_back({as_symbol(p), src_of(p)});
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
   shadowed.reserve(fixed.size() + (has_rest ? 1 : 0));
   for (const auto& pe : fixed)
      shadowed.push_back(pe.name);
   if (has_rest)
      shadowed.push_back(rest_name);
   StaticEnv body_env = shadow_env(senv, shadowed);

   if (proper_list_length(body_cons) <= 0)
      throw SchemeAnalysisError(form_name + " body cannot be empty", outer_src);

   Value cur = body_cons;
   while (is_cons(cur))
      {
      analyze(car(cur), body_env);
      cur = cdr(cur);
      }
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

static void analyze_lambda(const Value& sexpr, const StaticEnv& senv)
   {
   if (proper_list_length(sexpr) < 3)
      throw SchemeAnalysisError(
          "lambda requires a parameter list and at least one body expression",
          src_of(sexpr));
   analyze_lambda_shape(car(cdr(sexpr)), cdr(cdr(sexpr)), "lambda", src_of(sexpr), senv);
   }

static void analyze_case_lambda(const Value& sexpr, const StaticEnv& senv)
   {
   // Zero clauses is permitted by R7RS §4.2.9; the resulting procedure raises
   // a SchemeArityError on any call (no matching clause).
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
      analyze_lambda_shape(car(clause), cdr(clause), "case-lambda", src_of(clause), senv);
      cur = cdr(cur);
      }
   }

static void analyze_define(const Value& sexpr, const StaticEnv& senv)
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
   if (arity)
      {
      StaticEnv value_env = senv;
      value_env[name] = arity;
      analyze(value_sexpr, value_env);
      }
   else
      {
      analyze(value_sexpr, senv);
      }
   }

static void analyze_set(const Value& sexpr, const StaticEnv& senv)
   {
   int n = proper_list_length(sexpr) - 1;
   if (n != 2)
      throw SchemeAnalysisError(
          "set! requires a name and a value (got " + std::to_string(n) + " arguments)",
          src_of(sexpr));
   require_symbol(car(cdr(sexpr)), "set!");
   analyze(car(cdr(cdr(sexpr))), senv);
   }

static void analyze_if(const Value& sexpr, const StaticEnv& senv)
   {
   int n = proper_list_length(sexpr) - 1;
   if (n != 2 && n != 3)
      throw SchemeAnalysisError(
          "if requires 2 or 3 arguments (test, then, optional else), got " + std::to_string(n),
          src_of(sexpr));
   analyze(car(cdr(sexpr)), senv);
   analyze(car(cdr(cdr(sexpr))), senv);
   if (n == 3)
      analyze(car(cdr(cdr(cdr(sexpr)))), senv);
   }

static void analyze_begin(const Value& sexpr, const StaticEnv& senv)
   {
   Value cur = cdr(sexpr);
   while (is_cons(cur))
      {
      analyze(car(cur), senv);
      cur = cdr(cur);
      }
   }

static void analyze_named_let(const Value& sexpr, const StaticEnv& senv)
   {
   if (proper_list_length(sexpr) < 4)
      throw SchemeAnalysisError(
          "named let requires a name, a binding list, and at least one body expression",
          src_of(sexpr));
   std::string name = as_symbol(car(cdr(sexpr)));
   auto pairs = parse_let_bindings(car(cdr(cdr(sexpr))), "named let");
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
   // Inits evaluate in enclosing env.
   for (const auto& p : pairs)
      analyze(p.second, senv);
   // Body sees name with fixed arity and params shadowed.
   StaticEnv name_env = senv;
   name_env[name] = std::make_pair((int)params.size(), (int)params.size());
   StaticEnv body_env = shadow_env(name_env, params);
   Value body_cons = cdr(cdr(cdr(sexpr)));
   if (proper_list_length(body_cons) <= 0)
      throw SchemeAnalysisError("named let body cannot be empty", src_of(sexpr));
   Value cur = body_cons;
   while (is_cons(cur))
      {
      analyze(car(cur), body_env);
      cur = cdr(cur);
      }
   }

static void analyze_let(const Value& sexpr, const StaticEnv& senv)
   {
   if (proper_list_length(sexpr) < 3)
      throw SchemeAnalysisError(
          "let requires a binding list and at least one body expression", src_of(sexpr));
   if (is_symbol(car(cdr(sexpr))))
      {
      analyze_named_let(sexpr, senv);
      return;
      }
   auto pairs = parse_let_bindings(car(cdr(sexpr)), "let");
   check_unique_let_names(pairs, "let", sexpr);
   for (const auto& p : pairs)
      analyze(p.second, senv);
   std::vector<std::string> names;
   for (const auto& p : pairs)
      names.push_back(p.first);
   StaticEnv body_env = shadow_env(senv, names);
   Value body_cons = cdr(cdr(sexpr));
   if (proper_list_length(body_cons) <= 0)
      throw SchemeAnalysisError("let body cannot be empty", src_of(sexpr));
   Value cur = body_cons;
   while (is_cons(cur))
      {
      analyze(car(cur), body_env);
      cur = cdr(cur);
      }
   }

static void analyze_let_star(const Value& sexpr, const StaticEnv& senv)
   {
   if (proper_list_length(sexpr) < 3)
      throw SchemeAnalysisError(
          "let* requires a binding list and at least one body expression", src_of(sexpr));
   auto pairs = parse_let_bindings(car(cdr(sexpr)), "let*");
   StaticEnv current_env = senv;
   for (const auto& p : pairs)
      {
      analyze(p.second, current_env);
      current_env = shadow_env(current_env, {p.first});
      }
   Value body_cons = cdr(cdr(sexpr));
   if (proper_list_length(body_cons) <= 0)
      throw SchemeAnalysisError("let* body cannot be empty", src_of(sexpr));
   Value cur = body_cons;
   while (is_cons(cur))
      {
      analyze(car(cur), current_env);
      cur = cdr(cur);
      }
   }

static void analyze_letrec_family(const Value& sexpr, const StaticEnv& senv,
                                  const std::string& name)
   {
   if (proper_list_length(sexpr) < 3)
      throw SchemeAnalysisError(
          name + " requires a binding list and at least one body expression", src_of(sexpr));
   auto pairs = parse_let_bindings(car(cdr(sexpr)), name);
   check_unique_let_names(pairs, name, sexpr);
   std::vector<std::string> names;
   for (const auto& p : pairs)
      names.push_back(p.first);
   StaticEnv inner_env = shadow_env(senv, names);
   for (const auto& p : pairs)
      analyze(p.second, inner_env);
   Value body_cons = cdr(cdr(sexpr));
   if (proper_list_length(body_cons) <= 0)
      throw SchemeAnalysisError(name + " body cannot be empty", src_of(sexpr));
   Value cur = body_cons;
   while (is_cons(cur))
      {
      analyze(car(cur), inner_env);
      cur = cdr(cur);
      }
   }

static void analyze_letrec(const Value& sexpr, const StaticEnv& senv)
   {
   analyze_letrec_family(sexpr, senv, "letrec");
   }
static void analyze_letrec_star(const Value& sexpr, const StaticEnv& senv)
   {
   analyze_letrec_family(sexpr, senv, "letrec*");
   }

static void analyze_cond(const Value& sexpr, const StaticEnv& senv)
   {
   if (proper_list_length(sexpr) < 2)
      throw SchemeAnalysisError("cond must have at least one clause", src_of(sexpr));
   auto clauses = cons_to_list(cdr(sexpr));
   int total = (int)clauses.size();
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
         Value cur = body_cons;
         while (is_cons(cur))
            {
            analyze(car(cur), senv);
            cur = cdr(cur);
            }
         }
      else if (head_is_arrow)
         {
         analyze(car(clause), senv);           // test
         analyze(car(cdr(cdr(clause))), senv); // proc
         }
      else if (clen == 1)
         {
         analyze(car(clause), senv);
         }
      else
         {
         analyze(car(clause), senv);
         Value cur = cdr(clause);
         while (is_cons(cur))
            {
            analyze(car(cur), senv);
            cur = cdr(cur);
            }
         }
      }
   }

static void analyze_case(const Value& sexpr, const StaticEnv& senv)
   {
   if (proper_list_length(sexpr) < 3)
      throw SchemeAnalysisError("case requires a key and at least one clause", src_of(sexpr));
   analyze(car(cdr(sexpr)), senv);
   auto clauses = cons_to_list(cdr(cdr(sexpr)));
   int total = (int)clauses.size();
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
         Value body_cons = cdr(clause);
         if (is_cons(body_cons) && is_symbol(car(body_cons)) &&
             as_symbol(car(body_cons)) == "=>" && senv.count("=>") == 0)
            {
            if (!is_cons(cdr(body_cons)) || !is_nil(cdr(cdr(body_cons))))
               throw SchemeAnalysisError(
                   "case 'else =>' clause must have exactly one expression",
                   src_of(clause));
            analyze(car(cdr(body_cons)), senv);
            }
         else
            {
            Value cur = body_cons;
            while (is_cons(cur))
               {
               analyze(car(cur), senv);
               cur = cdr(cur);
               }
            }
         }
      else
         {
         if (!is_cons(head) && !is_nil(head))
            throw SchemeAnalysisError(
                "case clause head must be a list of datums, got " + render(head), src_of(head));
         if (proper_list_length(head) < 0)
            throw SchemeAnalysisError("case datum list must be a proper list", src_of(head));
         Value body_cons = cdr(clause);
         if (is_cons(body_cons) && is_symbol(car(body_cons)) &&
             as_symbol(car(body_cons)) == "=>")
            {
            if (!is_cons(cdr(body_cons)) || !is_nil(cdr(cdr(body_cons))))
               throw SchemeAnalysisError(
                   "case '=>' clause must have exactly one expression", src_of(clause));
            analyze(car(cdr(body_cons)), senv);
            }
         else
            {
            Value cur = body_cons;
            while (is_cons(cur))
               {
               analyze(car(cur), senv);
               cur = cdr(cur);
               }
            }
         }
      }
   }

static void analyze_do(const Value& sexpr, const StaticEnv& /*senv*/)
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
   }

static void analyze_include(const Value& sexpr, const StaticEnv& /*senv*/)
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
   }

static void analyze_cond_expand(const Value& sexpr, const StaticEnv& /*senv*/)
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

static void analyze_and(const Value& sexpr, const StaticEnv& senv)
   {
   Value cur = cdr(sexpr);
   while (is_cons(cur))
      {
      analyze(car(cur), senv);
      cur = cdr(cur);
      }
   }

static void analyze_or(const Value& sexpr, const StaticEnv& senv)
   {
   Value cur = cdr(sexpr);
   while (is_cons(cur))
      {
      analyze(car(cur), senv);
      cur = cdr(cur);
      }
   }

static void analyze_when_unless(const Value& sexpr, const StaticEnv& senv,
                                const std::string& name)
   {
   if (proper_list_length(sexpr) < 3)
      throw SchemeAnalysisError(
          name + " requires a test and at least one body expression", src_of(sexpr));
   analyze(car(cdr(sexpr)), senv);
   Value cur = cdr(cdr(sexpr));
   while (is_cons(cur))
      {
      analyze(car(cur), senv);
      cur = cdr(cur);
      }
   }

static void analyze_when(const Value& sexpr, const StaticEnv& senv)
   {
   analyze_when_unless(sexpr, senv, "when");
   }
static void analyze_unless(const Value& sexpr, const StaticEnv& senv)
   {
   analyze_when_unless(sexpr, senv, "unless");
   }

static void analyze_quote(const Value& sexpr, const StaticEnv& /*senv*/)
   {
   int n = proper_list_length(sexpr) - 1;
   if (n != 1)
      throw SchemeAnalysisError(
          "quote requires exactly 1 argument, got " + std::to_string(n), src_of(sexpr));
   }

static void analyze_delay(const Value& sexpr, const StaticEnv& senv)
   {
   std::string name = as_symbol(car(sexpr));
   int n = proper_list_length(sexpr) - 1;
   if (n != 1)
      throw SchemeAnalysisError(
          name + " requires exactly 1 argument, got " + std::to_string(n), src_of(sexpr));
   analyze(car(cdr(sexpr)), senv);
   }

static void analyze_library_form(const Value& /*sexpr*/, const StaticEnv& /*senv*/) {}

static void analyze_unquote_outside(const Value& sexpr, const StaticEnv& /*senv*/)
   {
   std::string head_name = as_symbol(car(sexpr));
   throw SchemeAnalysisError(
       head_name + " is only valid inside a quasiquote template", src_of(sexpr));
   }

// ── Public analyze ────────────────────────────────────────────────────────────

Value analyze(const Value& sexpr, const StaticEnv& senv)
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
         }
      return sexpr;
      }

   Value head = car(sexpr);
   if (is_symbol(head))
      {
      auto it = s_special_forms.find(as_symbol(head));
      if (it != s_special_forms.end())
         {
         it->second(sexpr, senv);
         return sexpr;
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
   if (!fn_is_global_kw)
      analyze(fn_sexpr, senv);
   for (const Value& arg : args)
      analyze(arg, senv);
   check_app_arity(fn_sexpr, args, senv, sexpr);
   return sexpr;
   }

Value analyze(const Value& sexpr)
   {
   return analyze(sexpr, primitive_arities());
   }
