#include "syntax_rules.h"
#include "expander.h"
#include "gc.h"
#include <algorithm>
#include <cassert>
#include <unordered_set>

// ── Syntactic keywords that are never renamed in templates ───────────────────

static bool is_syntactic_keyword(const std::string& s)
   {
   static const std::unordered_set<std::string> KEYWORDS = {
      "if","lambda","begin","define","set!","quote",
      "let","let*","letrec","letrec*",
      "and","or","case","cond","when","unless","do","guard",
      "parameterize","define-syntax","let-syntax","letrec-syntax",
      "define-record-type","syntax-rules","define-library",
      "import","export","case-lambda",
      "quasiquote","unquote","unquote-splicing",
      "define-values","let-values","let*-values",
      "include","include-ci","cond-expand",
      "delay","delay-force",
      "else","=>","library",
      "_","...",
   };
   return KEYWORDS.count(s) > 0;
   }

// ── SyntaxMatch ───────────────────────────────────────────────────────────────

struct SyntaxMatch
   {
   std::unordered_map<std::string,Value>              scalars;
   std::unordered_map<std::string,std::vector<Value>> ell;    // depth >= 1
   std::unordered_map<std::string,int>                ell_depth;
   };

static bool is_ellipsis_sym(Value v, uint32_t ell_sid)
   {
   return is_symbol(v) && as_symbol_id(v) == ell_sid;
   }

// ── Pattern-variable collectors ───────────────────────────────────────────────

static void collect_pvars(Value pat,
                           const std::vector<uint32_t>& literals,
                           uint32_t ell_sid,
                           std::unordered_set<std::string>& out)
   {
   if (is_symbol(pat))
      {
      uint32_t sid = as_symbol_id(pat);
      if (sid == ell_sid) return;
      std::string s = symbol_name(sid);
      if (s == "_") return;
      for (auto lit : literals)
         if (lit == sid) return;
      out.insert(s);
      return;
      }
   if (is_cons(pat))
      {
      collect_pvars(car(pat), literals, ell_sid, out);
      collect_pvars(cdr(pat), literals, ell_sid, out);
      return;
      }
   if (is_vector(pat))
      {
      for (auto& item : as_vector(pat)->elements)
         collect_pvars(item, literals, ell_sid, out);
      }
   }

static void collect_pvars_with_depth(Value pat,
                                      const std::vector<uint32_t>& literals,
                                      uint32_t ell_sid,
                                      std::unordered_map<std::string,int>& out,
                                      int depth)
   {
   if (is_symbol(pat))
      {
      uint32_t sid = as_symbol_id(pat);
      if (sid == ell_sid) return;
      std::string s = symbol_name(sid);
      if (s == "_") return;
      for (auto lit : literals)
         if (lit == sid) return;
      out[s] = depth;
      return;
      }
   if (is_cons(pat))
      {
      Value cur = pat;
      while (is_cons(cur))
         {
         Value elem = car(cur);
         Value rest = cdr(cur);
         bool has_ell = is_cons(rest) && is_ellipsis_sym(car(rest), ell_sid);
         if (has_ell)
            {
            collect_pvars_with_depth(elem, literals, ell_sid, out, depth + 1);
            cur = cdr(rest);
            }
         else
            {
            collect_pvars_with_depth(elem, literals, ell_sid, out, depth);
            cur = rest;
            }
         }
      if (!is_nil(cur))
         collect_pvars_with_depth(cur, literals, ell_sid, out, depth);
      return;
      }
   if (is_vector(pat))
      {
      const auto& items = as_vector(pat)->elements;
      size_t n = items.size();
      for (size_t i = 0; i < n; ++i)
         {
         bool has_ell = (i + 1 < n && is_symbol(items[i+1]) &&
                         as_symbol_id(items[i+1]) == ell_sid);
         if (has_ell)
            {
            collect_pvars_with_depth(items[i], literals, ell_sid, out, depth + 1);
            ++i;
            }
         else
            collect_pvars_with_depth(items[i], literals, ell_sid, out, depth);
         }
      }
   }

// ── Free-identifier collector ─────────────────────────────────────────────────

