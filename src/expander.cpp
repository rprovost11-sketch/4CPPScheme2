#include "expander.h"
#include "parser.h"
#include "gc.h"
#include <atomic>
#include <algorithm>
#include <cctype>
#include <filesystem>

static constexpr const char* GENSYM_PREFIX = "\x01h.";
static constexpr int         GENSYM_PREFIX_LEN = 3;

static std::atomic<uint32_t> g_gensym_counter{0};

std::string hygiene_gensym(std::string_view base)
   {
   if (base.size() >= (size_t)GENSYM_PREFIX_LEN &&
       base.substr(0, GENSYM_PREFIX_LEN) == GENSYM_PREFIX)
      return std::string(base);
   uint32_t n = ++g_gensym_counter;
   return std::string(GENSYM_PREFIX) + std::string(base) + "." + std::to_string(n);
   }

void hygiene_gensym_reset()
   {
   g_gensym_counter.store(0);
   }

// ── Cons construction helpers ─────────────────────────────────────────────────

static Value cons2(Value car_val, Value cdr_val)
   {
   auto* cell = gc_alloc_cons();
   cell->car  = car_val;
   cell->cdr  = cdr_val;
   return make_cons(cell);
   }

static Value make_list(const std::vector<Value>& items)
   {
   Value result = make_nil();
   for (int i = (int)items.size() - 1; i >= 0; --i)
      {
      auto* cell = gc_alloc_cons();
      cell->car  = items[i];
      cell->cdr  = result;
      result     = make_cons(cell);
      }
   return result;
   }

// ── List utilities ────────────────────────────────────────────────────────────

static int list_length(Value v)
   {
   int n = 0;
   while (is_cons(v))
      {
      ++n;
      v = cdr(v);
      }
   return is_nil(v) ? n : -1;
   }

static bool is_head_sym(Value form, std::string_view name)
   {
   if (!is_cons(form)) return false;
   Value h = car(form);
   if (!is_symbol(h)) return false;
   return symbol_name(as_symbol_id(h)) == name;
   }

// ── Rename table type ─────────────────────────────────────────────────────────

using RenameTable = std::unordered_map<std::string, std::string>;

static RenameTable mask_table(const RenameTable& table,
                              const std::vector<std::string>& bound)
   {
   if (bound.empty()) return table;
   RenameTable result;
   for (auto& [k, v] : table)
      {
      bool masked = false;
      for (auto& b : bound)
         if (b == k) { masked = true; break; }
      if (!masked) result[k] = v;
      }
   return result;
   }

// ── Forward declarations ──────────────────────────────────────────────────────

static Value expand_impl(Value sexpr, Environment*& env_ref, int& depth);
static Value expand_list(Value cell, Environment*& env_ref, int& depth);
static Value expand_body(Value body_cons, Environment*& env_ref, int& depth);
static Value rename_refs(Value form, const RenameTable& table);

// ── _gensym_rename_formals ────────────────────────────────────────────────────

static Value gensym_rename_formals(Value formals, RenameTable& table)
   {
   if (is_symbol(formals))
      {
      std::string name = symbol_name(as_symbol_id(formals));
      std::string gs   = hygiene_gensym(name);
      table[name]      = gs;
      return make_symbol(gs);
      }
   if (is_nil(formals))
      return formals;
   std::vector<std::string> seen;
   std::vector<Value>       items;
   Value cur = formals;
   while (is_cons(cur))
      {
      Value sym = car(cur);
      if (is_symbol(sym))
         {
         std::string name = symbol_name(as_symbol_id(sym));
         for (auto& s : seen)
            if (s == name)
               throw SchemeSyntaxError("duplicate parameter name in lambda: " + name);
         seen.push_back(name);
         std::string gs = hygiene_gensym(name);
         table[name]    = gs;
         items.push_back(make_symbol(gs));
         }
      else
         items.push_back(sym);
      cur = cdr(cur);
      }
   Value tail;
   if (is_nil(cur))
      tail = make_nil();
   else if (is_symbol(cur))
      {
      std::string name = symbol_name(as_symbol_id(cur));
      for (auto& s : seen)
         if (s == name)
            throw SchemeSyntaxError("rest parameter name conflicts with fixed parameter: " + name);
      std::string gs = hygiene_gensym(name);
      table[name]    = gs;
      tail = make_symbol(gs);
      }
   else
      tail = cur;
   Value result = tail;
   for (int i = (int)items.size() - 1; i >= 0; --i)
      result = cons2(items[i], result);
   return result;
   }

// ── rename_refs ───────────────────────────────────────────────────────────────

static std::vector<std::string> collect_formals_names(Value formals)
   {
   std::vector<std::string> names;
   Value cur = formals;
   while (is_cons(cur))
      {
      if (is_symbol(car(cur)))
         names.push_back(symbol_name(as_symbol_id(car(cur))));
      cur = cdr(cur);
      }
   if (is_symbol(cur))
      names.push_back(symbol_name(as_symbol_id(cur)));
   return names;
   }

static std::vector<std::string> collect_let_bound_names(Value bindings)
   {
   std::vector<std::string> names;
   Value cur = bindings;
   while (is_cons(cur))
      {
      Value pair = car(cur);
      if (is_cons(pair) && is_symbol(car(pair)))
         names.push_back(symbol_name(as_symbol_id(car(pair))));
      cur = cdr(cur);
      }
   return names;
   }

static Value rrif_lambda(Value form, const RenameTable& table)
   {
   if (!is_cons(cdr(form))) return rename_refs(form, table);
   Value formals   = car(cdr(form));
   Value body_cons = cdr(cdr(form));
   auto  inner     = mask_table(table, collect_formals_names(formals));
   Value new_body  = rename_refs(body_cons, inner);
   
   return cons2(car(form), cons2(formals, new_body));
   }

static Value rrif_case_lambda(Value form, const RenameTable& table);

static Value rrif_let(Value form, const RenameTable& table, std::string_view kind)
   {
   Value head = car(form);
   if (!is_cons(cdr(form))) return rename_refs(form, table);
   Value bindings, body_cons;
   Value named_sym = make_nil();
   if (kind == "let" && is_symbol(car(cdr(form))) && is_cons(cdr(cdr(form))))
      {
      named_sym = car(cdr(form));
      bindings  = car(cdr(cdr(form)));
      body_cons = cdr(cdr(cdr(form)));
      }
   else
      {
      bindings  = car(cdr(form));
      body_cons = cdr(cdr(form));
      }
   auto bound      = collect_let_bound_names(bindings);
   auto body_table = mask_table(table, bound);

   // Rename init exprs
   auto rename_pair_parallel = [&](Value pair) -> Value {
      if (!is_cons(pair) || !is_cons(cdr(pair))) return pair;
      Value new_init = rename_refs(car(cdr(pair)), table);
      if (new_init.repr == car(cdr(pair)).repr) return pair;
      return cons2(car(pair), cons2(new_init, cdr(cdr(pair))));
   };
   auto rename_pair_body = [&](Value pair) -> Value {
      if (!is_cons(pair) || !is_cons(cdr(pair))) return pair;
      Value new_init = rename_refs(car(cdr(pair)), body_table);
      if (new_init.repr == car(cdr(pair)).repr) return pair;
      return cons2(car(pair), cons2(new_init, cdr(cdr(pair))));
   };
   // let*: progressive masking
   RenameTable progressive = table;

   Value new_bindings;
   if (kind == "let")
      {
      // rebuild binding list with renamed inits
      Value out = make_nil();
      std::vector<Value> pairs;
      Value c = bindings;
      while (is_cons(c)) { pairs.push_back(car(c)); c = cdr(c); }
      for (int i = (int)pairs.size() - 1; i >= 0; --i)
         out = cons2(rename_pair_parallel(pairs[i]), out);
      new_bindings = out;
      }
   else if (kind == "let*")
      {
      Value out = make_nil();
      std::vector<Value> pairs;
      Value c = bindings;
      while (is_cons(c)) { pairs.push_back(car(c)); c = cdr(c); }
      // Must process forward for progressive masking
      std::vector<Value> new_pairs;
      for (auto& pair : pairs)
         {
         if (!is_cons(pair) || !is_symbol(car(pair)) || !is_cons(cdr(pair)))
            {
            new_pairs.push_back(pair);
            continue;
            }
         std::string name = symbol_name(as_symbol_id(car(pair)));
         Value new_init = rename_refs(car(cdr(pair)), progressive);
         progressive.erase(name);
         if (new_init.repr == car(cdr(pair)).repr)
            new_pairs.push_back(pair);
         else
            new_pairs.push_back(cons2(car(pair), cons2(new_init, cdr(cdr(pair)))));
         }
      for (int i = (int)new_pairs.size() - 1; i >= 0; --i)
         out = cons2(new_pairs[i], out);
      new_bindings = out;
      }
   else
      {
      // letrec / letrec*: inits see body_table
      Value out = make_nil();
      std::vector<Value> pairs;
      Value c = bindings;
      while (is_cons(c)) { pairs.push_back(car(c)); c = cdr(c); }
      for (int i = (int)pairs.size() - 1; i >= 0; --i)
         out = cons2(rename_pair_body(pairs[i]), out);
      new_bindings = out;
      }
   Value new_body = rename_refs(body_cons, body_table);
   bool bindings_changed = (new_bindings.repr != bindings.repr);
   bool body_changed     = (new_body.repr     != body_cons.repr);
   if (!bindings_changed && !body_changed)
      return form;
   if (!is_nil(named_sym))
      {
      // named let: also mask loop name from body
      std::string loop_name = symbol_name(as_symbol_id(named_sym));
      auto loop_table = body_table;
      loop_table.erase(loop_name);
      new_body = rename_refs(body_cons, loop_table);
      return cons2(head, cons2(named_sym, cons2(new_bindings, new_body)));
      }
   return cons2(head, cons2(new_bindings, new_body));
   }

