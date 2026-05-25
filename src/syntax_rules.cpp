// syntax_rules.cpp -- R7RS syntax-rules pattern matcher and template instantiator.
// Direct port of pyscheme/syntax_rules.py.
#include "syntax_rules.h"
#include "Parser.h"         // SchemeSyntaxError
#include "PrettyPrinter.h"  // scheme_pretty_print
#include <atomic>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ── Module-private globals ─────────────────────────────────────────────────
// Port of syntax_rules.py _SYNTACTIC_KEYWORDS, _GENSYM_COUNTER, _GENSYM_PREFIX.

static const std::unordered_set<std::string> SYNTACTIC_KEYWORDS = {
    "if", "lambda", "begin", "define", "set!", "quote",
    "let", "let*", "letrec", "letrec*",
    "and", "or", "case", "cond", "when", "unless", "do", "guard",
    "parameterize", "define-syntax", "let-syntax", "letrec-syntax",
    "define-record-type", "syntax-rules", "define-library",
    "import", "export", "case-lambda",
    "quasiquote", "unquote", "unquote-splicing",
    "define-values", "let-values", "let*-values",
    "include", "include-ci", "cond-expand",
    "delay", "delay-force",
    "else", "=>", "library",
    "_", "...",
};

static const std::string GENSYM_PREFIX = "\x01h.";
static std::atomic<int>  s_gensym_counter{0};

// ── hygiene_gensym ─────────────────────────────────────────────────────────

std::string hygiene_gensym(const std::string& base) {
    if (base.rfind(GENSYM_PREFIX, 0) == 0)
        return base;
    int n = ++s_gensym_counter;
    return GENSYM_PREFIX + base + '.' + std::to_string(n);
}

// ── SyntaxMatch ────────────────────────────────────────────────────────────
// Port of syntax_rules.py _SyntaxMatch.
// ellipsis values: at depth 1, plain Values; at depth 2+, SchemeVector* Values
// wrapping the next level (mirrors Python's nested-list approach).

struct SyntaxMatch {
    std::unordered_map<uint32_t, Value>              scalars;   // depth-0 pvars
    std::unordered_map<uint32_t, std::vector<Value>> ellipsis;  // depth>=1 pvars
    std::unordered_map<uint32_t, int>                ell_depth; // pvar -> depth
};

// ── Helpers ────────────────────────────────────────────────────────────────

static bool is_ellipsis_sym(const Value& form, uint32_t ellipsis_id) {
    return is_symbol(form) && as_symbol_id(form) == ellipsis_id;
}

// Sentinel ellipsis id that disables ellipsis expansion inside (... inner).
static uint32_t get_no_ellipsis_id() {
    static const uint32_t id = []() {
        std::string s;
        s += '\x00';
        s += "no-ellipsis";
        s += '\x00';
        return intern_symbol(s);
    }();
    return id;
}

// ── Pattern variable collectors ────────────────────────────────────────────
// Port of syntax_rules.py collect_pvars.

static void collect_pvars(const Value& pat,
                           const std::vector<uint32_t>& literals,
                           uint32_t ellipsis_id,
                           std::unordered_set<uint32_t>& out) {
    static const uint32_t US = intern_symbol("_");
    if (is_symbol(pat)) {
        uint32_t sid = as_symbol_id(pat);
        if (sid == US || sid == ellipsis_id) return;
        for (uint32_t lit : literals)
            if (sid == lit) return;
        out.insert(sid);
        return;
    }
    if (is_cons(pat)) {
        collect_pvars(car(pat), literals, ellipsis_id, out);
        collect_pvars(cdr(pat), literals, ellipsis_id, out);
    }
    if (is_vector(pat))
        for (const Value& item : as_vector_items_const(pat))
            collect_pvars(item, literals, ellipsis_id, out);
}

