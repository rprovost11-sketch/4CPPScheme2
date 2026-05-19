#include "analyzer.h"
#include "environment.h"
#include "symbol.h"
#include <algorithm>
#include <mutex>
#include <unordered_set>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Primitive arity registry

static std::unordered_map<std::string, std::pair<int,int>> g_prim_arities;
static std::mutex g_prim_mutex;

void register_primitive_arity(const std::string& name, int min_args, int max_args)
   {
   std::lock_guard<std::mutex> lock(g_prim_mutex);
   g_prim_arities[name] = {min_args, max_args};
   }

void seed_static_env(StaticEnv& senv)
   {
   std::lock_guard<std::mutex> lock(g_prim_mutex);
   for (const auto& kv : g_prim_arities)
      senv[kv.first] = kv.second;
   }

// ─────────────────────────────────────────────────────────────────────────────
// Helpers

static bool sym_named(Value v, const char* name)
   {
   return is_symbol(v) && symbol_name(as_symbol_id(v)) == name;
   }

static int list_length(Value cell)
   {
   if (is_nil(cell)) return 0;
   int n = 0;
   Value cur = cell;
   while (is_cons(cur)) { ++n; cur = as_cons(cur)->cdr; }
   return is_nil(cur) ? n : -1;
   }

static std::vector<Value> list_to_vec(Value cell)
   {
   std::vector<Value> v;
   Value cur = cell;
   while (is_cons(cur)) { v.push_back(as_cons(cur)->car); cur = as_cons(cur)->cdr; }
   return v;
   }

static constexpr const char GENSYM_PFX[] = "\x01h.";
static constexpr size_t GENSYM_PFX_LEN   = 3;

static std::string strip_gensym(const std::string& name)
   {
   if (name.size() < GENSYM_PFX_LEN || name.compare(0, GENSYM_PFX_LEN, GENSYM_PFX) != 0)
      return name;
   std::string rest = name.substr(GENSYM_PFX_LEN);
   size_t dot = rest.rfind('.');
   if (dot != std::string::npos)
      {
      std::string sfx = rest.substr(dot + 1);
      if (!sfx.empty() && std::all_of(sfx.begin(), sfx.end(), ::isdigit))
         return rest.substr(0, dot);
      }
   return rest;
   }

static std::string render(Value v)
   {
   if (is_nil(v))    return "()";
   if (is_bool(v))   return as_bool(v) ? "#t" : "#f";
   if (is_fixnum(v)) return std::to_string(as_fixnum(v));
   if (is_flonum(v)) return std::to_string(as_flonum(v));
   if (is_symbol(v)) return strip_gensym(symbol_name(as_symbol_id(v)));
   if (is_string(v)) return "\"" + as_string(v)->data + "\"";
   if (is_char(v))
      {
      char32_t cp = as_char(v);
      if (cp < 128) { std::string s = "#\\"; s += (char)cp; return s; }
      return "#\\<char>";
      }
   if (is_rational(v))
      {
      auto* r = as_rational(v);
      return std::to_string(r->num) + "/" + std::to_string(r->den);
      }
   if (is_cons(v))
      {
      std::string out = "(";
      Value cur = v;
      bool first = true;
      while (is_cons(cur))
         {
         if (!first) out += " ";
         first = false;
         out += render(as_cons(cur)->car);
         cur = as_cons(cur)->cdr;
         }
      if (!is_nil(cur)) { out += " . "; out += render(cur); }
      out += ")";
      return out;
      }
   return "<#value>";
   }

static std::string require_sym(Value v, const std::string& ctx)
   {
   if (!is_symbol(v))
      throw SchemeAnalysisError("expected an identifier in " + ctx + ", got " + render(v));
   return symbol_name(as_symbol_id(v));
   }

static StaticEnv shadow_env(const StaticEnv& env, const std::vector<std::string>& names)
   {
   StaticEnv copy = env;
   for (const auto& n : names) copy.erase(n);
   return copy;
   }

// ─────────────────────────────────────────────────────────────────────────────
// Static arity

static std::pair<int,int> lambda_arity(Value lam)
   {
   Value params = as_cons(as_cons(lam)->cdr)->car;
   if (is_symbol(params)) return {0, -1};
   if (is_nil(params))    return {0,  0};
   int n = 0;
   Value cur = params;
   while (is_cons(cur)) { ++n; cur = as_cons(cur)->cdr; }
   return is_nil(cur) ? std::make_pair(n, n) : std::make_pair(n, -1);
   }