static Value rrif_case(Value form, const RenameTable& table)
   {
   if (!is_cons(cdr(form))) return rename_refs(form, table);
   Value new_key = rename_refs(car(cdr(form)), table);
   // rename clause bodies; datum lists are literal
   Value clauses = cdr(cdr(form));
   std::vector<Value> new_clauses_vec;
   bool changed = (new_key.repr != car(cdr(form)).repr);
   Value c = clauses;
   while (is_cons(c))
      {
      Value clause = car(c);
      if (!is_cons(clause))
         {
         new_clauses_vec.push_back(clause);
         c = cdr(c);
         continue;
         }
      Value h    = car(clause);
      Value body = cdr(clause);
      // symbol head (else) is renamed; list head is literal datum
      Value new_h    = is_symbol(h) ? rename_refs(h, table) : h;
      Value new_body = rename_refs(body, table);
      if (new_h.repr == h.repr && new_body.repr == body.repr)
         new_clauses_vec.push_back(clause);
      else
         {
         new_clauses_vec.push_back(cons2(new_h, new_body));
         changed = true;
         }
      c = cdr(c);
      }
   if (!changed) return form;
   Value new_clauses = make_nil();
   for (int i = (int)new_clauses_vec.size() - 1; i >= 0; --i)
      new_clauses = cons2(new_clauses_vec[i], new_clauses);
   return cons2(car(form), cons2(new_key, new_clauses));
   }

static Value rrif_case_lambda(Value form, const RenameTable& table)
   {
   std::vector<Value> new_clauses;
   bool changed = false;
   Value c = cdr(form);
   while (is_cons(c))
      {
      Value clause = car(c);
      if (!is_cons(clause))
         {
         new_clauses.push_back(clause);
         c = cdr(c);
         continue;
         }
      Value formals   = car(clause);
      Value body_cons = cdr(clause);
      auto  inner     = mask_table(table, collect_formals_names(formals));
      Value new_body  = rename_refs(body_cons, inner);
      if (false)
         new_clauses.push_back(clause);
      else
         {
         new_clauses.push_back(cons2(formals, new_body));
         changed = true;
         }
      c = cdr(c);
      }
   if (!changed) return form;
   Value new_cdr = make_nil();
   for (int i = (int)new_clauses.size() - 1; i >= 0; --i)
      new_cdr = cons2(new_clauses[i], new_cdr);
   return cons2(car(form), new_cdr);
   }

static Value rename_refs(Value form, const RenameTable& table)
   {
   if (table.empty()) return form;
   if (is_symbol(form))
      {
      std::string name = symbol_name(as_symbol_id(form));
      auto it = table.find(name);
      if (it != table.end())
         return make_symbol(it->second);
      return form;
      }
   if (!is_cons(form)) return form;
   Value head = car(form);
   if (is_symbol(head))
      {
      std::string hname = symbol_name(as_symbol_id(head));
      if (hname == "quote")                              return form;
      if (hname == "lambda")                             return rrif_lambda(form, table);
      if (hname == "let" || hname == "let*" ||
          hname == "letrec" || hname == "letrec*")       return rrif_let(form, table, hname);
      if (hname == "case-lambda")                        return rrif_case_lambda(form, table);
      if (hname == "case")                               return rrif_case(form, table);
      }
   Value new_head = rename_refs(head,     table);
   Value new_cdr  = rename_refs(cdr(form), table);
   if (new_head.repr == head.repr && new_cdr.repr == cdr(form).repr)
      return form;
   return cons2(new_head, new_cdr);
   }

// ── expand_list ───────────────────────────────────────────────────────────────

static Value expand_list(Value cell, Environment*& env_ref, int& depth)
   {
   std::vector<Value> items;
   Value cur = cell;
   while (is_cons(cur))
      {
      items.push_back(expand_impl(car(cur), env_ref, depth));
      cur = cdr(cur);
      }
   Value tail = is_nil(cur) ? make_nil() : expand_impl(cur, env_ref, depth);
   Value result = tail;
   for (int i = (int)items.size() - 1; i >= 0; --i)
      result = cons2(items[i], result);
   return result;
   }

// ── prescan_syntax / collect_body_forms / expand_body ─────────────────────────

static void prescan_syntax(std::vector<Value>& raw_forms,
                           Environment*& env_ref, int& depth);

static std::vector<Value> collect_body_forms(Value body_cons,
                                              Environment*& env_ref, int& depth)
   {
   std::vector<Value> raw_forms;
   Value cur = body_cons;
   while (is_cons(cur))
      {
      raw_forms.push_back(car(cur));
      cur = cdr(cur);
      }
   prescan_syntax(raw_forms, env_ref, depth);
   std::vector<Value> out;
   for (auto& raw : raw_forms)
      {
      if (is_head_sym(raw, "define-syntax"))
         continue;
      if (is_head_sym(raw, "define-values"))
         {
         out.push_back(raw);
         continue;
         }
      Value form = expand_impl(raw, env_ref, depth);
      if (is_head_sym(form, "begin"))
         {
         auto sub = collect_body_forms(cdr(form), env_ref, depth);
         for (auto& s : sub)
            out.push_back(s);
         }
      else
         out.push_back(form);
      }
   return out;
   }

static void prescan_syntax(std::vector<Value>& raw_forms,
                           Environment*& env_ref, int& depth)
   {
   for (auto& raw : raw_forms)
      {
      if (is_head_sym(raw, "define-syntax"))
         {
         // will be processed by expand_impl -> define-syntax handler
         expand_impl(raw, env_ref, depth);
         }
      else if (is_head_sym(raw, "begin"))
         {
         std::vector<Value> sub;
         Value c = cdr(raw);
         while (is_cons(c)) { sub.push_back(car(c)); c = cdr(c); }
         prescan_syntax(sub, env_ref, depth);
         }
      else if (is_cons(raw) && is_symbol(car(raw)))
         {
         // Check if head is a macro; expand once and recurse on begin/define-syntax
         Value head = car(raw);
         if (env_ref)
            {
            auto opt = env_ref->lookup_optional_id(as_symbol_id(head));
            if (opt && is_syntax_transformer(*opt))
               {
               try
                  {
                  Value expanded = expand_impl(raw, env_ref, depth);
                  if (is_head_sym(expanded, "begin"))
                     {
                     std::vector<Value> sub;
                     Value c = cdr(expanded);
                     while (is_cons(c)) { sub.push_back(car(c)); c = cdr(c); }
                     prescan_syntax(sub, env_ref, depth);
                     }
                  else if (is_head_sym(expanded, "define-syntax"))
                     expand_impl(expanded, env_ref, depth);
                  }
               catch (...) {}
               }
            }
         }
      }
   }