// Port of syntax_rules.py collect_pvars_with_depth.
static void collect_pvars_with_depth(const Value& pat,
                                      const std::vector<uint32_t>& literals,
                                      uint32_t ellipsis_id,
                                      std::unordered_map<uint32_t, int>& out,
                                      int depth) {
    static const uint32_t US = intern_symbol("_");
    if (is_symbol(pat)) {
        uint32_t sid = as_symbol_id(pat);
        if (sid == US || sid == ellipsis_id) return;
        for (uint32_t lit : literals)
            if (sid == lit) return;
        out[sid] = depth;
        return;
    }
    if (is_cons(pat)) {
        Value cur = pat;
        while (is_cons(cur)) {
            Value elem = car(cur);
            Value rest = cdr(cur);
            bool has_ell = is_cons(rest) && is_ellipsis_sym(car(rest), ellipsis_id);
            if (has_ell) {
                collect_pvars_with_depth(elem, literals, ellipsis_id, out, depth + 1);
                cur = cdr(rest);
            } else {
                collect_pvars_with_depth(elem, literals, ellipsis_id, out, depth);
                cur = rest;
            }
        }
        if (!is_nil(cur))
            collect_pvars_with_depth(cur, literals, ellipsis_id, out, depth);
    }
    if (is_vector(pat)) {
        const auto& items = as_vector_items_const(pat);
        int n = (int)items.size();
        int i = 0;
        while (i < n) {
            bool has_ell = (i + 1 < n && is_ellipsis_sym(items[i + 1], ellipsis_id));
            if (has_ell) {
                collect_pvars_with_depth(items[i], literals, ellipsis_id, out, depth + 1);
                i += 2;
            } else {
                collect_pvars_with_depth(items[i], literals, ellipsis_id, out, depth);
                i += 1;
            }
        }
    }
}

// ── Free-identifier collector ──────────────────────────────────────────────
// Port of syntax_rules.py collect_free_ids.

static void collect_free_ids(const Value& tmpl,
                              const std::unordered_set<uint32_t>& pvars,
                              const std::vector<uint32_t>& literals,
                              uint32_t ellipsis_id,
                              std::unordered_set<uint32_t>& out) {
    static const uint32_t US        = intern_symbol("_");
    static const uint32_t QUOTE_SID = intern_symbol("quote");
    if (is_symbol(tmpl)) {
        uint32_t sid = as_symbol_id(tmpl);
        if (sid == ellipsis_id || sid == US) return;
        if (pvars.count(sid)) return;
        for (uint32_t lit : literals)
            if (sid == lit) return;
        if (SYNTACTIC_KEYWORDS.count(symbol_name(sid))) return;
        out.insert(sid);
        return;
    }
    if (is_cons(tmpl)) {
        if (is_symbol(car(tmpl)) && as_symbol_id(car(tmpl)) == QUOTE_SID)
            return;
        collect_free_ids(car(tmpl), pvars, literals, ellipsis_id, out);
        collect_free_ids(cdr(tmpl), pvars, literals, ellipsis_id, out);
    }
    if (is_vector(tmpl))
        for (const Value& item : as_vector_items_const(tmpl))
            collect_free_ids(item, pvars, literals, ellipsis_id, out);
}

// ── Binding-site intro-name collector ─────────────────────────────────────
// Port of syntax_rules.py _cbi_formals, _cbi_let_bindings, collect_binding_intros.

static void cbi_formals(const Value& formals,
                         const std::unordered_set<uint32_t>& pvars,
                         std::unordered_set<uint32_t>& out) {
    Value cur = formals;
    while (is_cons(cur)) {
        if (is_symbol(car(cur))) {
            uint32_t n_sid = as_symbol_id(car(cur));
            if (!pvars.count(n_sid)) out.insert(n_sid);
        }
        cur = cdr(cur);
    }
    if (is_symbol(cur)) {
        uint32_t n_sid = as_symbol_id(cur);
        if (!pvars.count(n_sid)) out.insert(n_sid);
    }
}

static void cbi_let_bindings(const Value& bindings,
                              const std::unordered_set<uint32_t>& pvars,
                              std::unordered_set<uint32_t>& out) {
    Value cur = bindings;
    while (is_cons(cur)) {
        Value b = car(cur);
        if (is_cons(b) && is_symbol(car(b))) {
            uint32_t n_sid = as_symbol_id(car(b));
            if (!pvars.count(n_sid)) out.insert(n_sid);
        }
        cur = cdr(cur);
    }
}