static void collect_free_ids(Value tmpl,
                              const std::unordered_set<std::string>& pvars,
                              const std::vector<uint32_t>& literals,
                              uint32_t ell_sid,
                              std::unordered_set<std::string>& out)
   {
   if (is_symbol(tmpl))
      {
      uint32_t sid = as_symbol_id(tmpl);
      std::string s = symbol_name(sid);
      if (sid == ell_sid || s == "_") return;
      if (pvars.count(s)) return;
      for (auto lit : literals)
         if (lit == sid) return;
      if (is_syntactic_keyword(s)) return;
      out.insert(s);
      return;
      }
   if (is_cons(tmpl))
      {
      if (is_symbol(car(tmpl)) &&
          symbol_name(as_symbol_id(car(tmpl))) == "quote")
         return;
      collect_free_ids(car(tmpl), pvars, literals, ell_sid, out);
      collect_free_ids(cdr(tmpl), pvars, literals, ell_sid, out);
      return;
      }
   if (is_vector(tmpl))
      for (auto& item : as_vector(tmpl)->elements)
         collect_free_ids(item, pvars, literals, ell_sid, out);
   }

// ── Binding-site intro-name collector ─────────────────────────────────────────

static void cbi_formals(Value formals,
                        const std::unordered_set<std::string>& pvars,
                        std::unordered_set<std::string>& out)
   {
   Value cur = formals;
   while (is_cons(cur))
      {
      if (is_symbol(car(cur)))
         {
         std::string n = symbol_name(as_symbol_id(car(cur)));
         if (!pvars.count(n)) out.insert(n);
         }
      cur = cdr(cur);
      }
   if (is_symbol(cur))
      {
      std::string n = symbol_name(as_symbol_id(cur));
      if (!pvars.count(n)) out.insert(n);
      }
   }

static void cbi_let_bindings(Value bindings,
                              const std::unordered_set<std::string>& pvars,
                              std::unordered_set<std::string>& out)
   {
   Value cur = bindings;
   while (is_cons(cur))
      {
      Value b = car(cur);
      if (is_cons(b) && is_symbol(car(b)))
         {
         std::string n = symbol_name(as_symbol_id(car(b)));
         if (!pvars.count(n)) out.insert(n);
         }
      cur = cdr(cur);
      }
   }

static void collect_binding_intros(Value tmpl,
                                    const std::unordered_set<std::string>& pvars,
                                    std::unordered_set<std::string>& out)
   {
   if (!is_cons(tmpl)) return;
   if (is_symbol(car(tmpl)) && symbol_name(as_symbol_id(car(tmpl))) == "quote") return;
   if (is_symbol(car(tmpl)))
      {
      std::string hname = symbol_name(as_symbol_id(car(tmpl)));
      if (hname == "lambda")
         {
         if (is_cons(cdr(tmpl)))
            {
            cbi_formals(car(cdr(tmpl)), pvars, out);
            collect_binding_intros(cdr(cdr(tmpl)), pvars, out);
            }
         return;
         }
      if (hname == "let" || hname == "let*" ||
          hname == "letrec" || hname == "letrec*")
         {
         if (is_cons(cdr(tmpl)))
            {
            Value second     = car(cdr(tmpl));
            Value body_start = cdr(cdr(tmpl));
            Value bindings   = second;
            if (hname == "let" && is_symbol(second))
               {
               std::string n = symbol_name(as_symbol_id(second));
               if (!pvars.count(n)) out.insert(n);
               if (is_cons(body_start))
                  {
                  bindings   = car(body_start);
                  body_start = cdr(body_start);
                  }
               else return;
               }
            cbi_let_bindings(bindings, pvars, out);
            collect_binding_intros(body_start, pvars, out);
            }
         return;
         }
      if (hname == "define")
         {
         if (is_cons(cdr(tmpl)))
            {
            Value nameform = car(cdr(tmpl));
            if (is_symbol(nameform))
               {
               std::string n = symbol_name(as_symbol_id(nameform));
               if (!pvars.count(n)) out.insert(n);
               }
            else if (is_cons(nameform) && is_symbol(car(nameform)))
               {
               std::string n = symbol_name(as_symbol_id(car(nameform)));
               if (!pvars.count(n)) out.insert(n);
               cbi_formals(cdr(nameform), pvars, out);
               }
            collect_binding_intros(cdr(cdr(tmpl)), pvars, out);
            }
         return;
         }
      }
   collect_binding_intros(car(tmpl), pvars, out);
   collect_binding_intros(cdr(tmpl), pvars, out);
   }