// define-values setter helper
static Value dv_build_setter(const std::vector<std::string>& fixed,
                             const std::string& rest_name,  // empty = no rest
                             Value expanded_expr)
   {
   std::vector<Value> tmp_fixed;
   for (size_t i = 0; i < fixed.size(); ++i)
      tmp_fixed.push_back(make_symbol(hygiene_gensym("mv-tmp")));
   Value tmp_rest = is_nil(make_nil()) ? make_nil() :
      (rest_name.empty() ? make_nil() : make_symbol(hygiene_gensym("mv-tmp-rest")));
   bool has_rest = !rest_name.empty();
   if (has_rest)
      tmp_rest = make_symbol(hygiene_gensym("mv-tmp-rest"));

   std::vector<Value> body_items;
   Value set_sym = make_symbol("set!");
   for (size_t i = 0; i < fixed.size(); ++i)
      body_items.push_back(make_list({set_sym, make_symbol(fixed[i]), tmp_fixed[i]}));
   if (has_rest)
      body_items.push_back(make_list({set_sym, make_symbol(rest_name), tmp_rest}));

   // Consumer formals
   Value consumer_formals;
   if (!has_rest)
      consumer_formals = make_list(tmp_fixed);
   else if (tmp_fixed.empty())
      consumer_formals = tmp_rest;
   else
      {
      consumer_formals = tmp_rest;
      for (int i = (int)tmp_fixed.size() - 1; i >= 0; --i)
         consumer_formals = cons2(tmp_fixed[i], consumer_formals);
      }
   // (lambda consumer_formals body...)
   std::vector<Value> lam_items = {make_symbol("lambda"), consumer_formals};
   for (auto& b : body_items) lam_items.push_back(b);
   Value consumer = make_list(lam_items);
   // (lambda () init)
   Value producer = make_list({make_symbol("lambda"), make_nil(), expanded_expr});
   return make_list({make_symbol("call-with-values"), producer, consumer});
   }

static Value expand_body(Value body_cons, Environment*& env_ref, int& depth)
   {
   Environment* saved_env = env_ref;
   Environment* local_env = nullptr;
   (void)local_env;

   auto forms = collect_body_forms(body_cons, env_ref, depth);

   // Restore env
   env_ref = saved_env;

   // Collect leading define / define-values
   std::vector<std::tuple<Value,Value>> bindings;  // (name_sym, init_val)
   std::vector<Value> body_prefix;
   size_t i = 0;
   while (i < forms.size())
      {
      Value f = forms[i];
      if (is_head_sym(f, "define"))
         {
         // canonical (define sym init)
         if (!is_cons(cdr(f)) || !is_symbol(car(cdr(f))) ||
             !is_cons(cdr(cdr(f))) || !is_nil(cdr(cdr(cdr(f)))))
            break;
         bindings.emplace_back(car(cdr(f)), car(cdr(cdr(f))));
         ++i;
         continue;
         }
      if (is_head_sym(f, "define-values"))
         {
         if (!is_cons(cdr(f)) || !is_cons(cdr(cdr(f))) ||
             !is_nil(cdr(cdr(cdr(f)))))
            break;
         Value formals_sexpr = car(cdr(f));
         Value expr          = car(cdr(cdr(f)));
         std::vector<std::string> fixed_names;
         std::string rest_name;
         Value cur = formals_sexpr;
         bool ok = true;
         if (is_symbol(cur))
            rest_name = symbol_name(as_symbol_id(cur));
         else
            {
            while (is_cons(cur))
               {
               if (!is_symbol(car(cur))) { ok = false; break; }
               fixed_names.push_back(symbol_name(as_symbol_id(car(cur))));
               cur = cdr(cur);
               }
            if (is_symbol(cur))
               rest_name = symbol_name(as_symbol_id(cur));
            else if (!is_nil(cur))
               ok = false;
            }
         if (!ok) break;
         for (auto& n : fixed_names)
            bindings.emplace_back(make_symbol(n), make_unspecified());
         if (!rest_name.empty())
            bindings.emplace_back(make_symbol(rest_name), make_unspecified());
         body_prefix.push_back(dv_build_setter(fixed_names, rest_name,
                                               expand_impl(expr, env_ref, depth)));
         ++i;
         continue;
         }
      break;
      }
   if (bindings.empty())
      {
      Value result = make_nil();
      for (int j = (int)forms.size() - 1; j >= 0; --j)
         result = cons2(forms[j], result);
      return result;
      }

   // Build gensym table (per-application intro scope)
   std::unordered_map<std::string,int> name_counts;
   for (auto& [sym, _] : bindings)
      {
      std::string n = symbol_name(as_symbol_id(sym));
      name_counts[n]++;
      }
   std::vector<std::string> gensym_names;
   for (auto& [sym, _] : bindings)
      gensym_names.push_back(hygiene_gensym(symbol_name(as_symbol_id(sym))));
   RenameTable rename_table;
   for (size_t j = 0; j < bindings.size(); ++j)
      {
      std::string n = symbol_name(as_symbol_id(std::get<0>(bindings[j])));
      if (name_counts[n] == 1)
         rename_table[n] = gensym_names[j];
      }

   // Build bindings chain (letrec*)
   std::vector<Value> rest_forms;
   for (auto& bp : body_prefix) rest_forms.push_back(bp);
   for (size_t j = i; j < forms.size(); ++j) rest_forms.push_back(forms[j]);

   // Rename rest_forms
   for (auto& rf : rest_forms)
      rf = rename_refs(rf, rename_table);

   // Build ((gs init)...) as cons chain
   Value bindings_chain = make_nil();
   for (int j = (int)bindings.size() - 1; j >= 0; --j)
      {
      Value gs_sym = make_symbol(gensym_names[j]);
      Value init   = rename_refs(std::get<1>(bindings[j]), rename_table);
      Value pair   = make_list({gs_sym, init});
      bindings_chain = cons2(pair, bindings_chain);
      }
   Value rest_chain = make_nil();
   for (int j = (int)rest_forms.size() - 1; j >= 0; --j)
      rest_chain = cons2(rest_forms[j], rest_chain);

   // (letrec* bindings rest...)
   Value letrec_form = cons2(make_symbol("letrec*"),
                             cons2(bindings_chain, rest_chain));
   return cons2(letrec_form, make_nil());
   }

// ── Sugar handlers ────────────────────────────────────────────────────────────

static Value expand_define(Value sexpr, Environment*& env_ref, int& depth)
   {
   if (!is_cons(cdr(sexpr)))        return expand_list(sexpr, env_ref, depth);
   Value signature = car(cdr(sexpr));
   Value body_cons = cdr(cdr(sexpr));
   if (!is_cons(signature))         return expand_list(sexpr, env_ref, depth);
   if (is_nil(body_cons))           return expand_list(sexpr, env_ref, depth);
   Value name_sexpr  = car(signature);
   Value params_node = cdr(signature);
   Value body_chain  = expand_body(body_cons, env_ref, depth);
   Value lambda_form = cons2(make_symbol("lambda"),
                             cons2(params_node, body_chain));
   return make_list({make_symbol("define"), name_sexpr, lambda_form});
   }

static Value expand_lambda(Value sexpr, Environment*& env_ref, int& depth)
   {
   if (!is_cons(cdr(sexpr)))       return expand_list(sexpr, env_ref, depth);
   Value formals = car(cdr(sexpr));
   Value body    = cdr(cdr(sexpr));
   if (is_nil(body))               return expand_list(sexpr, env_ref, depth);
   RenameTable table;
   Value new_formals  = gensym_rename_formals(formals, table);
   Value renamed_body = rename_refs(body, table);
   Value expanded_body = expand_body(renamed_body, env_ref, depth);
   return cons2(make_symbol("lambda"),
                cons2(new_formals, expanded_body));
   }