static void collect_binding_intros(const Value& tmpl,
                                    const std::unordered_set<uint32_t>& pvars,
                                    std::unordered_set<uint32_t>& out) {
    if (!is_cons(tmpl)) return;
    Value h = car(tmpl);
    if (!is_symbol(h)) {
        collect_binding_intros(h, pvars, out);
        collect_binding_intros(cdr(tmpl), pvars, out);
        return;
    }
    uint32_t head_sid = as_symbol_id(h);
    static const uint32_t QUOTE_SID      = intern_symbol("quote");
    static const uint32_t LAMBDA_SID     = intern_symbol("lambda");
    static const uint32_t LET_SID        = intern_symbol("let");
    static const uint32_t LETSTAR_SID    = intern_symbol("let*");
    static const uint32_t LETREC_SID     = intern_symbol("letrec");
    static const uint32_t LETRECSTAR_SID = intern_symbol("letrec*");
    static const uint32_t DEFINE_SID     = intern_symbol("define");
    if (head_sid == QUOTE_SID) return;
    Value tmpl_cdr = cdr(tmpl);
    if (head_sid == LAMBDA_SID) {
        if (is_cons(tmpl_cdr)) {
            cbi_formals(car(tmpl_cdr), pvars, out);
            collect_binding_intros(cdr(tmpl_cdr), pvars, out);
        }
        return;
    }
    if (head_sid == LET_SID   || head_sid == LETSTAR_SID ||
        head_sid == LETREC_SID || head_sid == LETRECSTAR_SID) {
        if (is_cons(tmpl_cdr)) {
            Value second     = car(tmpl_cdr);
            Value body_start = cdr(tmpl_cdr);
            Value bindings   = second;
            if (head_sid == LET_SID && is_symbol(second)) {
                uint32_t name_sid = as_symbol_id(second);
                if (!pvars.count(name_sid)) out.insert(name_sid);
                if (is_cons(body_start)) {
                    bindings   = car(body_start);
                    body_start = cdr(body_start);
                } else {
                    return;
                }
            }
            cbi_let_bindings(bindings, pvars, out);
            collect_binding_intros(body_start, pvars, out);
        }
        return;
    }
    if (head_sid == DEFINE_SID) {
        if (is_cons(tmpl_cdr)) {
            Value nameform = car(tmpl_cdr);
            if (is_symbol(nameform)) {
                uint32_t n_sid = as_symbol_id(nameform);
                if (!pvars.count(n_sid)) out.insert(n_sid);
            } else if (is_cons(nameform) && is_symbol(car(nameform))) {
                uint32_t n_sid = as_symbol_id(car(nameform));
                if (!pvars.count(n_sid)) out.insert(n_sid);
                cbi_formals(cdr(nameform), pvars, out);
            }
            collect_binding_intros(cdr(tmpl_cdr), pvars, out);
        }
        return;
    }
    collect_binding_intros(car(tmpl), pvars, out);
    collect_binding_intros(cdr(tmpl), pvars, out);
}

// ── Pattern matching ────────────────────────────────────────────────────────
// Port of syntax_rules.py _list_length_approx, _datum_equal, _match_pattern,
// _match_list_pattern, _match_vector_pattern.

static int list_length_approx(const Value& lst) {
    int n = 0;
    Value cur = lst;
    while (is_cons(cur)) { ++n; cur = cdr(cur); }
    return n;
}

static bool datum_equal(const Value& a, const Value& b) {
    if (eqv_atom(a, b)) return true;
    if (is_string(a) && is_string(b))
        return as_string(a) == as_string(b);
    if (is_cons(a) && is_cons(b))
        return datum_equal(car(a), car(b)) && datum_equal(cdr(a), cdr(b));
    if (is_nil(a) && is_nil(b)) return true;
    if (is_vector(a) && is_vector(b)) {
        const auto& ia = as_vector_items_const(a);
        const auto& ib = as_vector_items_const(b);
        if (ia.size() != ib.size()) return false;
        for (size_t i = 0; i < ia.size(); ++i)
            if (!datum_equal(ia[i], ib[i])) return false;
        return true;
    }
    return false;
}