// ── Datum equality (for literal matching in patterns) ─────────────────────────

static bool datum_equal(Value a, Value b)
   {
   if (values_eqv(a, b)) return true;
   if (is_string(a) && is_string(b))
      return as_string(a)->data == as_string(b)->data;
   if (is_cons(a) && is_cons(b))
      return datum_equal(car(a), car(b)) && datum_equal(cdr(a), cdr(b));
   if (is_nil(a) && is_nil(b)) return true;
   if (is_vector(a) && is_vector(b))
      {
      auto& va = as_vector(a)->elements;
      auto& vb = as_vector(b)->elements;
      if (va.size() != vb.size()) return false;
      for (size_t i = 0; i < va.size(); ++i)
         if (!datum_equal(va[i], vb[i])) return false;
      return true;
      }
   return false;
   }

// ── Pattern matching ──────────────────────────────────────────────────────────

static bool match_pattern(Value pat, Value form,
                           const std::vector<uint32_t>& literals,
                           uint32_t ell_sid, SyntaxMatch& out);

static bool match_list_pattern(Value pat_list, Value form_list,
                                const std::vector<uint32_t>& literals,
                                uint32_t ell_sid, SyntaxMatch& out)
   {
   while (is_cons(pat_list))
      {
      Value pat_elem = car(pat_list);
      Value pat_rest = cdr(pat_list);
      bool  has_ell  = is_cons(pat_rest) &&
                       is_ellipsis_sym(car(pat_rest), ell_sid);
      if (has_ell)
         {
         Value suffix_pat = cdr(pat_rest);
         // Count suffix length
         int suffix_need = 0;
         { Value c = suffix_pat; while (is_cons(c)){ ++suffix_need; c = cdr(c); } }
         // Collect form elements into vector
         std::vector<Value> form_vec;
         { Value t = form_list;
           while (is_cons(t)){ form_vec.push_back(car(t)); t = cdr(t); } }
         int total = (int)form_vec.size();
         if (total < suffix_need) return false;
         int n_ell = total - suffix_need;
         // Collect pvar depths within pat_elem
         std::unordered_map<std::string,int> pvar_depths;
         collect_pvars_with_depth(pat_elem, literals, ell_sid, pvar_depths, 0);
         for (auto& [pv, _] : pvar_depths)
            {
            out.ell[pv] = {};
            out.ell_depth[pv] = pvar_depths[pv] + 1;
            }
         // Match each form element against pat_elem
         for (int i = 0; i < n_ell; ++i)
            {
            SyntaxMatch sub;
            if (!match_pattern(pat_elem, form_vec[i], literals, ell_sid, sub))
               return false;
            for (auto& [k, v] : sub.scalars)
               out.ell[k].push_back(v);
            for (auto& [k, sv] : sub.ell)
               {
               // Convert sub.ell[k] (vector<Value>) to a Scheme cons list
               Value sublist = make_nil();
               for (int j = (int)sv.size() - 1; j >= 0; --j)
                  {
                  auto* cell = gc_alloc_cons();
                  cell->car = sv[j];
                  cell->cdr = sublist;
                  sublist = make_cons(cell);
                  }
               out.ell[k].push_back(sublist);
               }
            }
         // Build suffix form list for remaining elements
         Value suffix_form = make_nil();
         for (int j = total - 1; j >= n_ell; --j)
            {
            auto* cell = gc_alloc_cons();
            cell->car = form_vec[j];
            cell->cdr = suffix_form;
            suffix_form = make_cons(cell);
            }
         return match_list_pattern(suffix_pat, suffix_form, literals, ell_sid, out);
         }
      // No ellipsis
      if (!is_cons(form_list)) return false;
      if (!match_pattern(pat_elem, car(form_list), literals, ell_sid, out))
         return false;
      pat_list  = pat_rest;
      form_list = cdr(form_list);
      }
   if (is_nil(pat_list))
      return is_nil(form_list);
   if (is_symbol(pat_list))
      {
      uint32_t sid = as_symbol_id(pat_list);
      for (auto lit : literals)
         if (lit == sid) return false;
      out.scalars[symbol_name(sid)] = form_list;
      return true;
      }
   return false;
   }