static Value expand_let_family(Value sexpr, std::string_view head_name,
                                Environment*& env_ref, int& depth)
   {
   if (!is_cons(cdr(sexpr)))       return expand_list(sexpr, env_ref, depth);
   Value first = car(cdr(sexpr));
   Value rest  = cdr(cdr(sexpr));
   // Named let: (let <name> <bindings> <body>...)
   Value named_sym = make_nil();
   Value bindings_form, body_cons;
   if (head_name == "let" && is_symbol(first) && is_cons(rest))
      {
      named_sym     = first;
      bindings_form = car(rest);
      body_cons     = cdr(rest);
      }
   else
      {
      bindings_form = first;
      body_cons     = rest;
      }
   if (!is_cons(bindings_form) && !is_nil(bindings_form))
      return expand_list(sexpr, env_ref, depth);
   if (is_nil(body_cons))
      return expand_list(sexpr, env_ref, depth);

   // Collect raw (name, init, pair_src) triples
   struct RawPair { std::string name; Value init; };
   std::vector<RawPair> raw_pairs;
   Value cur = bindings_form;
   while (is_cons(cur))
      {
      Value pair = car(cur);
      if (!is_cons(pair) || !is_symbol(car(pair)) ||
          !is_cons(cdr(pair)) || !is_nil(cdr(cdr(pair))))
         return expand_list(sexpr, env_ref, depth);
      raw_pairs.push_back({symbol_name(as_symbol_id(car(pair))), car(cdr(pair))});
      cur = cdr(cur);
      }
   if (!is_nil(cur)) return expand_list(sexpr, env_ref, depth);

   // Duplicate check (not for let*)
   if (head_name != "let*")
      {
      std::vector<std::string> seen;
      for (auto& p : raw_pairs)
         {
         for (auto& s : seen)
            if (s == p.name)
               throw SchemeSyntaxError("duplicate variable name in " +
                                       std::string(head_name) + " bindings: " + p.name);
         seen.push_back(p.name);
         }
      }

   // Generate gensyms
   RenameTable rename_table;
   for (auto& p : raw_pairs)
      rename_table[p.name] = hygiene_gensym(p.name);
   std::string loop_gs;
   Value new_named_sym = make_nil();
   if (!is_nil(named_sym))
      {
      std::string loop_name = symbol_name(as_symbol_id(named_sym));
      loop_gs = hygiene_gensym(loop_name);
      rename_table[loop_name] = loop_gs;
      new_named_sym = make_symbol(loop_gs);
      }

   // Build new binding pairs
   std::vector<Value> new_pairs;
   if (head_name == "let")
      {
      for (auto& p : raw_pairs)
         {
         Value gs_sym  = make_symbol(rename_table[p.name]);
         Value new_init = expand_impl(p.init, env_ref, depth);
         new_pairs.push_back(make_list({gs_sym, new_init}));
         }
      }
   else if (head_name == "let*")
      {
      RenameTable progressive;
      for (auto& p : raw_pairs)
         {
         Value renamed_init = rename_refs(p.init, progressive);
         Value new_init     = expand_impl(renamed_init, env_ref, depth);
         Value gs_sym       = make_symbol(rename_table[p.name]);
         new_pairs.push_back(make_list({gs_sym, new_init}));
         progressive[p.name] = rename_table[p.name];
         }
      }
   else  // letrec / letrec*
      {
      for (auto& p : raw_pairs)
         {
         Value renamed_init = rename_refs(p.init, rename_table);
         Value new_init     = expand_impl(renamed_init, env_ref, depth);
         Value gs_sym       = make_symbol(rename_table[p.name]);
         new_pairs.push_back(make_list({gs_sym, new_init}));
         }
      }

   // Build bindings cons chain
   Value new_bindings = make_nil();
   for (int i = (int)new_pairs.size() - 1; i >= 0; --i)
      new_bindings = cons2(new_pairs[i], new_bindings);

   // Rename body refs and expand
   Value renamed_body  = rename_refs(body_cons, rename_table);
   Value expanded_body = expand_body(renamed_body, env_ref, depth);

   Value tail;
   if (!is_nil(new_named_sym))
      tail = cons2(new_named_sym, cons2(new_bindings, expanded_body));
   else
      tail = cons2(new_bindings, expanded_body);
   return cons2(make_symbol(std::string(head_name)), tail);
   }

static Value expand_if(Value sexpr, Environment*& env_ref, int& depth)
   {
   if (list_length(sexpr) != 3) return expand_list(sexpr, env_ref, depth);
   Value test_v    = car(cdr(sexpr));
   Value then_v    = car(cdr(cdr(sexpr)));
   return make_list({make_symbol("if"),
                     expand_impl(test_v, env_ref, depth),
                     expand_impl(then_v, env_ref, depth),
                     make_unspecified()});
   }

static Value expand_when_unless(Value sexpr, std::string_view name,
                                 Environment*& env_ref, int& depth)
   {
   if (!is_cons(cdr(sexpr))) return expand_list(sexpr, env_ref, depth);
   Value test_v = car(cdr(sexpr));
   Value body   = cdr(cdr(sexpr));
   if (is_nil(body)) return expand_list(sexpr, env_ref, depth);
   Value expanded_body = expand_body(body, env_ref, depth);
   return cons2(make_symbol(std::string(name)),
                cons2(expand_impl(test_v, env_ref, depth), expanded_body));
   }

static Value expand_case_lambda(Value sexpr, Environment*& env_ref, int& depth)
   {
   if (!is_cons(cdr(sexpr))) return expand_list(sexpr, env_ref, depth);
   std::vector<Value> expanded_clauses;
   Value cur = cdr(sexpr);
   while (is_cons(cur))
      {
      Value clause = car(cur);
      if (!is_cons(clause) || !is_cons(cdr(clause)))
         return expand_list(sexpr, env_ref, depth);
      Value formals = car(clause);
      Value body    = cdr(clause);
      RenameTable table;
      Value new_formals  = gensym_rename_formals(formals, table);
      Value renamed_body = rename_refs(body, table);
      Value expanded_body = expand_body(renamed_body, env_ref, depth);
      expanded_clauses.push_back(cons2(new_formals, expanded_body));
      cur = cdr(cur);
      }
   if (!is_nil(cur)) return expand_list(sexpr, env_ref, depth);
   Value tail = make_nil();
   for (int i = (int)expanded_clauses.size() - 1; i >= 0; --i)
      tail = cons2(expanded_clauses[i], tail);
   return cons2(make_symbol("case-lambda"), tail);
   }

static Value expand_do(Value sexpr, Environment*& env_ref, int& depth)
   {
   if (list_length(sexpr) < 3) return expand_list(sexpr, env_ref, depth);
   Value bindings_cons = car(cdr(sexpr));
   Value test_cons     = car(cdr(cdr(sexpr)));
   Value body_cons     = cdr(cdr(cdr(sexpr)));
   if (list_length(bindings_cons) < 0) return expand_list(sexpr, env_ref, depth);
   if (!is_cons(test_cons))            return expand_list(sexpr, env_ref, depth);
   if (list_length(test_cons) < 1)     return expand_list(sexpr, env_ref, depth);

   std::vector<Value> vars_list, inits_list, steps_list;
   Value cur = bindings_cons;
   while (is_cons(cur))
      {
      Value b = car(cur);
      int blen = list_length(b);
      if (blen != 2 && blen != 3) return expand_list(sexpr, env_ref, depth);
      if (!is_symbol(car(b)))      return expand_list(sexpr, env_ref, depth);
      vars_list.push_back(car(b));
      inits_list.push_back(expand_impl(car(cdr(b)), env_ref, depth));
      if (blen == 3)
         steps_list.push_back(expand_impl(car(cdr(cdr(b))), env_ref, depth));
      else
         steps_list.push_back(make_nil());  // sentinel: nil = use var itself
      cur = cdr(cur);
      }
   // Duplicate guard
   {
      std::vector<std::string> seen;
      for (auto& v : vars_list)
         {
         std::string n = symbol_name(as_symbol_id(v));
         for (auto& s : seen)
            if (s == n) return expand_list(sexpr, env_ref, depth);
         seen.push_back(n);
         }
   }
   Value test_expr = expand_impl(car(test_cons), env_ref, depth);
   Value loop_name = make_symbol(hygiene_gensym("do-loop"));

   // Recursive call
   std::vector<Value> call_items = {loop_name};
   for (size_t j = 0; j < vars_list.size(); ++j)
      {
      if (is_nil(steps_list[j]))  // no step — pass var forward
         call_items.push_back(vars_list[j]);
      else
         call_items.push_back(steps_list[j]);
      }
   Value recur_call = make_list(call_items);

   // Body form
   std::vector<Value> body_items;
   Value bc = body_cons;
   while (is_cons(bc)) { body_items.push_back(expand_impl(car(bc), env_ref, depth)); bc = cdr(bc); }
   Value body_form;
   if (body_items.empty())
      body_form = recur_call;
   else
      {
      std::vector<Value> seq = {make_symbol("begin")};
      for (auto& bi : body_items) seq.push_back(bi);
      seq.push_back(recur_call);
      body_form = make_list(seq);
      }

   // Result form
   std::vector<Value> result_items;
   cur = cdr(test_cons);
   while (is_cons(cur)) { result_items.push_back(expand_impl(car(cur), env_ref, depth)); cur = cdr(cur); }
   Value result_form;
   if (result_items.empty())
      result_form = make_unspecified();
   else if (result_items.size() == 1)
      result_form = result_items[0];
   else
      {
      std::vector<Value> seq = {make_symbol("begin")};
      for (auto& ri : result_items) seq.push_back(ri);
      result_form = make_list(seq);
      }

   Value if_form = make_list({make_symbol("if"), test_expr, result_form, body_form});

   // Build binding pairs
   std::vector<Value> binding_forms;
   for (size_t j = 0; j < vars_list.size(); ++j)
      binding_forms.push_back(make_list({vars_list[j], inits_list[j]}));
   Value let_bindings = make_list(binding_forms);
   return make_list({make_symbol("let"), loop_name, let_bindings, if_form});
   }