// Forward declarations (match_pattern ↔ match_list_pattern are mutually recursive).
static bool match_pattern(const Value& pat, const Value& form,
                           const std::vector<uint32_t>& literals,
                           uint32_t ellipsis_id, SyntaxMatch& out);
static bool match_list_pattern(Value pat_list, Value form_list,
                                const std::vector<uint32_t>& literals,
                                uint32_t ellipsis_id, SyntaxMatch& out);
static bool match_vector_pattern(const std::vector<Value>& pat_items,
                                  const std::vector<Value>& form_items,
                                  const std::vector<uint32_t>& literals,
                                  uint32_t ellipsis_id, SyntaxMatch& out);

static bool match_pattern(const Value& pat, const Value& form,
                           const std::vector<uint32_t>& literals,
                           uint32_t ellipsis_id, SyntaxMatch& out) {
    static const uint32_t US = intern_symbol("_");
    if (is_symbol(pat)) {
        uint32_t sid = as_symbol_id(pat);
        if (sid == US) return true;
        for (uint32_t lit : literals) {
            if (sid == lit)
                return is_symbol(form) && as_symbol_id(form) == sid;
        }
        out.scalars[sid] = form;
        return true;
    }
    if (is_cons(pat))
        return match_list_pattern(pat, form, literals, ellipsis_id, out);
    if (is_vector(pat)) {
        if (!is_vector(form)) return false;
        return match_vector_pattern(as_vector_items_const(pat),
                                    as_vector_items_const(form),
                                    literals, ellipsis_id, out);
    }
    if (is_nil(pat)) return is_nil(form);
    return datum_equal(pat, form);
}

static bool match_list_pattern(Value pat_list, Value form_list,
                                const std::vector<uint32_t>& literals,
                                uint32_t ellipsis_id, SyntaxMatch& out) {
    while (is_cons(pat_list)) {
        Value pat_elem = car(pat_list);
        Value pat_rest = cdr(pat_list);
        bool has_ell = is_cons(pat_rest) && is_ellipsis_sym(car(pat_rest), ellipsis_id);
        if (has_ell) {
            Value suffix_pat = cdr(pat_rest);
            int   suffix_need = list_length_approx(suffix_pat);
            std::vector<Value> form_vec;
            {
                Value tmp = form_list;
                while (is_cons(tmp)) { form_vec.push_back(car(tmp)); tmp = cdr(tmp); }
            }
            int total = (int)form_vec.size();
            if (total < suffix_need) return false;
            int n_ellipsis = total - suffix_need;
            std::unordered_map<uint32_t, int> pvar_depths;
            collect_pvars_with_depth(pat_elem, literals, ellipsis_id, pvar_depths, 0);
            for (auto& [pv, d] : pvar_depths) {
                out.ellipsis[pv] = {};
                out.ell_depth[pv] = d + 1;
            }
            for (int i = 0; i < n_ellipsis; ++i) {
                SyntaxMatch sub;
                if (!match_pattern(pat_elem, form_vec[i], literals, ellipsis_id, sub))
                    return false;
                for (auto& [k, v] : sub.scalars)
                    out.ellipsis[k].push_back(v);
                for (auto& [k, vlist] : sub.ellipsis)
                    out.ellipsis[k].push_back(make_vector(vlist));
            }
            Value suffix_form = NIL_VALUE;
            for (int j = total - 1; j >= n_ellipsis; --j)
                suffix_form = alloc_cons(form_vec[j], suffix_form);
            return match_list_pattern(suffix_pat, suffix_form, literals, ellipsis_id, out);
        }
        if (!is_cons(form_list)) return false;
        if (!match_pattern(pat_elem, car(form_list), literals, ellipsis_id, out))
            return false;
        pat_list  = pat_rest;
        form_list = cdr(form_list);
    }
    if (is_nil(pat_list))  return is_nil(form_list);
    if (is_symbol(pat_list)) {
        uint32_t sid = as_symbol_id(pat_list);
        for (uint32_t lit : literals)
            if (sid == lit) return false;
        out.scalars[sid] = form_list;
        return true;
    }
    return false;
}