static std::optional<std::pair<int,int>> peek_lambda_arity(Value v)
   {
   if (!is_cons(v) || !sym_named(as_cons(v)->car, "lambda")) return std::nullopt;
   if (!is_cons(as_cons(v)->cdr))                             return std::nullopt;
   if (list_length(v) < 3)                                    return std::nullopt;
   return lambda_arity(v);
   }

static void check_call_arity(Value fn, const std::vector<Value>& args,
                               const StaticEnv& env)
   {
   std::string fn_name;
   std::pair<int,int> arity{0, 0};
   bool found = false;

   if (is_cons(fn) && sym_named(as_cons(fn)->car, "lambda"))
      {
      auto opt = peek_lambda_arity(fn);
      if (!opt) return;
      arity = *opt;
      found = true;
      }
   else if (is_symbol(fn))
      {
      fn_name = symbol_name(as_symbol_id(fn));
      auto it  = env.find(fn_name);
      if (it == env.end()) return;
      arity = it->second;
      found = true;
      }
   if (!found) return;

   int lo = arity.first;
   int hi = arity.second;
   int n  = (int)args.size();
   if (n < lo || (hi >= 0 && n > hi))
      throw SchemeArityError(arity_mismatch_msg(fn_name, lo, hi, n));
   }

void extend_static_env_with_define(StaticEnv& senv, Value sexpr)
   {
   if (!is_cons(sexpr) || !sym_named(as_cons(sexpr)->car, "define")) return;
   Value r1 = as_cons(sexpr)->cdr;
   if (!is_cons(r1)) return;
   Value r2 = as_cons(r1)->cdr;
   if (!is_cons(r2)) return;
   Value name_v  = as_cons(r1)->car;
   Value value_v = as_cons(r2)->car;
   if (!is_symbol(name_v)) return;
   std::string name = symbol_name(as_symbol_id(name_v));

   if (is_cons(value_v) && sym_named(as_cons(value_v)->car, "lambda"))
      {
      auto opt = peek_lambda_arity(value_v);
      if (opt) senv[name] = *opt;
      else     senv.erase(name);
      }
   else if (is_cons(value_v) && sym_named(as_cons(value_v)->car, "case-lambda"))
      {
      senv[name] = {0, -1};
      }
   else if (is_symbol(value_v))
      {
      std::string alias = symbol_name(as_symbol_id(value_v));
      auto it = senv.find(alias);
      if (it != senv.end()) senv[name] = it->second;
      else                  senv.erase(name);
      }
   else
      {
      senv.erase(name);
      }
   }

// ─────────────────────────────────────────────────────────────────────────────
// Forward declaration

static void analyze_impl(Value sexpr, const StaticEnv& env);

// ─────────────────────────────────────────────────────────────────────────────
// Let-binding helpers

static std::vector<std::pair<std::string,Value>>
parse_let_bindings(Value bindings, const std::string& form)
   {
   if (!is_cons(bindings) && !is_nil(bindings))
      throw SchemeAnalysisError(
         form + " bindings must be a list, got " + render(bindings));
   if (list_length(bindings) < 0)
      throw SchemeAnalysisError(form + " bindings must be a proper list");
   std::vector<std::pair<std::string,Value>> pairs;
   Value cur = bindings;
   while (is_cons(cur))
      {
      Value b = as_cons(cur)->car;
      if (list_length(b) != 2)
         throw SchemeAnalysisError(
            form + " binding must be (name value), got " + render(b));
      std::string var = require_sym(as_cons(b)->car, form + " binding");
      pairs.push_back({var, as_cons(as_cons(b)->cdr)->car});
      cur = as_cons(cur)->cdr;
      }
   return pairs;
   }

static void check_unique_names(
   const std::vector<std::pair<std::string,Value>>& pairs,
   const std::string& form)
   {
   std::unordered_set<std::string> seen;
   for (const auto& kv : pairs)
      if (!seen.insert(kv.first).second)
         throw SchemeAnalysisError(
            "duplicate variable name in " + form + " bindings: " + kv.first);
   }

// ─────────────────────────────────────────────────────────────────────────────
// Lambda shape validator (shared by lambda and case-lambda)