// ── quasiquote ────────────────────────────────────────────────────────────────

static Value qq_quote(Value x)  { return make_list({make_symbol("quote"), x}); }
static Value qq_cons(Value a, Value b) { return make_list({make_symbol("cons"), a, b}); }
static Value qq_append(Value a, Value b) { return make_list({make_symbol("append"), a, b}); }
static Value qq_list2(Value a, Value b) { return make_list({make_symbol("list"), a, b}); }

static Value qq_walk(Value x, int level, Environment*& env_ref, int& depth)
   {
   if (is_cons(x))
      {
      Value head = car(x);
      if (is_symbol(head))
         {
         std::string hname = symbol_name(as_symbol_id(head));
         if (hname == "unquote")
            {
            if (!is_cons(cdr(x)) || !is_nil(cdr(cdr(x))))
               throw SchemeSyntaxError("unquote requires exactly one argument");
            Value e = car(cdr(x));
            if (level == 1)
               return expand_impl(e, env_ref, depth);
            return qq_list2(qq_quote(make_symbol("unquote")),
                            qq_walk(e, level - 1, env_ref, depth));
            }
         if (hname == "quasiquote")
            {
            if (!is_cons(cdr(x)) || !is_nil(cdr(cdr(x))))
               throw SchemeSyntaxError("quasiquote requires exactly one argument");
            Value e = car(cdr(x));
            return qq_list2(qq_quote(make_symbol("quasiquote")),
                            qq_walk(e, level + 1, env_ref, depth));
            }
         if (hname == "unquote-splicing")
            throw SchemeSyntaxError("unquote-splicing must appear inside a list");
         }
      // Splicing: x.car is (unquote-splicing e)
      if (is_cons(head) && is_symbol(car(head)) &&
          symbol_name(as_symbol_id(car(head))) == "unquote-splicing")
         {
         if (!is_cons(cdr(head)) || !is_nil(cdr(cdr(head))))
            throw SchemeSyntaxError("unquote-splicing requires exactly one argument");
         Value e    = car(cdr(head));
         Value tail = qq_walk(cdr(x), level, env_ref, depth);
         if (level == 1)
            return qq_append(expand_impl(e, env_ref, depth), tail);
         Value spliced = qq_list2(qq_quote(make_symbol("unquote-splicing")),
                                  qq_walk(e, level - 1, env_ref, depth));
         return qq_cons(spliced, tail);
         }
      return qq_cons(qq_walk(car(x), level, env_ref, depth),
                     qq_walk(cdr(x), level, env_ref, depth));
      }
   // Vector template
   if (is_vector(x))
      {
      const auto& items_ref = as_vector(x)->elements;
      std::vector<Value> items(items_ref.begin(), items_ref.end());
      Value list_chain = make_list(items);
      Value list_expr  = qq_walk(list_chain, level, env_ref, depth);
      return make_list({make_symbol("list->vector"), list_expr});
      }
   return qq_quote(x);
   }

static Value expand_quasiquote(Value sexpr, Environment*& env_ref, int& depth)
   {
   if (!is_cons(cdr(sexpr)) || !is_nil(cdr(cdr(sexpr))))
      return expand_list(sexpr, env_ref, depth);
   return qq_walk(car(cdr(sexpr)), 1, env_ref, depth);
   }

// ── multi-value forms ─────────────────────────────────────────────────────────

static std::pair<std::vector<std::string>,std::string>
mv_collect_formals(Value formals_sexpr)
   {
   // Returns ({fixed}, rest) or ({}, "@error") on shape error
   std::vector<std::string> fixed;
   if (is_symbol(formals_sexpr))
      return {fixed, symbol_name(as_symbol_id(formals_sexpr))};
   Value cur = formals_sexpr;
   while (is_cons(cur))
      {
      if (!is_symbol(car(cur))) return {{}, "@error"};
      fixed.push_back(symbol_name(as_symbol_id(car(cur))));
      cur = cdr(cur);
      }
   if (is_nil(cur)) return {fixed, ""};
   if (is_symbol(cur)) return {fixed, symbol_name(as_symbol_id(cur))};
   return {{}, "@error"};
   }

static Value mv_thunk(Value expanded_init)
   { return make_list({make_symbol("lambda"), make_nil(), expanded_init}); }

static Value mv_lambda(Value formals_sexpr, const std::vector<Value>& body)
   {
   std::vector<Value> items = {make_symbol("lambda"), formals_sexpr};
   for (auto& b : body) items.push_back(b);
   return make_list(items);
   }

static Value mv_cwv(Value producer, Value consumer)
   { return make_list({make_symbol("call-with-values"), producer, consumer}); }

static Value expand_let_values(Value sexpr, Environment*& env_ref, int& depth)
   {
   if (list_length(sexpr) < 3) return expand_list(sexpr, env_ref, depth);
   Value bindings_cons = car(cdr(sexpr));
   Value body_cons     = cdr(cdr(sexpr));
   if (list_length(bindings_cons) < 0) return expand_list(sexpr, env_ref, depth);
   struct Clause { Value formals; Value expanded_init; };
   std::vector<Clause> clauses;
   Value cur = bindings_cons;
   while (is_cons(cur))
      {
      Value b = car(cur);
      if (list_length(b) != 2) return expand_list(sexpr, env_ref, depth);
      clauses.push_back({car(b), expand_impl(car(cdr(b)), env_ref, depth)});
      cur = cdr(cur);
      }
   std::vector<Value> body_items;
   Value bc = expand_body(body_cons, env_ref, depth);
   while (is_cons(bc)) { body_items.push_back(car(bc)); bc = cdr(bc); }
   if (body_items.empty()) return expand_list(sexpr, env_ref, depth);
   if (clauses.empty())
      {
      std::vector<Value> items = {make_symbol("begin")};
      for (auto& b : body_items) items.push_back(b);
      return make_list(items);
      }
   // Outer let: bind tmp = (cwv (lambda () init) list)
   std::vector<Value> outer_bindings, tmp_syms;
   for (auto& cl : clauses)
      {
      Value tmp_sym = make_symbol(hygiene_gensym("mv"));
      tmp_syms.push_back(tmp_sym);
      Value producer = mv_thunk(cl.expanded_init);
      Value cwv = mv_cwv(producer, make_symbol("list"));
      outer_bindings.push_back(make_list({tmp_sym, cwv}));
      }
   Value outer_bindings_cons = make_list(outer_bindings);
   // Inner let: destructure each tmp
   std::vector<Value> inner_bindings;
   for (size_t j = 0; j < clauses.size(); ++j)
      {
      auto [fixed, rest] = mv_collect_formals(clauses[j].formals);
      if (rest == "@error") return expand_list(sexpr, env_ref, depth);
      Value tmp_sym = tmp_syms[j];
      // Build car/cdr chains
      Value chain = tmp_sym;
      for (size_t k = 0; k < fixed.size(); ++k)
         {
         // nth ref: (car (cdr^k tmp))
         Value ref = tmp_sym;
         for (size_t q = 0; q < k; ++q)
            ref = make_list({make_symbol("cdr"), ref});
         ref = make_list({make_symbol("car"), ref});
         inner_bindings.push_back(make_list({make_symbol(fixed[k]), ref}));
         (void)chain;
         }
      if (!rest.empty())
         {
         Value ref = tmp_sym;
         for (size_t q = 0; q < fixed.size(); ++q)
            ref = make_list({make_symbol("cdr"), ref});
         inner_bindings.push_back(make_list({make_symbol(rest), ref}));
         }
      }
   Value let_sym = make_symbol("let");
   Value inner_bindings_cons = inner_bindings.empty() ? make_nil() : make_list(inner_bindings);
   std::vector<Value> inner_items = {let_sym, inner_bindings_cons};
   for (auto& b : body_items) inner_items.push_back(b);
   Value inner_let = make_list(inner_items);
   return make_list({let_sym, outer_bindings_cons, inner_let});
   }