static bool match_vector_pattern(const std::vector<Value>& pat_items,
                                  const std::vector<Value>& form_items,
                                  const std::vector<uint32_t>& literals,
                                  uint32_t ellipsis_id, SyntaxMatch& out) {
    int i = 0, j = 0;
    int n_pat  = (int)pat_items.size();
    int n_form = (int)form_items.size();
    while (i < n_pat) {
        const Value& pat_elem = pat_items[i];
        bool has_ell = (i + 1 < n_pat && is_ellipsis_sym(pat_items[i + 1], ellipsis_id));
        if (has_ell) {
            int suffix_count = n_pat - (i + 2);
            int available    = n_form - j;
            if (available < suffix_count) return false;
            int n_ellipsis = available - suffix_count;
            std::unordered_map<uint32_t, int> pvar_depths;
            collect_pvars_with_depth(pat_elem, literals, ellipsis_id, pvar_depths, 0);
            for (auto& [pv, d] : pvar_depths) {
                out.ellipsis[pv]  = {};
                out.ell_depth[pv] = d + 1;
            }
            for (int k = 0; k < n_ellipsis; ++k) {
                SyntaxMatch sub;
                if (!match_pattern(pat_elem, form_items[j + k], literals, ellipsis_id, sub))
                    return false;
                for (auto& [key, v] : sub.scalars)
                    out.ellipsis[key].push_back(v);
                for (auto& [key, vlist] : sub.ellipsis)
                    out.ellipsis[key].push_back(make_vector(vlist));
            }
            j += n_ellipsis;
            i += 2;
            continue;
        }
        if (j >= n_form) return false;
        if (!match_pattern(pat_elem, form_items[j], literals, ellipsis_id, out))
            return false;
        ++i; ++j;
    }
    return j == n_form;
}

// ── Template instantiation ──────────────────────────────────────────────────
// Port of syntax_rules.py _collect_ell_refs, _raise_syntax_error,
// _instantiate_vector, _instantiate_list, _instantiate.

static void collect_ell_refs(const Value& tmpl, const SyntaxMatch& match,
                              std::vector<uint32_t>& out) {
    if (is_symbol(tmpl)) {
        uint32_t sid = as_symbol_id(tmpl);
        if (match.ellipsis.count(sid)) out.push_back(sid);
        return;
    }
    if (is_cons(tmpl)) {
        collect_ell_refs(car(tmpl), match, out);
        collect_ell_refs(cdr(tmpl), match, out);
    }
    if (is_vector(tmpl))
        for (const Value& item : as_vector_items_const(tmpl))
            collect_ell_refs(item, match, out);
}

// Forward declarations (instantiate ↔ instantiate_list are mutually recursive).
static Value instantiate(const Value& tmpl, const SyntaxMatch& match,
                          uint32_t ellipsis_id, SourceInfo* use_src,
                          const std::unordered_map<uint32_t, uint32_t>& free_id_map);
static Value instantiate_list(Value tmpl_list, const SyntaxMatch& match,
                               uint32_t ellipsis_id, SourceInfo* use_src,
                               const std::unordered_map<uint32_t, uint32_t>& free_id_map);

[[noreturn]] static void raise_syntax_error(
    const Value& args_tail, const SyntaxMatch& match,
    uint32_t ellipsis_id, SourceInfo* use_src,
    const std::unordered_map<uint32_t, uint32_t>& free_id_map) {
    std::vector<Value> args;
    Value cur = args_tail;
    while (is_cons(cur)) {
        args.push_back(instantiate(car(cur), match, ellipsis_id, use_src, free_id_map));
        cur = cdr(cur);
    }
    std::string msg;
    size_t di;
    if (!args.empty() && is_string(args[0])) {
        msg = as_string(args[0]);
        di  = 1;
    } else {
        msg = "syntax-error";
        di  = 0;
    }
    if (di < args.size()) {
        std::string extras;
        while (di < args.size()) {
            if (!extras.empty()) extras += ' ';
            extras += scheme_pretty_print(args[di++]);
        }
        msg += ": " + extras;
    }
    throw SchemeSyntaxError(msg, use_src ? new SourceInfo(*use_src) : nullptr);
}