static bool match_vector_pattern(const std::vector<Value>& pat_items,
                                  const std::vector<Value>& form_items,
                                  const std::vector<uint32_t>& literals,
                                  uint32_t ell_sid, SyntaxMatch& out)
   {
   size_t i = 0, j = 0;
   size_t n_pat  = pat_items.size();
   size_t n_form = form_items.size();
   while (i < n_pat)
      {
      bool has_ell = (i + 1 < n_pat &&
                      is_symbol(pat_items[i+1]) &&
                      as_symbol_id(pat_items[i+1]) == ell_sid);
      if (has_ell)
         {
         size_t suffix_count = n_pat - (i + 2);
         size_t available    = n_form - j;
         if (available < suffix_count) return false;
         size_t n_ell = available - suffix_count;
         std::unordered_map<std::string,int> pvar_depths;
         collect_pvars_with_depth(pat_items[i], literals, ell_sid, pvar_depths, 0);
         for (auto& [pv, _] : pvar_depths)
            {
            out.ell[pv] = {};
            out.ell_depth[pv] = pvar_depths[pv] + 1;
            }
         for (size_t k = 0; k < n_ell; ++k)
            {
            SyntaxMatch sub;
            if (!match_pattern(pat_items[i], form_items[j+k], literals, ell_sid, sub))
               return false;
            for (auto& [key, v] : sub.scalars)
               out.ell[key].push_back(v);
            for (auto& [key, sv] : sub.ell)
               {
               Value sublist = make_nil();
               for (int q = (int)sv.size() - 1; q >= 0; --q)
                  {
                  auto* cell = gc_alloc_cons();
                  cell->car = sv[q];
                  cell->cdr = sublist;
                  sublist = make_cons(cell);
                  }
               out.ell[key].push_back(sublist);
               }
            }
         j += n_ell;
         i += 2;
         continue;
         }
      if (j >= n_form) return false;
      if (!match_pattern(pat_items[i], form_items[j], literals, ell_sid, out))
         return false;
      ++i; ++j;
      }
   return j == n_form;
   }

static bool match_pattern(Value pat, Value form,
                           const std::vector<uint32_t>& literals,
                           uint32_t ell_sid, SyntaxMatch& out)
   {
   if (is_symbol(pat))
      {
      uint32_t sid = as_symbol_id(pat);
      std::string s = symbol_name(sid);
      if (s == "_") return true;
      for (auto lit : literals)
         {
         if (lit == sid)
            return is_symbol(form) && as_symbol_id(form) == sid;
         }
      out.scalars[s] = form;
      return true;
      }
   if (is_cons(pat))
      return match_list_pattern(pat, form, literals, ell_sid, out);
   if (is_vector(pat))
      {
      if (!is_vector(form)) return false;
      return match_vector_pattern(as_vector(pat)->elements,
                                  as_vector(form)->elements,
                                  literals, ell_sid, out);
      }
   if (is_nil(pat))
      return is_nil(form);
   return datum_equal(pat, form);
   }

// ── Template instantiation ────────────────────────────────────────────────────

// Convert a scheme cons list back to vector<Value> (for peel at depth > 1)
static std::vector<Value> cons_to_vec(Value list)
   {
   std::vector<Value> v;
   while (is_cons(list))
      {
      v.push_back(car(list));
      list = cdr(list);
      }
   return v;
   }

static void collect_ell_refs(Value tmpl, const SyntaxMatch& m,
                              std::vector<std::string>& out)
   {
   if (is_symbol(tmpl))
      {
      std::string s = symbol_name(as_symbol_id(tmpl));
      if (m.ell.count(s)) out.push_back(s);
      return;
      }
   if (is_cons(tmpl))
      {
      collect_ell_refs(car(tmpl), m, out);
      collect_ell_refs(cdr(tmpl), m, out);
      return;
      }
   if (is_vector(tmpl))
      for (auto& item : as_vector(tmpl)->elements)
         collect_ell_refs(item, m, out);
   }