static Value expand_let_star_values(Value sexpr, Environment*& env_ref, int& depth)
   {
   if (list_length(sexpr) < 3) return expand_list(sexpr, env_ref, depth);
   Value bindings_cons = car(cdr(sexpr));
   Value body_cons     = cdr(cdr(sexpr));
   if (list_length(bindings_cons) < 0) return expand_list(sexpr, env_ref, depth);
   struct Clause { Value formals; Value expanded_init; };
   std::vector<Clause> clauses;
   Value cur = bindings_cons;
   while (is_cons(cur))
      {
      Value b = car(cur);
      if (list_length(b) != 2) return expand_list(sexpr, env_ref, depth);
      clauses.push_back({car(b), expand_impl(car(cdr(b)), env_ref, depth)});
      cur = cdr(cur);
      }
   std::vector<Value> body_items;
   Value bc = expand_body(body_cons, env_ref, depth);
   while (is_cons(bc)) { body_items.push_back(car(bc)); bc = cdr(bc); }
   if (body_items.empty()) return expand_list(sexpr, env_ref, depth);
   if (clauses.empty())
      {
      std::vector<Value> items = {make_symbol("begin")};
      for (auto& b : body_items) items.push_back(b);
      return make_list(items);
      }
   // Build nested cwv
   Value inner = mv_lambda(clauses.back().formals, body_items);
   Value result = mv_cwv(mv_thunk(clauses.back().expanded_init), inner);
   for (int i = (int)clauses.size() - 2; i >= 0; --i)
      {
      Value consumer = mv_lambda(clauses[i].formals, {result});
      result = mv_cwv(mv_thunk(clauses[i].expanded_init), consumer);
      }
   return result;
   }

static Value expand_define_values(Value sexpr, Environment*& env_ref, int& depth)
   {
   if (list_length(sexpr) != 3) return expand_list(sexpr, env_ref, depth);
   Value formals_sexpr = car(cdr(sexpr));
   Value expr          = car(cdr(cdr(sexpr)));
   auto [fixed, rest]  = mv_collect_formals(formals_sexpr);
   if (rest == "@error") return expand_list(sexpr, env_ref, depth);
   std::vector<Value> items = {make_symbol("begin")};
   Value define_sym = make_symbol("define");
   for (auto& n : fixed)
      items.push_back(make_list({define_sym, make_symbol(n), make_unspecified()}));
   if (!rest.empty())
      items.push_back(make_list({define_sym, make_symbol(rest), make_unspecified()}));
   items.push_back(dv_build_setter(fixed, rest, expand_impl(expr, env_ref, depth)));
   return make_list(items);
   }

static Value expand_guard(Value sexpr, Environment*& env_ref, int& depth)
   {
   if (list_length(sexpr) < 3) return expand_list(sexpr, env_ref, depth);
   Value guard_spec = car(cdr(sexpr));
   Value body_cons  = cdr(cdr(sexpr));
   if (list_length(guard_spec) < 1) return expand_list(sexpr, env_ref, depth);
   Value var_sexpr = car(guard_spec);
   if (!is_symbol(var_sexpr)) return expand_list(sexpr, env_ref, depth);
   Value clauses_cons = cdr(guard_spec);
   if (list_length(clauses_cons) < 0) return expand_list(sexpr, env_ref, depth);
   // Check for else clause
   bool has_else = false;
   Value last_clause = make_nil();
   Value cur = clauses_cons;
   while (is_cons(cur)) { last_clause = car(cur); cur = cdr(cur); }
   if (is_cons(last_clause) && is_symbol(car(last_clause)) &&
       symbol_name(as_symbol_id(car(last_clause))) == "else")
      has_else = true;
   std::vector<Value> body_items;
   Value bc = expand_body(body_cons, env_ref, depth);
   while (is_cons(bc)) { body_items.push_back(car(bc)); bc = cdr(bc); }
   if (body_items.empty()) return expand_list(sexpr, env_ref, depth);
   // Build cond
   std::vector<Value> cond_items = {make_symbol("cond")};
   cur = clauses_cons;
   while (is_cons(cur)) { cond_items.push_back(expand_list(car(cur), env_ref, depth)); cur = cdr(cur); }
   if (!has_else)
      cond_items.push_back(make_list({make_symbol("else"),
                                      make_list({make_symbol("raise"), var_sexpr})}));
   Value cond_form = make_list(cond_items);
   // guard-k escape
   Value guard_k_sym  = make_symbol("%guard-k");
   Value escape_call  = make_list({guard_k_sym, cond_form});
   Value handler      = make_list({make_symbol("lambda"),
                                   make_list({var_sexpr}), escape_call});
   std::vector<Value> thunk_items = {make_symbol("lambda"), make_nil()};
   for (auto& b : body_items) thunk_items.push_back(b);
   Value thunk = make_list(thunk_items);
   Value weh   = make_list({make_symbol("with-exception-handler"), handler, thunk});
   Value escape_lambda = make_list({make_symbol("lambda"),
                                    make_list({guard_k_sym}), weh});
   return make_list({make_symbol("call/cc"), escape_lambda});
   }

static Value expand_parameterize(Value sexpr, Environment*& env_ref, int& depth)
   {
   if (list_length(sexpr) < 3) return expand_list(sexpr, env_ref, depth);
   Value bindings_cons = car(cdr(sexpr));
   Value body_cons     = cdr(cdr(sexpr));
   if (list_length(bindings_cons) < 0) return expand_list(sexpr, env_ref, depth);
   std::vector<Value> params_exprs, values_exprs;
   Value cur = bindings_cons;
   while (is_cons(cur))
      {
      Value b = car(cur);
      if (list_length(b) != 2) return expand_list(sexpr, env_ref, depth);
      params_exprs.push_back(expand_impl(car(b), env_ref, depth));
      values_exprs.push_back(expand_impl(car(cdr(b)), env_ref, depth));
      cur = cdr(cur);
      }
   std::vector<Value> body_items;
   Value bc = expand_body(body_cons, env_ref, depth);
   while (is_cons(bc)) { body_items.push_back(car(bc)); bc = cdr(bc); }
   if (body_items.empty()) return expand_list(sexpr, env_ref, depth);
   Value list_sym = make_symbol("list");
   std::vector<Value> p_items = {list_sym};
   for (auto& p : params_exprs) p_items.push_back(p);
   std::vector<Value> v_items = {list_sym};
   for (auto& v : values_exprs) v_items.push_back(v);
   std::vector<Value> lam_items = {make_symbol("lambda"), make_nil()};
   for (auto& b : body_items) lam_items.push_back(b);
   return make_list({make_symbol("%with-parameters"),
                     make_list(p_items), make_list(v_items), make_list(lam_items)});
   }