static Value instantiate_vector(const std::vector<Value>& tmpl_items,
                                  const SyntaxMatch& match,
                                  uint32_t ellipsis_id,
                                  SourceInfo* use_src,
                                  const std::unordered_map<uint32_t, uint32_t>& free_id_map) {
    std::vector<Value> output;
    int i = 0;
    int n = (int)tmpl_items.size();
    while (i < n) {
        const Value& elem = tmpl_items[i];
        bool has_ell = (i + 1 < n && is_ellipsis_sym(tmpl_items[i + 1], ellipsis_id));
        if (has_ell) {
            std::vector<uint32_t> ell_syms;
            collect_ell_refs(elem, match, ell_syms);
            if (!ell_syms.empty()) {
                int count = (int)match.ellipsis.at(ell_syms[0]).size();
                for (int k = 0; k < count; ++k) {
                    SyntaxMatch sub;
                    sub.scalars   = match.scalars;
                    sub.ellipsis  = match.ellipsis;
                    sub.ell_depth = match.ell_depth;
                    for (uint32_t sv : ell_syms) {
                        int d = match.ell_depth.count(sv) ? match.ell_depth.at(sv) : 0;
                        const Value& peeled = match.ellipsis.at(sv)[k];
                        if (d == 1) {
                            sub.scalars[sv] = peeled;
                            sub.ellipsis.erase(sv);
                            sub.ell_depth.erase(sv);
                        } else {
                            sub.ellipsis[sv]  = as_vector_items_const(peeled);
                            sub.ell_depth[sv] = d - 1;
                        }
                    }
                    output.push_back(instantiate(elem, sub, ellipsis_id, use_src, free_id_map));
                }
            }
            i += 2;
            continue;
        }
        output.push_back(instantiate(elem, match, ellipsis_id, use_src, free_id_map));
        ++i;
    }
    return make_vector(std::move(output));
}

static Value instantiate_list(Value tmpl_list, const SyntaxMatch& match,
                               uint32_t ellipsis_id, SourceInfo* use_src,
                               const std::unordered_map<uint32_t, uint32_t>& free_id_map) {
    std::vector<Value> output;
    Value cur = tmpl_list;
    while (is_cons(cur)) {
        Value elem = car(cur);
        Value rest = cdr(cur);
        bool has_ell = is_cons(rest) && is_ellipsis_sym(car(rest), ellipsis_id);
        if (has_ell) {
            std::vector<uint32_t> ell_syms;
            collect_ell_refs(elem, match, ell_syms);
            if (!ell_syms.empty()) {
                int n = (int)match.ellipsis.at(ell_syms[0]).size();
                for (int idx = 0; idx < n; ++idx) {
                    SyntaxMatch sub;
                    sub.scalars   = match.scalars;
                    sub.ellipsis  = match.ellipsis;
                    sub.ell_depth = match.ell_depth;
                    for (uint32_t sv : ell_syms) {
                        int d = match.ell_depth.count(sv) ? match.ell_depth.at(sv) : 0;
                        const Value& peeled = match.ellipsis.at(sv)[idx];
                        if (d == 1) {
                            sub.scalars[sv] = peeled;
                            sub.ellipsis.erase(sv);
                            sub.ell_depth.erase(sv);
                        } else {
                            sub.ellipsis[sv]  = as_vector_items_const(peeled);
                            sub.ell_depth[sv] = d - 1;
                        }
                    }
                    output.push_back(instantiate(elem, sub, ellipsis_id, use_src, free_id_map));
                }
            }
            cur = cdr(rest);
            continue;
        }
        output.push_back(instantiate(elem, match, ellipsis_id, use_src, free_id_map));
        cur = rest;
    }
    Value tail = is_nil(cur) ? NIL_VALUE
                             : instantiate(cur, match, ellipsis_id, use_src, free_id_map);
    Value result = tail;
    for (int i = (int)output.size() - 1; i >= 0; --i)
        result = alloc_cons(output[i], result,
                            use_src ? new SourceInfo(*use_src) : nullptr);
    return result;
}