static Value instantiate(Value tmpl, const SyntaxMatch& match,
                          uint32_t ell_sid,
                          const std::unordered_map<std::string,std::string>& free_id_map);

static void raise_syntax_error_tmpl(Value args_tail, const SyntaxMatch& match,
                                     uint32_t ell_sid,
                                     const std::unordered_map<std::string,std::string>& free_id_map)
   {
   std::vector<Value> args;
   Value cur = args_tail;
   while (is_cons(cur))
      {
      args.push_back(instantiate(car(cur), match, ell_sid, free_id_map));
      cur = cdr(cur);
      }
   std::string msg = "syntax-error";
   size_t start = 0;
   if (!args.empty() && is_string(args[0]))
      {
      msg = as_string(args[0])->data;
      start = 1;
      }
   for (size_t i = start; i < args.size(); ++i)
      msg += ": " + value_to_string(args[i]);
   throw SchemeSyntaxError(msg);
   }

static Value instantiate_vector(const std::vector<Value>& tmpl_items,
                                  const SyntaxMatch& match, uint32_t ell_sid,
                                  const std::unordered_map<std::string,std::string>& free_id_map)
   {
   std::vector<Value> output;
   size_t n = tmpl_items.size();
   for (size_t i = 0; i < n; ++i)
      {
      bool has_ell = (i + 1 < n &&
                      is_symbol(tmpl_items[i+1]) &&
                      as_symbol_id(tmpl_items[i+1]) == ell_sid);
      if (has_ell)
         {
         std::vector<std::string> ell_syms;
         collect_ell_refs(tmpl_items[i], match, ell_syms);
         if (!ell_syms.empty())
            {
            size_t count = match.ell.at(ell_syms[0]).size();
            for (size_t k = 0; k < count; ++k)
               {
               SyntaxMatch sub;
               sub.scalars   = match.scalars;
               sub.ell       = match.ell;
               sub.ell_depth = match.ell_depth;
               for (auto& sv : ell_syms)
                  {
                  int d = match.ell_depth.at(sv);
                  Value peeled = match.ell.at(sv)[k];
                  if (d == 1)
                     {
                     sub.scalars[sv] = peeled;
                     sub.ell.erase(sv);
                     sub.ell_depth.erase(sv);
                     }
                  else
                     {
                     sub.ell[sv] = cons_to_vec(peeled);
                     sub.ell_depth[sv] = d - 1;
                     }
                  }
               output.push_back(instantiate(tmpl_items[i], sub, ell_sid, free_id_map));
               }
            }
         ++i;
         continue;
         }
      output.push_back(instantiate(tmpl_items[i], match, ell_sid, free_id_map));
      }
   auto* vec = gc_alloc_vector(output.size());
   for (size_t i = 0; i < output.size(); ++i)
      vec->elements[i] = output[i];
   return make_vector(vec);
   }