static Value expand_define_record_type(Value sexpr, Environment*& env_ref, int& depth)
   {
   (void)env_ref; (void)depth;
   if (list_length(sexpr) < 4) return expand_list(sexpr, env_ref, depth);
   Value type_name_sexpr = car(cdr(sexpr));
   Value ctor_spec       = car(cdr(cdr(sexpr)));
   Value pred_sexpr      = car(cdr(cdr(cdr(sexpr))));
   Value field_specs     = cdr(cdr(cdr(cdr(sexpr))));
   if (!is_symbol(type_name_sexpr) || !is_symbol(pred_sexpr)) return expand_list(sexpr, env_ref, depth);
   if (list_length(ctor_spec) < 1) return expand_list(sexpr, env_ref, depth);
   Value ctor_name = car(ctor_spec);
   if (!is_symbol(ctor_name)) return expand_list(sexpr, env_ref, depth);
   // ctor fields
   std::vector<Value> ctor_fields;
   Value cur = cdr(ctor_spec);
   while (is_cons(cur))
      {
      if (!is_symbol(car(cur))) return expand_list(sexpr, env_ref, depth);
      ctor_fields.push_back(car(cur));
      cur = cdr(cur);
      }
   if (!is_nil(cur)) return expand_list(sexpr, env_ref, depth);
   // field specs
   struct FieldEntry { Value fname; Value accessor; Value mutator; bool has_mutator; };
   std::vector<FieldEntry> field_entries;
   cur = field_specs;
   while (is_cons(cur))
      {
      Value spec = car(cur);
      int slen = list_length(spec);
      if (slen != 2 && slen != 3) return expand_list(sexpr, env_ref, depth);
      Value fname    = car(spec);
      Value accessor = car(cdr(spec));
      if (!is_symbol(fname) || !is_symbol(accessor)) return expand_list(sexpr, env_ref, depth);
      if (slen == 3)
         {
         Value mutator = car(cdr(cdr(spec)));
         if (!is_symbol(mutator)) return expand_list(sexpr, env_ref, depth);
         field_entries.push_back({fname, accessor, mutator, true});
         }
      else
         field_entries.push_back({fname, accessor, make_nil(), false});
      cur = cdr(cur);
      }
   if (!is_nil(cur)) return expand_list(sexpr, env_ref, depth);
   std::string type_name = symbol_name(as_symbol_id(type_name_sexpr));
   // Hidden gensym for the record-type descriptor
   std::string rt_sym_name = hygiene_gensym("record-type-" + type_name);
   Value rt_sym = make_symbol(rt_sym_name);
   auto mk_sym = [](const std::string& n) { return make_symbol(n); };
   auto mk_define = [&](Value name_sexpr, Value value_sexpr) {
      return make_list({mk_sym("define"), name_sexpr, value_sexpr});
   };
   auto mk_quote_val = [&](Value datum) {
      return make_list({mk_sym("quote"), datum});
   };
   std::vector<Value> forms = {mk_sym("begin")};
   // All field names
   std::vector<Value> all_field_names;
   for (auto& fe : field_entries) all_field_names.push_back(fe.fname);
   // (%make-record-type 'name '(fields...))
   Value fields_quoted = mk_quote_val(make_list(all_field_names));
   Value rt_init = make_list({mk_sym("%make-record-type"),
                              mk_quote_val(type_name_sexpr), fields_quoted});
   forms.push_back(mk_define(rt_sym, rt_init));
   // Constructor
   std::unordered_map<std::string,bool> ctor_field_set;
   for (auto& cf : ctor_fields)
      ctor_field_set[symbol_name(as_symbol_id(cf))] = true;
   std::vector<Value> list_items = {mk_sym("list")};
   for (auto& fn : all_field_names)
      {
      std::string fname = symbol_name(as_symbol_id(fn));
      if (ctor_field_set.count(fname))
         list_items.push_back(fn);
      else
         list_items.push_back(make_unspecified());
      }
   Value ctor_body = make_list({mk_sym("%make-record"), rt_sym, make_list(list_items)});
   Value ctor_params = make_list(ctor_fields);
   forms.push_back(mk_define(ctor_name,
      make_list({mk_sym("lambda"), ctor_params, ctor_body})));
   // Predicate
   Value obj_sym = mk_sym("obj");
   Value pred_body = make_list({mk_sym("%record-of-type?"), obj_sym, rt_sym});
   forms.push_back(mk_define(pred_sexpr,
      make_list({mk_sym("lambda"), make_list({obj_sym}), pred_body})));
   // Fields
   for (size_t i = 0; i < field_entries.size(); ++i)
      {
      auto& fe = field_entries[i];
      Value idx_val = make_fixnum((int64_t)i);
      Value acc_init = make_list({mk_sym("%make-record-accessor"), rt_sym,
                                  idx_val, mk_quote_val(fe.accessor)});
      forms.push_back(mk_define(fe.accessor, acc_init));
      if (fe.has_mutator)
         {
         Value mut_init = make_list({mk_sym("%make-record-mutator"), rt_sym,
                                     idx_val, mk_quote_val(fe.mutator)});
         forms.push_back(mk_define(fe.mutator, mut_init));
         }
      }
   return make_list(forms);
   }

// ── cond-expand ────────────────────────────────────────────────────────────────

static const std::unordered_map<std::string,bool>& cekscheme_features()
   {
   static std::unordered_map<std::string,bool> feats = []
      {
      std::unordered_map<std::string,bool> f;
      f["r7rs"]           = true;
      f["exact-closed"]   = true;
      f["exact-rational"] = true;
      f["ratios"]         = true;
      f["ieee-float"]     = true;
      f["full-unicode"]   = true;
      f["cekscheme"]      = true;
#if defined(_WIN32)
      f["windows"] = true;
#elif defined(__linux__)
      f["posix"]  = true;
      f["linux"]  = true;
#elif defined(__APPLE__)
      f["posix"]  = true;
      f["darwin"] = true;
#endif
      return f;
      }();
   return feats;
   }

static bool feature_req_matches(Value req);

static bool feature_req_matches(Value req)
   {
   if (is_symbol(req))
      {
      std::string name = symbol_name(as_symbol_id(req));
      if (name == "else") return true;
      return cekscheme_features().count(name) > 0;
      }
   if (!is_cons(req) || !is_symbol(car(req))) return false;
   std::string op = symbol_name(as_symbol_id(car(req)));
   if (op == "and")
      {
      Value cur = cdr(req);
      while (is_cons(cur))
         {
         if (!feature_req_matches(car(cur))) return false;
         cur = cdr(cur);
         }
      return true;
      }
   if (op == "or")
      {
      Value cur = cdr(req);
      while (is_cons(cur))
         {
         if (feature_req_matches(car(cur))) return true;
         cur = cdr(cur);
         }
      return false;
      }
   if (op == "not" && is_cons(cdr(req)))
      return !feature_req_matches(car(cdr(req)));
   return false;
   }

static Value expand_cond_expand(Value sexpr, Environment*& env_ref, int& depth)
   {
   if (!is_cons(cdr(sexpr))) return expand_list(sexpr, env_ref, depth);
   Value cur = cdr(sexpr);
   while (is_cons(cur))
      {
      Value clause = car(cur);
      if (!is_cons(clause)) return expand_list(sexpr, env_ref, depth);
      Value req  = car(clause);
      Value body = cdr(clause);
      if (feature_req_matches(req))
         {
         if (is_nil(body)) return make_unspecified();
         std::vector<Value> items = {make_symbol("begin")};
         Value bc = body;
         while (is_cons(bc)) { items.push_back(expand_impl(car(bc), env_ref, depth)); bc = cdr(bc); }
         return make_list(items);
         }
      cur = cdr(cur);
      }
   return expand_list(sexpr, env_ref, depth);
   }

// ── include ───────────────────────────────────────────────────────────────────

static Value expand_include(Value sexpr, bool ci,
                             Environment*& env_ref, int& depth)
   {
   if (!is_cons(cdr(sexpr))) return expand_list(sexpr, env_ref, depth);
   std::vector<Value> all_forms;
   Value cur = cdr(sexpr);
   while (is_cons(cur))
      {
      Value path_val = car(cur);
      if (!is_string(path_val)) return expand_list(sexpr, env_ref, depth);
      std::string path = as_string(path_val)->data;
      // TODO: resolve relative to including file; for now use path as-is
      try
         {
         std::string source;
         {
         FILE* fp = fopen(path.c_str(), "r");
         if (!fp) throw SchemeSyntaxError("include: file not found: " + path);
         fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
         source.resize(sz);
         fread(source.data(), 1, sz, fp);
         fclose(fp);
         }
         Parser p(source);
         while (!p.at_end())
            {
            auto v = p.next();
            if (!v) break;
            Value form = *v;
            if (ci)
               {
               // case-fold: lowercase all symbol names recursively
               std::function<Value(Value)> fold_sym = [&](Value f) -> Value {
                  if (is_symbol(f))
                     {
                     std::string n = symbol_name(as_symbol_id(f));
                     std::transform(n.begin(), n.end(), n.begin(),
                        [](unsigned char c){ return (char)std::tolower(c); });
                     return make_symbol(n);
                     }
                  if (is_cons(f))
                     return cons2(fold_sym(car(f)), fold_sym(cdr(f)));
                  return f;
               };
               form = fold_sym(form);
               }
            all_forms.push_back(expand_impl(form, env_ref, depth));
            }
         }
      catch (SchemeSyntaxError&) { throw; }
      catch (...) { return expand_list(sexpr, env_ref, depth); }
      cur = cdr(cur);
      }
   if (all_forms.empty()) return expand_list(sexpr, env_ref, depth);
   std::vector<Value> items = {make_symbol("begin")};
   for (auto& f : all_forms) items.push_back(f);
   return make_list(items);
   }