static Value instantiate(const Value& tmpl, const SyntaxMatch& match,
                          uint32_t ellipsis_id, SourceInfo* use_src,
                          const std::unordered_map<uint32_t, uint32_t>& free_id_map) {
    if (is_symbol(tmpl)) {
        uint32_t sid = as_symbol_id(tmpl);
        auto it = match.scalars.find(sid);
        if (it != match.scalars.end()) return it->second;
        auto it2 = free_id_map.find(sid);
        if (it2 != free_id_map.end())
            return make_symbol_id(it2->second, src_of(tmpl));
        return tmpl;
    }
    if (is_cons(tmpl)) {
        // R7RS §4.3.2: (ellipsis inner) disables ellipsis inside inner.
        if (is_ellipsis_sym(car(tmpl), ellipsis_id) &&
            is_cons(cdr(tmpl)) && is_nil(cdr(cdr(tmpl))))
            return instantiate(car(cdr(tmpl)), match, get_no_ellipsis_id(),
                               use_src, free_id_map);
        static const uint32_t SYNTAX_ERROR_SID = intern_symbol("syntax-error");
        if (is_symbol(car(tmpl)) && as_symbol_id(car(tmpl)) == SYNTAX_ERROR_SID)
            raise_syntax_error(cdr(tmpl), match, ellipsis_id, use_src, free_id_map);
        return instantiate_list(tmpl, match, ellipsis_id, use_src, free_id_map);
    }
    if (is_vector(tmpl))
        return instantiate_vector(as_vector_items_const(tmpl), match,
                                  ellipsis_id, use_src, free_id_map);
    return tmpl;
}

// ── apply_syntax_transformer ────────────────────────────────────────────────
// Port of syntax_rules.py apply_syntax_transformer.

Value apply_syntax_transformer(const Value& t_val, const Value& form) {
    const auto& literals       = as_syntax_transformer_literals(t_val);
    uint32_t    ellipsis_id    = as_syntax_transformer_ellipsis(t_val);
    const auto& binding_intros = as_syntax_transformer_binding_intro_names(t_val);
    SourceInfo* use_src        = src_of(form);

    // Copy base free_id_map; add per-application gensyms for binding-site intros.
    std::unordered_map<uint32_t, uint32_t> free_id_map =
        as_syntax_transformer_free_id_map(t_val);
    for (uint32_t iname_id : binding_intros) {
        if (!free_id_map.count(iname_id))
            free_id_map[iname_id] = intern_symbol(hygiene_gensym(symbol_name(iname_id)));
    }

    Value form_tail = is_cons(form) ? cdr(form) : NIL_VALUE;

    const auto& rules = as_syntax_transformer_rules(t_val);
    for (const auto& rule : rules) {
        if (is_cons(rule.pattern)) {
            SyntaxMatch match;
            if (match_list_pattern(cdr(rule.pattern), form_tail,
                                   literals, ellipsis_id, match))
                return instantiate(rule.tmpl, match, ellipsis_id, use_src, free_id_map);
        }
    }
    throw SchemeSyntaxError(
        "syntax-rules: no matching pattern for '" +
        as_syntax_transformer_name(t_val) + "'",
        use_src ? new SourceInfo(*use_src) : nullptr);
}

// ── parse_syntax_rules ──────────────────────────────────────────────────────
// Port of syntax_rules.py parse_syntax_rules.