static Value instantiate_list(Value tmpl_list, const SyntaxMatch& match,
                               uint32_t ell_sid,
                               const std::unordered_map<std::string,std::string>& free_id_map)
   {
   std::vector<Value> output;
   Value cur = tmpl_list;
   while (is_cons(cur))
      {
      Value elem = car(cur);
      Value rest = cdr(cur);
      bool has_ell = is_cons(rest) && is_ellipsis_sym(car(rest), ell_sid);
      if (has_ell)
         {
         std::vector<std::string> ell_syms;
         collect_ell_refs(elem, match, ell_syms);
         if (!ell_syms.empty())
            {
            size_t n = match.ell.at(ell_syms[0]).size();
            for (size_t i = 0; i < n; ++i)
               {
               SyntaxMatch sub;
               sub.scalars   = match.scalars;
               sub.ell       = match.ell;
               sub.ell_depth = match.ell_depth;
               for (auto& sv : ell_syms)
                  {
                  int d = match.ell_depth.at(sv);
                  Value peeled = match.ell.at(sv)[i];
                  if (d == 1)
                     {
                     sub.scalars[sv] = peeled;
                     sub.ell.erase(sv);
                     sub.ell_depth.erase(sv);
                     }
                  else
                     {
                     sub.ell[sv] = cons_to_vec(peeled);
                     sub.ell_depth[sv] = d - 1;
                     }
                  }
               output.push_back(instantiate(elem, sub, ell_sid, free_id_map));
               }
            }
         cur = cdr(rest);
         continue;
         }
      output.push_back(instantiate(elem, match, ell_sid, free_id_map));
      cur = rest;
      }
   // Build result cons list
   Value tail = is_nil(cur) ? make_nil()
                            : instantiate(cur, match, ell_sid, free_id_map);
   Value result = tail;
   for (int i = (int)output.size() - 1; i >= 0; --i)
      {
      auto* cell = gc_alloc_cons();
      cell->car = output[i];
      cell->cdr = result;
      result = make_cons(cell);
      }
   return result;
   }

static Value instantiate(Value tmpl, const SyntaxMatch& match,
                          uint32_t ell_sid,
                          const std::unordered_map<std::string,std::string>& free_id_map)
   {
   if (is_symbol(tmpl))
      {
      std::string s = symbol_name(as_symbol_id(tmpl));
      auto it = match.scalars.find(s);
      if (it != match.scalars.end()) return it->second;
      auto it2 = free_id_map.find(s);
      if (it2 != free_id_map.end()) return make_symbol(it2->second);
      return tmpl;
      }
   if (is_cons(tmpl))
      {
      // (ellipsis inner) — disable ellipsis inside inner
      if (is_ellipsis_sym(car(tmpl), ell_sid) &&
          is_cons(cdr(tmpl)) && is_nil(cdr(cdr(tmpl))))
         {
         static uint32_t NO_ELL = intern_symbol("\x00no-ellipsis\x00");
         return instantiate(car(cdr(tmpl)), match, NO_ELL, free_id_map);
         }
      // (syntax-error msg datum...)
      if (is_symbol(car(tmpl)) &&
          symbol_name(as_symbol_id(car(tmpl))) == "syntax-error")
         {
         raise_syntax_error_tmpl(cdr(tmpl), match, ell_sid, free_id_map);
         }
      return instantiate_list(tmpl, match, ell_sid, free_id_map);
      }
   if (is_vector(tmpl))
      return instantiate_vector(as_vector(tmpl)->elements, match, ell_sid, free_id_map);
   return tmpl;
   }

// ── apply_syntax_transformer ──────────────────────────────────────────────────

Value apply_syntax_transformer(Value transformer, Value form)
   {
   if (!is_syntax_transformer(transformer))
      throw SchemeSyntaxError("apply_syntax_transformer: not a transformer");
   auto* t = as_syntax_transformer(transformer);
   uint32_t ell_sid = t->ellipsis;
   Value form_tail = is_cons(form) ? cdr(form) : make_nil();
   // Per-application gensym for binding-intro-names
   std::unordered_map<std::string,std::string> free_id_map = t->free_id_map;
   for (auto& iname : t->binding_intro_names)
      {
      if (!free_id_map.count(iname))
         free_id_map[iname] = hygiene_gensym(iname);
      }
   for (auto& rule : t->rules)
      {
      Value pattern = rule.pattern;
      Value tmpl    = rule.tmpl;
      if (is_cons(pattern))
         {
         SyntaxMatch match;
         if (match_list_pattern(cdr(pattern), form_tail,
                                t->literals, ell_sid, match))
            return instantiate(tmpl, match, ell_sid, free_id_map);
         }
      }
   throw SchemeSyntaxError("syntax-rules: no matching pattern for '" + t->name + "'");
   }

// ── parse_syntax_rules_val ────────────────────────────────────────────────────