// ── define-syntax / let-syntax / letrec-syntax ───────────────────────────────

static Value expand_define_syntax(Value sexpr, Environment*& env_ref, int& depth)
   {
   (void)depth;
   if (!is_cons(cdr(sexpr)) || !is_symbol(car(cdr(sexpr))))
      throw SchemeSyntaxError("define-syntax: expected (define-syntax <name> <transformer>)");
   Value macro_sym  = car(cdr(sexpr));
   if (!is_cons(cdr(cdr(sexpr))))
      throw SchemeSyntaxError("define-syntax: missing transformer");
   Value tr_expr = car(cdr(cdr(sexpr)));
   if (!is_head_sym(tr_expr, "syntax-rules"))
      throw SchemeSyntaxError("define-syntax: transformer must be (syntax-rules ...)");
   // parse_syntax_rules is provided by syntax_rules.cpp
   // We call it via apply_syntax_transformer indirection: for now stub.
   // Forward declaration:
   extern Value parse_syntax_rules_val(Value tail_after_syntax_rules,
                                        Environment* def_env,
                                        std::string_view name);
   std::string name = symbol_name(as_symbol_id(macro_sym));
   Value t = parse_syntax_rules_val(cdr(tr_expr), env_ref, name);
   if (env_ref)
      env_ref->bind_id(as_symbol_id(macro_sym), t);
   return make_unspecified();
   }

static Value expand_let_syntax_form(Value sexpr, bool is_letrec,
                                     Environment*& env_ref, int& depth)
   {
   extern Value parse_syntax_rules_val(Value tail_after_syntax_rules,
                                        Environment* def_env,
                                        std::string_view name);
   if (!is_cons(cdr(sexpr)))
      throw SchemeSyntaxError("let-syntax: malformed");
   Value bindings = car(cdr(sexpr));
   Value body     = cdr(cdr(sexpr));
   if (!is_cons(body))
      throw SchemeSyntaxError("let-syntax: empty body");
   Environment* outer_env = env_ref;
   // Create child env
   auto* child_env = new Environment(outer_env);
   // Note: GC cannot manage this without gc_alloc_environment.
   // This will leak unless we track it. For now use the GC allocator.
   // TODO: use gc_alloc_environment when available.
   if (is_letrec)
      env_ref = child_env;
   try
      {
      RenameTable let_rename;
      Value cur = bindings;
      while (is_cons(cur))
         {
         Value b = car(cur);
         if (!is_cons(b) || !is_symbol(car(b)) || !is_cons(cdr(b)))
            throw SchemeSyntaxError("let-syntax: malformed binding");
         std::string bname = symbol_name(as_symbol_id(car(b)));
         Value tr_expr = car(cdr(b));
         if (!is_head_sym(tr_expr, "syntax-rules"))
            throw SchemeSyntaxError("let-syntax: transformer must be (syntax-rules ...)");
         Value t = parse_syntax_rules_val(cdr(tr_expr), env_ref, bname);
         if (is_letrec)
            child_env->bind_id(as_symbol_id(car(b)), t);
         else
            {
            std::string gs = hygiene_gensym(bname);
            uint32_t gs_id = intern_symbol(gs);
            child_env->bind_id(gs_id, t);
            let_rename[bname] = gs;
            }
         cur = cdr(cur);
         }
      env_ref = child_env;
      // Wrap multiple body forms in (begin ...)
      Value wrapped;
      if (is_cons(cdr(body)))
         {
         std::vector<Value> items = {make_symbol("begin")};
         Value bc = body;
         while (is_cons(bc)) { items.push_back(car(bc)); bc = cdr(bc); }
         wrapped = make_list(items);
         }
      else
         wrapped = car(body);
      if (!let_rename.empty())
         wrapped = rename_refs(wrapped, let_rename);
      Value result = expand_impl(wrapped, env_ref, depth);
      env_ref = outer_env;
      return result;
      }
   catch (...)
      {
      env_ref = outer_env;
      throw;
      }
   }

// ── Main expand loop ──────────────────────────────────────────────────────────

static constexpr int MAX_EXPAND_ITER  = 200;
static constexpr int MAX_EXPAND_DEPTH = 500;

static Value expand_impl(Value sexpr, Environment*& env_ref, int& depth)
   {
   if (depth > MAX_EXPAND_DEPTH)
      throw SchemeSyntaxError("macro expansion depth exceeded");
   ++depth;
   struct DepthGuard { int& d; ~DepthGuard(){ --d; } } guard{depth};
   (void)guard;

   int iter = 0;
   while (true)
      {
      if (++iter > MAX_EXPAND_ITER)
         throw SchemeSyntaxError("macro expansion limit exceeded");
      if (!is_cons(sexpr))
         return sexpr;
      Value head = car(sexpr);
      if (is_symbol(head))
         {
         std::string name = symbol_name(as_symbol_id(head));
         // Syntax-installing forms
         if (name == "define-syntax")
            return expand_define_syntax(sexpr, env_ref, depth);
         if (name == "let-syntax")
            return expand_let_syntax_form(sexpr, false, env_ref, depth);
         if (name == "letrec-syntax")
            return expand_let_syntax_form(sexpr, true, env_ref, depth);
         // Check for user macro
         if (env_ref)
            {
            auto opt = env_ref->lookup_optional_id(as_symbol_id(head));
            if (opt && is_syntax_transformer(*opt))
               {
               sexpr = apply_syntax_transformer(*opt, sexpr);
               continue;
               }
            }
         // Sugar handlers
         if (name == "define")          return expand_define(sexpr, env_ref, depth);
         if (name == "lambda")          return expand_lambda(sexpr, env_ref, depth);
         if (name == "let")             return expand_let_family(sexpr, "let", env_ref, depth);
         if (name == "let*")            return expand_let_family(sexpr, "let*", env_ref, depth);
         if (name == "letrec")          return expand_let_family(sexpr, "letrec", env_ref, depth);
         if (name == "letrec*")         return expand_let_family(sexpr, "letrec*", env_ref, depth);
         if (name == "if")              return expand_if(sexpr, env_ref, depth);
         if (name == "quote")           return sexpr;
         if (name == "when")            return expand_when_unless(sexpr, "when", env_ref, depth);
         if (name == "unless")          return expand_when_unless(sexpr, "unless", env_ref, depth);
         if (name == "case-lambda")     return expand_case_lambda(sexpr, env_ref, depth);
         if (name == "do")              return expand_do(sexpr, env_ref, depth);
         if (name == "quasiquote")      return expand_quasiquote(sexpr, env_ref, depth);
         if (name == "let-values")      return expand_let_values(sexpr, env_ref, depth);
         if (name == "let*-values")     return expand_let_star_values(sexpr, env_ref, depth);
         if (name == "define-values")   return expand_define_values(sexpr, env_ref, depth);
         if (name == "define-record-type") return expand_define_record_type(sexpr, env_ref, depth);
         if (name == "parameterize")    return expand_parameterize(sexpr, env_ref, depth);
         if (name == "guard")           return expand_guard(sexpr, env_ref, depth);
         if (name == "cond-expand")     return expand_cond_expand(sexpr, env_ref, depth);
         if (name == "include")         return expand_include(sexpr, false, env_ref, depth);
         if (name == "include-ci")      return expand_include(sexpr, true,  env_ref, depth);
         if (name == "define-library")  return sexpr;
         if (name == "import")          return sexpr;
         }
      // SyntaxTransformer in operator position
      if (is_syntax_transformer(head))
         {
         sexpr = apply_syntax_transformer(head, sexpr);
         continue;
         }
      return expand_list(sexpr, env_ref, depth);
      }
   }

Value expand(Value sexpr, Environment*& env_ref)
   {
   int depth = 0;
   return expand_impl(sexpr, env_ref, depth);
   }