Value parse_syntax_rules(Value tail, Environment* def_env, const std::string& name) {
    if (!is_cons(tail)) {
        SourceInfo* s = src_of(tail);
        throw SchemeSyntaxError("syntax-rules: malformed",
                                s ? new SourceInfo(*s) : nullptr);
    }

    static const uint32_t DEFAULT_ELLIPSIS_ID = intern_symbol("...");
    static const uint32_t US                  = intern_symbol("_");

    uint32_t ellipsis_id = DEFAULT_ELLIPSIS_ID;
    Value    lit_list;
    Value    rules_list;

    Value first = car(tail);
    Value rest  = cdr(tail);
    if (is_symbol(first) && is_cons(rest)) {
        Value second = car(rest);
        if (is_nil(second) || is_cons(second)) {
            ellipsis_id = as_symbol_id(first);
            lit_list    = second;
            rules_list  = cdr(rest);
        } else {
            lit_list   = first;
            rules_list = rest;
        }
    } else {
        lit_list   = first;
        rules_list = rest;
    }

    std::vector<uint32_t> literals;
    {
        Value cur = lit_list;
        while (is_cons(cur)) {
            Value elem = car(cur);
            if (!is_symbol(elem)) {
                SourceInfo* s = src_of(elem);
                throw SchemeSyntaxError(
                    "syntax-rules: literal must be a symbol",
                    s ? new SourceInfo(*s) : nullptr);
            }
            uint32_t lit_sid = as_symbol_id(elem);
            if (lit_sid == US || lit_sid == ellipsis_id) {
                SourceInfo* s = src_of(elem);
                throw SchemeSyntaxError(
                    "syntax-rules: '" + symbol_name(lit_sid) + "' cannot appear in literals list",
                    s ? new SourceInfo(*s) : nullptr);
            }
            literals.push_back(lit_sid);
            cur = cdr(cur);
        }
    }

    std::vector<SyntaxTransformer::Rule>                              rules;
    std::unordered_set<uint32_t>                                      pvars_union;
    std::vector<std::pair<Value, std::unordered_set<uint32_t>>>       templates;

    {
        Value cur = rules_list;
        while (is_cons(cur)) {
            Value rule = car(cur);
            if (!is_cons(rule) || !is_cons(cdr(rule))) {
                SourceInfo* s = src_of(rule);
                throw SchemeSyntaxError(
                    "syntax-rules: each rule must be (pattern template)",
                    s ? new SourceInfo(*s) : nullptr);
            }
            Value pattern = car(rule);
            Value tmpl    = car(cdr(rule));
            std::unordered_set<uint32_t> pvars;
            if (is_cons(pattern))
                collect_pvars(cdr(pattern), literals, ellipsis_id, pvars);
            for (uint32_t pv : pvars) pvars_union.insert(pv);
            SyntaxTransformer::Rule r;
            r.pattern = pattern;
            r.tmpl    = tmpl;
            rules.push_back(r);
            templates.emplace_back(tmpl, pvars);
            cur = cdr(cur);
        }
    }

    // Collect all free identifiers across all templates.
    std::unordered_set<uint32_t> free_ids;
    for (auto& [tmpl, pvars] : templates)
        collect_free_ids(tmpl, pvars, literals, ellipsis_id, free_ids);

    // Collect binding-position intro names.
    std::unordered_set<uint32_t> binding_intros;
    for (auto& [tmpl, pvars] : templates)
        collect_binding_intros(tmpl, pvars, binding_intros);

    // Resolve free ids against definition environment.
    std::unordered_map<uint32_t, uint32_t> free_id_map;
    std::unordered_set<uint32_t>           intro_names;
    if (def_env) {
        for (uint32_t fid : free_ids) {
            if (def_env->lookup_optional_id(fid).has_value()) {
                uint32_t gs_id = intern_symbol(hygiene_gensym(symbol_name(fid)));
                free_id_map[fid] = gs_id;
            } else {
                intro_names.insert(fid);
            }
        }
    } else {
        for (uint32_t fid : free_ids)
            intro_names.insert(fid);
    }

    // binding_intro_names = intro_names that appear in binding positions.
    std::unordered_set<uint32_t> binding_intro_names;
    for (uint32_t n : intro_names)
        if (binding_intros.count(n))
            binding_intro_names.insert(n);

    // Bind each free_id alias in the global env so it persists.
    if (!free_id_map.empty() && def_env) {
        Environment* global_env = def_env->getGlobalEnv();
        for (auto& [fid, gs_id] : free_id_map)
            global_env->bind_id(gs_id, def_env->lookup_id(fid));
    }

    return make_syntax_transformer(name,
                                   std::move(literals),
                                   ellipsis_id,
                                   std::move(rules),
                                   std::move(free_id_map),
                                   std::move(intro_names),
                                   std::move(binding_intro_names));
}