static void check_lambda_shape(Value params, Value body_cons,
                                const std::string& form, const StaticEnv& env)
   {
   std::vector<std::string> fixed;
   std::string rest;
   bool has_rest = false;

   if (is_symbol(params))
      {
      rest = symbol_name(as_symbol_id(params));
      has_rest = true;
      }
   else if (is_nil(params))
      {
      // no params
      }
   else if (is_cons(params))
      {
      Value cur = params;
      while (is_cons(cur))
         {
         Value p = as_cons(cur)->car;
         if (!is_symbol(p))
            throw SchemeAnalysisError(
               "expected an identifier in " + form + " parameter list, got " + render(p));
         fixed.push_back(symbol_name(as_symbol_id(p)));
         cur = as_cons(cur)->cdr;
         }
      if (!is_nil(cur))
         {
         if (!is_symbol(cur))
            throw SchemeAnalysisError(
               "expected an identifier in " + form + " rest parameter, got " + render(cur));
         rest = symbol_name(as_symbol_id(cur));
         has_rest = true;
         }
      }
   else
      {
      throw SchemeAnalysisError(
         form + " parameter list must be a list or identifier, got " + render(params));
      }

   std::unordered_set<std::string> seen;
   for (const auto& p : fixed)
      if (!seen.insert(p).second)
         throw SchemeAnalysisError("duplicate parameter name in " + form + ": " + p);
   if (has_rest && seen.count(rest))
      throw SchemeAnalysisError(
         "rest parameter name conflicts with fixed parameter: " + rest);

   std::vector<std::string> all = fixed;
   if (has_rest) all.push_back(rest);
   StaticEnv benv = shadow_env(env, all);

   if (list_length(body_cons) <= 0)
      throw SchemeAnalysisError(form + " body cannot be empty");
   Value cur = body_cons;
   while (is_cons(cur))
      {
      analyze_impl(as_cons(cur)->car, benv);
      cur = as_cons(cur)->cdr;
      }
   }

// ─────────────────────────────────────────────────────────────────────────────
// Special-form handlers

static void h_lambda(Value sx, const StaticEnv& env)
   {
   if (list_length(sx) < 3)
      throw SchemeAnalysisError(
         "lambda requires a parameter list and at least one body expression");
   Value cdr1 = as_cons(sx)->cdr;
   check_lambda_shape(as_cons(cdr1)->car, as_cons(cdr1)->cdr, "lambda", env);
   }

static void h_define(Value sx, const StaticEnv& env)
   {
   int n = list_length(sx) - 1;
   if (n != 2)
      throw SchemeAnalysisError(
         "define requires a name and a value (got " + std::to_string(n) + " arguments)");
   Value r1 = as_cons(sx)->cdr;
   Value name_v  = as_cons(r1)->car;
   Value value_v = as_cons(as_cons(r1)->cdr)->car;
   require_sym(name_v, "define");
   std::string name = symbol_name(as_symbol_id(name_v));
   StaticEnv venv = env;
   auto opt = peek_lambda_arity(value_v);
   if (opt) venv[name] = *opt;
   analyze_impl(value_v, venv);
   }

static void h_set(Value sx, const StaticEnv& env)
   {
   int n = list_length(sx) - 1;
   if (n != 2)
      throw SchemeAnalysisError(
         "set! requires a name and a value (got " + std::to_string(n) + " arguments)");
   Value r1 = as_cons(sx)->cdr;
   require_sym(as_cons(r1)->car, "set!");
   analyze_impl(as_cons(as_cons(r1)->cdr)->car, env);
   }

static void h_if(Value sx, const StaticEnv& env)
   {
   int n = list_length(sx) - 1;
   if (n != 2 && n != 3)
      throw SchemeAnalysisError(
         "if requires 2 or 3 arguments (test, then, optional else), got "
         + std::to_string(n));
   Value r1 = as_cons(sx)->cdr;
   Value r2 = as_cons(r1)->cdr;
   analyze_impl(as_cons(r1)->car, env);
   analyze_impl(as_cons(r2)->car, env);
   if (n == 3)
      analyze_impl(as_cons(as_cons(r2)->cdr)->car, env);
   }