Value parse_syntax_rules_val(Value tail, Environment* def_env,
                              std::string_view name)
   {
   if (!is_cons(tail))
      throw SchemeSyntaxError("syntax-rules: malformed");
   // Parse optional custom ellipsis symbol + literals list
   std::string ell_str = "...";
   Value first     = car(tail);
   Value rest_tail = cdr(tail);
   Value lit_list, rules_list;
   if (is_symbol(first) && is_cons(rest_tail))
      {
      Value second = car(rest_tail);
      if (is_nil(second) || is_cons(second))
         {
         ell_str     = symbol_name(as_symbol_id(first));
         lit_list    = second;
         rules_list  = cdr(rest_tail);
         }
      else
         {
         lit_list   = first;
         rules_list = rest_tail;
         }
      }
   else
      {
      lit_list   = first;
      rules_list = rest_tail;
      }
   uint32_t ell_sid = intern_symbol(ell_str);
   // Parse literals
   std::vector<uint32_t> literals;
   Value cur = lit_list;
   while (is_cons(cur))
      {
      if (!is_symbol(car(cur)))
         throw SchemeSyntaxError("syntax-rules: literal must be a symbol");
      uint32_t sid = as_symbol_id(car(cur));
      std::string s = symbol_name(sid);
      if (s == "_" || s == ell_str)
         throw SchemeSyntaxError("syntax-rules: '" + s + "' cannot appear in literals list");
      literals.push_back(sid);
      cur = cdr(cur);
      }
   // Parse rules
   struct RuleRaw { Value pattern; Value tmpl; };
   std::vector<RuleRaw> raw_rules;
   std::unordered_set<std::string> pvars_union;
   std::vector<std::pair<Value,std::unordered_set<std::string>>> templates;
   cur = rules_list;
   while (is_cons(cur))
      {
      Value rule = car(cur);
      if (!is_cons(rule) || !is_cons(cdr(rule)))
         throw SchemeSyntaxError("syntax-rules: each rule must be (pattern template)");
      Value pattern  = car(rule);
      Value tmpl_val = car(cdr(rule));
      std::unordered_set<std::string> pvars;
      if (is_cons(pattern))
         collect_pvars(cdr(pattern), literals, ell_sid, pvars);
      for (auto& s : pvars) pvars_union.insert(s);
      raw_rules.push_back({pattern, tmpl_val});
      templates.push_back({tmpl_val, pvars});
      cur = cdr(cur);
      }
   // Collect free identifiers
   std::unordered_set<std::string> free_ids;
   for (auto& [tmpl_val, pvars] : templates)
      collect_free_ids(tmpl_val, pvars, literals, ell_sid, free_ids);
   // Collect binding-position intro names
   std::unordered_set<std::string> binding_intros;
   for (auto& [tmpl_val, pvars] : templates)
      collect_binding_intros(tmpl_val, pvars, binding_intros);
   // Build free_id_map and intro_names
   std::unordered_map<std::string,std::string> free_id_map;
   std::unordered_set<std::string> intro_names;
   for (auto& fid : free_ids)
      {
      std::optional<Value> opt;
      if (def_env)
         {
         uint32_t fid_sid = intern_symbol(fid);
         opt = def_env->lookup_optional_id(fid_sid);
         }
      if (opt)
         {
         std::string gs = hygiene_gensym(fid);
         free_id_map[fid] = gs;
         // Bind alias in global env so it persists past temporary envs
         if (def_env && def_env->global)
            {
            uint32_t gs_sid = intern_symbol(gs);
            def_env->global->bind_id(gs_sid, *opt);
            }
         }
      else
         intro_names.insert(fid);
      }
   // binding_intro_names = intro_names ∩ binding_intros
   std::vector<std::string> binding_intro_names;
   for (auto& n : intro_names)
      if (binding_intros.count(n))
         binding_intro_names.push_back(n);
   // Build the transformer
   auto* t = gc_alloc_syntax_transformer();
   t->name           = std::string(name);
   t->literals       = literals;
   t->ellipsis       = ell_sid;
   t->def_env        = def_env;
   t->free_id_map    = std::move(free_id_map);
   for (auto& n : intro_names) t->intro_names.push_back(n);
   t->binding_intro_names = std::move(binding_intro_names);
   for (auto& r : raw_rules)
      t->rules.push_back({r.pattern, r.tmpl});
   return make_syntax_transformer(t);
   }