static void h_begin(Value sx, const StaticEnv& env)
   {
   if (list_length(sx) < 2)
      throw SchemeAnalysisError("begin must have at least one body expression");
   Value cur = as_cons(sx)->cdr;
   while (is_cons(cur)) { analyze_impl(as_cons(cur)->car, env); cur = as_cons(cur)->cdr; }
   }

static void h_named_let(Value sx, const StaticEnv& env)
   {
   if (list_length(sx) < 4)
      throw SchemeAnalysisError(
         "named let requires a name, a binding list, and at least one body expression");
   Value r1 = as_cons(sx)->cdr;
   std::string loop_name = require_sym(as_cons(r1)->car, "named let");
   Value r2 = as_cons(r1)->cdr;
   auto pairs = parse_let_bindings(as_cons(r2)->car, "named let");

   std::unordered_set<std::string> seen;
   for (const auto& kv : pairs)
      if (!seen.insert(kv.first).second)
         throw SchemeAnalysisError(
            "duplicate parameter name in named let: " + kv.first);

   for (const auto& kv : pairs) analyze_impl(kv.second, env);

   StaticEnv name_env = env;
   name_env[loop_name] = {(int)pairs.size(), (int)pairs.size()};
   std::vector<std::string> pnames;
   for (const auto& kv : pairs) pnames.push_back(kv.first);
   StaticEnv body_env = shadow_env(name_env, pnames);

   Value body_cons = as_cons(r2)->cdr;
   if (list_length(body_cons) <= 0)
      throw SchemeAnalysisError("named let body cannot be empty");
   Value cur = body_cons;
   while (is_cons(cur)) { analyze_impl(as_cons(cur)->car, body_env); cur = as_cons(cur)->cdr; }
   }

static void h_let(Value sx, const StaticEnv& env)
   {
   if (list_length(sx) < 3)
      throw SchemeAnalysisError(
         "let requires a binding list and at least one body expression");
   Value r1 = as_cons(sx)->cdr;
   if (is_symbol(as_cons(r1)->car)) { h_named_let(sx, env); return; }

   auto pairs = parse_let_bindings(as_cons(r1)->car, "let");
   check_unique_names(pairs, "let");
   for (const auto& kv : pairs) analyze_impl(kv.second, env);

   std::vector<std::string> names;
   for (const auto& kv : pairs) names.push_back(kv.first);
   StaticEnv benv = shadow_env(env, names);

   Value body_cons = as_cons(r1)->cdr;
   if (list_length(body_cons) <= 0)
      throw SchemeAnalysisError("let body cannot be empty");
   Value cur = body_cons;
   while (is_cons(cur)) { analyze_impl(as_cons(cur)->car, benv); cur = as_cons(cur)->cdr; }
   }

static void h_let_star(Value sx, const StaticEnv& env)
   {
   if (list_length(sx) < 3)
      throw SchemeAnalysisError(
         "let* requires a binding list and at least one body expression");
   Value r1 = as_cons(sx)->cdr;
   auto pairs = parse_let_bindings(as_cons(r1)->car, "let*");
   StaticEnv cur_env = env;
   for (const auto& kv : pairs)
      {
      analyze_impl(kv.second, cur_env);
      cur_env = shadow_env(cur_env, {kv.first});
      }
   Value body_cons = as_cons(r1)->cdr;
   if (list_length(body_cons) <= 0)
      throw SchemeAnalysisError("let* body cannot be empty");
   Value cur = body_cons;
   while (is_cons(cur)) { analyze_impl(as_cons(cur)->car, cur_env); cur = as_cons(cur)->cdr; }
   }

static void h_letrec_family(Value sx, const StaticEnv& env, const std::string& form)
   {
   if (list_length(sx) < 3)
      throw SchemeAnalysisError(
         form + " requires a binding list and at least one body expression");
   Value r1 = as_cons(sx)->cdr;
   auto pairs = parse_let_bindings(as_cons(r1)->car, form);
   check_unique_names(pairs, form);
   std::vector<std::string> names;
   for (const auto& kv : pairs) names.push_back(kv.first);
   StaticEnv inner = shadow_env(env, names);
   for (const auto& kv : pairs) analyze_impl(kv.second, inner);
   Value body_cons = as_cons(r1)->cdr;
   if (list_length(body_cons) <= 0)
      throw SchemeAnalysisError(form + " body cannot be empty");
   Value cur = body_cons;
   while (is_cons(cur)) { analyze_impl(as_cons(cur)->car, inner); cur = as_cons(cur)->cdr; }
   }

static void h_letrec(Value sx, const StaticEnv& env)      { h_letrec_family(sx, env, "letrec");  }
static void h_letrec_star(Value sx, const StaticEnv& env) { h_letrec_family(sx, env, "letrec*"); }

static void h_cond(Value sx, const StaticEnv& env)
   {
   if (list_length(sx) < 2)
      throw SchemeAnalysisError("cond must have at least one clause");
   auto clauses = list_to_vec(as_cons(sx)->cdr);
   size_t total = clauses.size();
   for (size_t i = 0; i < total; ++i)
      {
      Value clause = clauses[i];
      int clen = list_length(clause);
      if (clen <= 0)
         throw SchemeAnalysisError(
            "cond clause must be a non-empty list, got " + render(clause));
      Value head = as_cons(clause)->car;
      bool head_else  = sym_named(head, "else") && env.find("else") == env.end();
      bool head_arrow = (clen == 3
                         && sym_named(as_cons(as_cons(clause)->cdr)->car, "=>")
                         && env.find("=>") == env.end());
      if (head_else)
         {
         if (i != total - 1)
            throw SchemeAnalysisError("cond 'else' clause must be the last clause");
         Value body = as_cons(clause)->cdr;
         if (list_length(body) <= 0)
            throw SchemeAnalysisError(
               "cond 'else' clause must have at least one expression");
         Value cur = body;
         while (is_cons(cur)) { analyze_impl(as_cons(cur)->car, env); cur = as_cons(cur)->cdr; }
         }
      else if (head_arrow)
         {
         analyze_impl(as_cons(clause)->car, env);
         analyze_impl(as_cons(as_cons(as_cons(clause)->cdr)->cdr)->car, env);
         }
      else if (clen == 1)
         {
         analyze_impl(as_cons(clause)->car, env);
         }
      else
         {
         analyze_impl(as_cons(clause)->car, env);
         Value cur = as_cons(clause)->cdr;
         while (is_cons(cur)) { analyze_impl(as_cons(cur)->car, env); cur = as_cons(cur)->cdr; }
         }
      }
   }

static void h_case(Value sx, const StaticEnv& env)
   {
   if (list_length(sx) < 3)
      throw SchemeAnalysisError("case requires a key and at least one clause");
   analyze_impl(as_cons(as_cons(sx)->cdr)->car, env);
   auto clauses = list_to_vec(as_cons(as_cons(sx)->cdr)->cdr);
   size_t total = clauses.size();
   for (size_t i = 0; i < total; ++i)
      {
      Value clause = clauses[i];
      int clen = list_length(clause);
      if (clen < 2)
         throw SchemeAnalysisError(
            "case clause must be (<datum-list> <expr>...) or (else <expr>...), got "
            + render(clause));
      Value head    = as_cons(clause)->car;
      Value body    = as_cons(clause)->cdr;
      bool is_else  = sym_named(head, "else") && env.find("else") == env.end();
      bool is_arrow = is_cons(body)
                      && sym_named(as_cons(body)->car, "=>")
                      && env.find("=>") == env.end();
      if (is_else)
         {
         if (i != total - 1)
            throw SchemeAnalysisError("case 'else' clause must be the last clause");
         if (is_arrow)
            {
            if (!(is_cons(as_cons(body)->cdr) && is_nil(as_cons(as_cons(body)->cdr)->cdr)))
               throw SchemeAnalysisError(
                  "case 'else =>' clause must have exactly one expression");
            analyze_impl(as_cons(as_cons(body)->cdr)->car, env);
            }
         else
            {
            Value cur = body;
            while (is_cons(cur)) { analyze_impl(as_cons(cur)->car, env); cur = as_cons(cur)->cdr; }
            }
         }
      else
         {
         if (!is_cons(head) && !is_nil(head))
            throw SchemeAnalysisError(
               "case clause head must be a list of datums, got " + render(head));
         if (list_length(head) < 0)
            throw SchemeAnalysisError("case datum list must be a proper list");
         if (is_arrow)
            {
            if (!(is_cons(as_cons(body)->cdr) && is_nil(as_cons(as_cons(body)->cdr)->cdr)))
               throw SchemeAnalysisError(
                  "case '=>' clause must have exactly one expression");
            analyze_impl(as_cons(as_cons(body)->cdr)->car, env);
            }
         else
            {
            Value cur = body;
            while (is_cons(cur)) { analyze_impl(as_cons(cur)->car, env); cur = as_cons(cur)->cdr; }
            }
         }
      }
   }

static void h_case_lambda(Value sx, const StaticEnv& env)
   {
   if (list_length(sx) < 2)
      throw SchemeAnalysisError("case-lambda requires at least one clause");
   Value cur = as_cons(sx)->cdr;
   while (is_cons(cur))
      {
      Value clause = as_cons(cur)->car;
      if (!is_cons(clause))
         throw SchemeAnalysisError(
            "case-lambda clause must be a list, got " + render(clause));
      if (list_length(clause) < 2)
         throw SchemeAnalysisError(
            "case-lambda clause must have formals and a non-empty body, got "
            + render(clause));
      check_lambda_shape(as_cons(clause)->car, as_cons(clause)->cdr, "case-lambda", env);
      cur = as_cons(cur)->cdr;
      }
   }

static void h_and(Value sx, const StaticEnv& env)
   {
   Value cur = as_cons(sx)->cdr;
   while (is_cons(cur)) { analyze_impl(as_cons(cur)->car, env); cur = as_cons(cur)->cdr; }
   }

static void h_or(Value sx, const StaticEnv& env)
   {
   Value cur = as_cons(sx)->cdr;
   while (is_cons(cur)) { analyze_impl(as_cons(cur)->car, env); cur = as_cons(cur)->cdr; }
   }

static void h_when_unless(Value sx, const StaticEnv& env, const std::string& form)
   {
   if (list_length(sx) < 3)
      throw SchemeAnalysisError(
         form + " requires a test and at least one body expression");
   Value r1 = as_cons(sx)->cdr;
   analyze_impl(as_cons(r1)->car, env);
   Value cur = as_cons(r1)->cdr;
   while (is_cons(cur)) { analyze_impl(as_cons(cur)->car, env); cur = as_cons(cur)->cdr; }
   }

static void h_when(Value sx, const StaticEnv& env)   { h_when_unless(sx, env, "when");   }
static void h_unless(Value sx, const StaticEnv& env) { h_when_unless(sx, env, "unless"); }

static void h_quote(Value sx, const StaticEnv&)
   {
   int n = list_length(sx) - 1;
   if (n != 1)
      throw SchemeAnalysisError(
         "quote requires exactly 1 argument, got " + std::to_string(n));
   }

static void h_delay(Value sx, const StaticEnv& env)
   {
   std::string form = symbol_name(as_symbol_id(as_cons(sx)->car));
   int n = list_length(sx) - 1;
   if (n != 1)
      throw SchemeAnalysisError(
         form + " requires exactly 1 argument, got " + std::to_string(n));
   analyze_impl(as_cons(as_cons(sx)->cdr)->car, env);
   }

static void h_include(Value sx, const StaticEnv&)
   {
   int n = list_length(sx) - 1;
   if (n < 1)
      throw SchemeAnalysisError("include requires at least one filename string");
   Value cur = as_cons(sx)->cdr;
   while (is_cons(cur))
      {
      Value arg = as_cons(cur)->car;
      if (!is_string(arg))
         throw SchemeAnalysisError(
            "include arguments must be string literals, got " + render(arg));
      cur = as_cons(cur)->cdr;
      }
   }

static void h_do(Value sx, const StaticEnv&)
   {
   if (list_length(sx) < 3)
      throw SchemeAnalysisError("do requires a binding list and a test clause");
   Value bindings = as_cons(as_cons(sx)->cdr)->car;
   Value test_sx  = as_cons(as_cons(as_cons(sx)->cdr)->cdr)->car;
   if (list_length(bindings) < 0)
      throw SchemeAnalysisError(
         "do bindings must be a proper list, got " + render(bindings));
   std::unordered_set<std::string> seen;
   Value cur = bindings;
   while (is_cons(cur))
      {
      Value b    = as_cons(cur)->car;
      int   blen = list_length(b);
      if (blen != 2 && blen != 3)
         throw SchemeAnalysisError(
            "do binding must be (var init) or (var init step), got " + render(b));
      std::string nm = require_sym(as_cons(b)->car, "do binding");
      if (!seen.insert(nm).second)
         throw SchemeAnalysisError("duplicate variable name in do bindings: " + nm);
      cur = as_cons(cur)->cdr;
      }
   if (!is_cons(test_sx))
      throw SchemeAnalysisError(
         "do test clause must be a list starting with a test expression");
   if (list_length(test_sx) < 1)
      throw SchemeAnalysisError("do test clause must be a proper list");
   }

static void h_unquote_outside(Value sx, const StaticEnv&)
   {
   std::string head = symbol_name(as_symbol_id(as_cons(sx)->car));
   throw SchemeAnalysisError(head + " is only valid inside a quasiquote template");
   }

static void h_cond_expand(Value sx, const StaticEnv&)
   {
   if (list_length(sx) < 2)
      throw SchemeAnalysisError("cond-expand requires at least one clause");
   for (const auto& cl : list_to_vec(as_cons(sx)->cdr))
      if (list_length(cl) < 1)
         throw SchemeAnalysisError("cond-expand clause must be a non-empty list");
   throw SchemeAnalysisError("cond-expand: no clause matched");
   }

static void h_passthrough(Value, const StaticEnv&) {}

// ─────────────────────────────────────────────────────────────────────────────
// Dispatch table

using Handler = void(*)(Value, const StaticEnv&);

static const std::unordered_map<std::string, Handler> SPECIAL_FORMS = {
   {"lambda",                       h_lambda},
   {"case-lambda",                  h_case_lambda},
   {"define",                       h_define},
   {"set!",                         h_set},
   {"if",                           h_if},
   {"begin",                        h_begin},
   {"let",                          h_let},
   {"let*",                         h_let_star},
   {"letrec",                       h_letrec},
   {"letrec*",                      h_letrec_star},
   {"cond",                         h_cond},
   {"case",                         h_case},
   {"do",                           h_do},
   {"include",                      h_include},
   {"include-ci",                   h_include},
   {"cond-expand",                  h_cond_expand},
   {"and",                          h_and},
   {"or",                           h_or},
   {"when",                         h_when},
   {"unless",                       h_unless},
   {"quote",                        h_quote},
   {"unquote",                      h_unquote_outside},
   {"unquote-splicing",             h_unquote_outside},
   {"delay",                        h_delay},
   {"delay-force",                  h_delay},
   {"define-library",               h_passthrough},
   {"import",                       h_passthrough},
   {"export",                       h_passthrough},
   {"include-library-declarations", h_passthrough},
   {"define-syntax",                h_passthrough},
   {"let-syntax",                   h_passthrough},
   {"letrec-syntax",                h_passthrough},
   {"syntax-rules",                 h_passthrough},
};

// ─────────────────────────────────────────────────────────────────────────────
// Main dispatcher

static void analyze_impl(Value sexpr, const StaticEnv& env)
   {
   if (is_nil(sexpr))
      throw SchemeAnalysisError(
         "empty list () is not a valid expression; use (quote ()) for the empty list");
   if (!is_cons(sexpr)) return;   // atom / symbol leaf

   Value head = as_cons(sexpr)->car;
   if (is_symbol(head))
      {
      const std::string& hname = symbol_name(as_symbol_id(head));
      auto it = SPECIAL_FORMS.find(hname);
      if (it != SPECIAL_FORMS.end()) { it->second(sexpr, env); return; }
      }

   // Application
   if (list_length(sexpr) < 0)
      throw SchemeAnalysisError("application must be a proper list");
   Value fn = as_cons(sexpr)->car;
   std::vector<Value> args;
   Value cur = as_cons(sexpr)->cdr;
   while (is_cons(cur)) { args.push_back(as_cons(cur)->car); cur = as_cons(cur)->cdr; }
   analyze_impl(fn, env);
   for (const auto& a : args) analyze_impl(a, env);
   check_call_arity(fn, args, env);
   }

// ─────────────────────────────────────────────────────────────────────────────
// Public API

Value analyze(Value val, StaticEnv* static_env)
   {
   if (static_env)
      {
      analyze_impl(val, *static_env);
      }
   else
      {
      StaticEnv fresh;
      seed_static_env(fresh);
      analyze_impl(val, fresh);
      }
   return val;
   }
