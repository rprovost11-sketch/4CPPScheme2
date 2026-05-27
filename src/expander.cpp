// Expander.cpp -- S-expression expander: rewrites sugar and user macros.
// Direct port of pyscheme/Expander.py.
#include "Expander.h"
#include "Parser.h"
#include "library.h"
#include "syntax_rules.h"
#include "gc.h"
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

// ── Module state ──────────────────────────────────────────────────────────────

static Environment* g_runtime_env          = nullptr;
static int          g_expand_depth         = 0;
static std::string  g_include_fallback_dir;

static constexpr int MAX_EXPAND_ITER  = 200;
static constexpr int MAX_EXPAND_DEPTH = 500;

using RenameTable = std::unordered_map<uint32_t, uint32_t>;

// ── Forward declarations ──────────────────────────────────────────────────────

static Value expand_inner(Value sexpr);
static Value expand_list(const Value& cell);
static Value expand_define_syntax(const Value& sexpr);
static Value expand_let_syntax(const Value& sexpr, bool is_letrec);
static Value rename_refs_in_form(const Value& form, const RenameTable& table);
static Value expand_body(const Value& body_cons);
static int   list_length(const Value& cell);
static bool  is_head(const Value& form, uint32_t name_id);
static Value map_list_cars(const Value& cons_list,
                            const std::function<Value(const Value&)>& fn);

// ── Keyword ID cache ──────────────────────────────────────────────────────────

static std::once_flag s_init_flag;
static std::unordered_map<uint32_t, std::function<Value(const Value&)>> s_sugar_handlers;

// Pre-interned IDs used in hot paths
static uint32_t sid_define_syntax;
static uint32_t sid_let_syntax;
static uint32_t sid_letrec_syntax;
static uint32_t sid_define;
static uint32_t sid_lambda;
static uint32_t sid_let;
static uint32_t sid_let_star;
static uint32_t sid_letrec;
static uint32_t sid_letrec_star;
static uint32_t sid_when;
static uint32_t sid_unless;
static uint32_t sid_case_lambda;
static uint32_t sid_if;
static uint32_t sid_quote;
static uint32_t sid_do;
static uint32_t sid_include;
static uint32_t sid_include_ci;
static uint32_t sid_cond_expand;
static uint32_t sid_quasiquote;
static uint32_t sid_let_values;
static uint32_t sid_let_star_values;
static uint32_t sid_define_values;
static uint32_t sid_define_record_type;
static uint32_t sid_parameterize;
static uint32_t sid_guard;
static uint32_t sid_define_library;
static uint32_t sid_import;
static uint32_t sid_begin;
static uint32_t sid_begin_kw;   // same as sid_begin; alias for clarity in body scan
static uint32_t sid_else;
static uint32_t sid_unquote;
static uint32_t sid_quasiquote_kw;
static uint32_t sid_unquote_splicing;
static uint32_t sid_syntax_rules;
static uint32_t sid_and_kw;
static uint32_t sid_or_kw;
static uint32_t sid_not_kw;
static uint32_t sid_library_kw;

// Forward declarations of sugar handlers needed for init
static Value expand_define(const Value& sexpr);
static Value expand_lambda(const Value& sexpr);
static Value expand_let_family(const Value& sexpr, const std::string& head_name);
static Value expand_let(const Value& sexpr);
static Value expand_let_star(const Value& sexpr);
static Value expand_letrec(const Value& sexpr);
static Value expand_letrec_star(const Value& sexpr);
static Value expand_when(const Value& sexpr);
static Value expand_unless(const Value& sexpr);
static Value expand_case_lambda(const Value& sexpr);
static Value expand_if(const Value& sexpr);
static Value expand_quote(const Value& sexpr);
static Value expand_do(const Value& sexpr);
static Value expand_include_form(const Value& sexpr, bool fold);
static Value expand_include(const Value& sexpr);
static Value expand_include_ci(const Value& sexpr);
static Value expand_cond_expand(const Value& sexpr);
static Value expand_quasiquote(const Value& sexpr);
static Value expand_let_values(const Value& sexpr);
static Value expand_let_star_values(const Value& sexpr);
static Value expand_define_values(const Value& sexpr);
static Value expand_define_record_type(const Value& sexpr);
static Value expand_parameterize(const Value& sexpr);
static Value expand_guard(const Value& sexpr);
static Value expand_define_library(const Value& sexpr);
static Value expand_import(const Value& sexpr);

static void init_sugar_handlers() {
    sid_define_syntax    = intern_symbol("define-syntax");
    sid_let_syntax       = intern_symbol("let-syntax");
    sid_letrec_syntax    = intern_symbol("letrec-syntax");
    sid_define           = intern_symbol("define");
    sid_lambda           = intern_symbol("lambda");
    sid_let              = intern_symbol("let");
    sid_let_star         = intern_symbol("let*");
    sid_letrec           = intern_symbol("letrec");
    sid_letrec_star      = intern_symbol("letrec*");
    sid_when             = intern_symbol("when");
    sid_unless           = intern_symbol("unless");
    sid_case_lambda      = intern_symbol("case-lambda");
    sid_if               = intern_symbol("if");
    sid_quote            = intern_symbol("quote");
    sid_do               = intern_symbol("do");
    sid_include          = intern_symbol("include");
    sid_include_ci       = intern_symbol("include-ci");
    sid_cond_expand      = intern_symbol("cond-expand");
    sid_quasiquote       = intern_symbol("quasiquote");
    sid_let_values       = intern_symbol("let-values");
    sid_let_star_values  = intern_symbol("let*-values");
    sid_define_values    = intern_symbol("define-values");
    sid_define_record_type = intern_symbol("define-record-type");
    sid_parameterize     = intern_symbol("parameterize");
    sid_guard            = intern_symbol("guard");
    sid_define_library   = intern_symbol("define-library");
    sid_import           = intern_symbol("import");
    sid_begin            = intern_symbol("begin");
    sid_begin_kw         = sid_begin;
    sid_else             = intern_symbol("else");
    sid_unquote          = intern_symbol("unquote");
    sid_quasiquote_kw    = sid_quasiquote;
    sid_unquote_splicing = intern_symbol("unquote-splicing");
    sid_syntax_rules     = intern_symbol("syntax-rules");
    sid_and_kw           = intern_symbol("and");
    sid_or_kw            = intern_symbol("or");
    sid_not_kw           = intern_symbol("not");
    sid_library_kw       = intern_symbol("library");

    s_sugar_handlers[sid_define]           = expand_define;
    s_sugar_handlers[sid_lambda]           = expand_lambda;
    s_sugar_handlers[sid_let]              = expand_let;
    s_sugar_handlers[sid_let_star]         = expand_let_star;
    s_sugar_handlers[sid_letrec]           = expand_letrec;
    s_sugar_handlers[sid_letrec_star]      = expand_letrec_star;
    s_sugar_handlers[sid_when]             = expand_when;
    s_sugar_handlers[sid_unless]           = expand_unless;
    s_sugar_handlers[sid_case_lambda]      = expand_case_lambda;
    s_sugar_handlers[sid_if]               = expand_if;
    s_sugar_handlers[sid_quote]            = expand_quote;
    s_sugar_handlers[sid_do]               = expand_do;
    s_sugar_handlers[sid_include]          = expand_include;
    s_sugar_handlers[sid_include_ci]       = expand_include_ci;
    s_sugar_handlers[sid_cond_expand]      = expand_cond_expand;
    s_sugar_handlers[sid_quasiquote]       = expand_quasiquote;
    s_sugar_handlers[sid_let_values]       = expand_let_values;
    s_sugar_handlers[sid_let_star_values]  = expand_let_star_values;
    s_sugar_handlers[sid_define_values]    = expand_define_values;
    s_sugar_handlers[sid_define_record_type] = expand_define_record_type;
    s_sugar_handlers[sid_parameterize]     = expand_parameterize;
    s_sugar_handlers[sid_guard]            = expand_guard;
    s_sugar_handlers[sid_define_library]   = expand_define_library;
    s_sugar_handlers[sid_import]           = expand_import;
}

// ── Public API ────────────────────────────────────────────────────────────────

void set_runtime_env(Environment* env) {
    static bool s_registered = false;
    if (!s_registered) { gc_env_root_push(&g_runtime_env); s_registered = true; }
    g_runtime_env = env;
}
Environment* get_runtime_env() { return g_runtime_env; }
void        set_include_fallback_dir(const std::string& dir) { g_include_fallback_dir = dir; }
std::string get_include_fallback_dir()                       { return g_include_fallback_dir; }

// ── Helpers ───────────────────────────────────────────────────────────────────

static int list_length(const Value& cell) {
    int n = 0;
    Value cur = cell;
    while (is_cons(cur)) { n++; cur = cdr(cur); }
    return is_nil(cur) ? n : -1;
}

static bool is_head(const Value& form, uint32_t name_id) {
    return is_cons(form) && is_symbol(car(form)) && as_symbol_id(car(form)) == name_id;
}

static Value lookup_macro(const Value& sym) {
    if (g_runtime_env == nullptr) return NIL_VALUE;
    auto opt = g_runtime_env->lookup_optional_id(as_symbol_id(sym));
    if (opt && is_syntax_transformer(*opt)) return *opt;
    return NIL_VALUE;
}

// ── Alpha-rename helpers ──────────────────────────────────────────────────────

static std::unordered_set<uint32_t> collect_formals_names(const Value& formals) {
    std::unordered_set<uint32_t> names;
    Value cur = formals;
    while (is_cons(cur)) {
        if (is_symbol(car(cur))) names.insert(as_symbol_id(car(cur)));
        cur = cdr(cur);
    }
    if (is_symbol(cur)) names.insert(as_symbol_id(cur));
    return names;
}

static std::unordered_set<uint32_t> collect_let_bound_names(const Value& bindings) {
    std::unordered_set<uint32_t> names;
    Value cur = bindings;
    while (is_cons(cur)) {
        Value pair = car(cur);
        if (is_cons(pair) && is_symbol(car(pair)))
            names.insert(as_symbol_id(car(pair)));
        cur = cdr(cur);
    }
    return names;
}

static RenameTable mask_table(const RenameTable& table,
                               const std::unordered_set<uint32_t>& bound) {
    if (bound.empty()) return table;
    RenameTable result;
    for (const auto& [k, v] : table)
        if (bound.find(k) == bound.end()) result[k] = v;
    return result;
}

static Value map_list_cars(const Value& cons_list,
                            const std::function<Value(const Value&)>& fn) {
    std::vector<Value> new_items;
    std::vector<SourceInfo*> new_srcs;
    Value cur = cons_list;
    while (is_cons(cur)) {
        new_items.push_back(fn(car(cur)));
        new_srcs.push_back(src_of(cur));
        cur = cdr(cur);
    }
    Value tail = cur;  // improper tail or NIL
    Value result = tail;
    for (int i = (int)new_items.size() - 1; i >= 0; i--)
        result = alloc_cons(new_items[i], result, new_srcs[i]);
    return result;
}

static Value gensym_rename_formals(const Value& formals, RenameTable& table) {
    if (is_symbol(formals)) {
        std::string name = as_symbol(formals);
        std::string gs   = hygiene_gensym(name);
        table[intern_symbol(name)] = intern_symbol(gs);
        return make_symbol(gs, nullptr);
    }
    if (is_nil(formals)) return formals;
    std::unordered_set<uint32_t> seen;
    std::vector<Value> items;
    Value cur = formals;
    while (is_cons(cur)) {
        Value sym = car(cur);
        if (is_symbol(sym)) {
            uint32_t sid = as_symbol_id(sym);
            if (seen.count(sid))
                throw SchemeSyntaxError("duplicate parameter name in lambda: " +
                                        symbol_name(sid), src_of(sym));
            seen.insert(sid);
            std::string gs = hygiene_gensym(symbol_name(sid));
            table[sid] = intern_symbol(gs);
            items.push_back(make_symbol(gs, nullptr));
        } else {
            items.push_back(sym);
        }
        cur = cdr(cur);
    }
    Value tail;
    if (is_nil(cur)) {
        tail = NIL_VALUE;
    } else if (is_symbol(cur)) {
        uint32_t sid = as_symbol_id(cur);
        if (seen.count(sid))
            throw SchemeSyntaxError("rest parameter name conflicts with fixed parameter: " +
                                    symbol_name(sid), src_of(cur));
        std::string gs = hygiene_gensym(symbol_name(sid));
        table[sid] = intern_symbol(gs);
        tail = make_symbol(gs, nullptr);
    } else {
        tail = cur;
    }
    Value result = tail;
    for (int i = (int)items.size() - 1; i >= 0; i--)
        result = alloc_cons(items[i], result, src_of(formals));
    return result;
}

// rrif helpers - forward declarations
static Value rrif_lambda(const Value& form, const RenameTable& table);
static Value rrif_let(const Value& form, const RenameTable& table, uint32_t kind_id);
static Value rrif_case_lambda(const Value& form, const RenameTable& table);
static Value rrif_case(const Value& form, const RenameTable& table);
static Value rrif_let_syntax(const Value& form, const RenameTable& table, bool is_letrec);
static Value rrif_default(const Value& form, const RenameTable& table);
static Value rrif_bindings_parallel(const Value& bindings, const RenameTable& table);
static Value rrif_bindings_let_star(const Value& bindings, RenameTable table);

static Value rename_refs_in_form(const Value& form, const RenameTable& table) {
    if (table.empty()) return form;
    if (is_symbol(form)) {
        auto it = table.find(as_symbol_id(form));
        if (it != table.end()) return make_symbol_id(it->second, src_of(form));
        return form;
    }
    if (is_vector(form)) {
        const auto& items = as_vector_items_const(form);
        std::vector<Value> new_items;
        new_items.reserve(items.size());
        for (const Value& item : items)
            new_items.push_back(rename_refs_in_form(item, table));
        return make_vector(std::move(new_items));
    }
    if (!is_cons(form)) return form;
    Value head = car(form);
    if (is_symbol(head)) {
        uint32_t hid = as_symbol_id(head);
        if (hid == sid_quote) {
            auto it = table.find(hid);
            if (it == table.end()) return form;
            // 'quote' is locally bound; rename head only, leave datum untouched.
            Value new_head = make_symbol_id(it->second, src_of(head));
            return alloc_cons(new_head, cdr(form), src_of(form));
        }
        if (hid == sid_lambda)        return rrif_lambda(form, table);
        if (hid == sid_let   || hid == sid_let_star ||
            hid == sid_letrec || hid == sid_letrec_star)
            return rrif_let(form, table, hid);
        if (hid == sid_case_lambda)   return rrif_case_lambda(form, table);
        if (hid == intern_symbol("case")) return rrif_case(form, table);
        if (hid == sid_let_syntax || hid == sid_letrec_syntax)
            return rrif_let_syntax(form, table, hid == sid_letrec_syntax);
    }
    Value new_head = rename_refs_in_form(head, table);
    Value new_cdr  = rename_refs_in_form(cdr(form), table);
    return alloc_cons(new_head, new_cdr, src_of(form));
}

static Value rrif_lambda(const Value& form, const RenameTable& table) {
    if (!is_cons(cdr(form))) return rrif_default(form, table);
    Value formals   = car(cdr(form));
    Value body_cons = cdr(cdr(form));
    RenameTable inner = mask_table(table, collect_formals_names(formals));
    Value new_body = rename_refs_in_form(body_cons, inner);
    return alloc_cons(car(form), alloc_cons(formals, new_body, src_of(cdr(form))), src_of(form));
}

static Value rrif_let(const Value& form, const RenameTable& table, uint32_t kind_id) {
    Value head = car(form);
    if (!is_cons(cdr(form))) return rrif_default(form, table);
    // Named let: (let loop_sym bindings body...)
    if (kind_id == sid_let && is_symbol(car(cdr(form)))) {
        if (!is_cons(cdr(cdr(form)))) return rrif_default(form, table);
        Value loop_sym  = car(cdr(form));
        Value bindings  = car(cdr(cdr(form)));
        Value body_cons = cdr(cdr(cdr(form)));
        auto bound = collect_let_bound_names(bindings);
        bound.insert(as_symbol_id(loop_sym));
        Value new_bindings = rrif_bindings_parallel(bindings, table);
        Value new_body     = rename_refs_in_form(body_cons, mask_table(table, bound));
        Value rest = alloc_cons(new_bindings, new_body, src_of(cdr(cdr(form))));
        return alloc_cons(head, alloc_cons(loop_sym, rest, src_of(cdr(form))), src_of(form));
    }
    Value bindings  = car(cdr(form));
    Value body_cons = cdr(cdr(form));
    RenameTable body_table = mask_table(table, collect_let_bound_names(bindings));
    Value new_bindings;
    if (kind_id == sid_let)
        new_bindings = rrif_bindings_parallel(bindings, table);
    else if (kind_id == sid_let_star)
        new_bindings = rrif_bindings_let_star(bindings, table);
    else
        new_bindings = rrif_bindings_parallel(bindings, body_table);
    Value new_body = rename_refs_in_form(body_cons, body_table);
    return alloc_cons(head, alloc_cons(new_bindings, new_body, src_of(cdr(form))), src_of(form));
}

static Value rrif_case_lambda(const Value& form, const RenameTable& table) {
    auto rename_clause = [&table](const Value& clause) -> Value {
        if (!is_cons(clause)) return clause;
        Value formals   = car(clause);
        Value body_cons = cdr(clause);
        RenameTable inner = mask_table(table, collect_formals_names(formals));
        Value new_body = rename_refs_in_form(body_cons, inner);
        return alloc_cons(formals, new_body, src_of(clause));
    };
    Value new_cdr = map_list_cars(cdr(form), rename_clause);
    return alloc_cons(car(form), new_cdr, src_of(form));
}

static Value rrif_default(const Value& form, const RenameTable& table) {
    Value new_head = rename_refs_in_form(car(form), table);
    Value new_cdr  = rename_refs_in_form(cdr(form), table);
    return alloc_cons(new_head, new_cdr, src_of(form));
}

static Value rrif_case(const Value& form, const RenameTable& table) {
    if (!is_cons(cdr(form))) return rrif_default(form, table);
    Value new_key = rename_refs_in_form(car(cdr(form)), table);
    auto rename_clause = [&table](const Value& clause) -> Value {
        if (!is_cons(clause)) return clause;
        Value head = car(clause);
        Value body = cdr(clause);
        Value new_head = is_symbol(head) ? rename_refs_in_form(head, table) : head;
        Value new_body = rename_refs_in_form(body, table);
        return alloc_cons(new_head, new_body, src_of(clause));
    };
    Value new_clauses = map_list_cars(cdr(cdr(form)), rename_clause);
    return alloc_cons(car(form), alloc_cons(new_key, new_clauses, src_of(cdr(form))), src_of(form));
}

static Value rrif_let_syntax(const Value& form, const RenameTable& table, bool is_letrec) {
    if (!is_cons(cdr(form))) return rrif_default(form, table);
    Value bindings  = car(cdr(form));
    Value body_cons = cdr(cdr(form));
    RenameTable body_table = mask_table(table, collect_let_bound_names(bindings));
    const RenameTable& tr_table = is_letrec ? body_table : table;
    auto rename_binding = [&tr_table](const Value& b) -> Value {
        if (is_cons(b) && is_symbol(car(b)) && is_cons(cdr(b))) {
            Value new_tr = rename_refs_in_form(car(cdr(b)), tr_table);
            return alloc_cons(car(b), alloc_cons(new_tr, cdr(cdr(b)), src_of(cdr(b))), src_of(b));
        }
        return b;
    };
    Value new_bindings = map_list_cars(bindings, rename_binding);
    Value new_body     = rename_refs_in_form(body_cons, body_table);
    Value new_cdr = alloc_cons(new_bindings, new_body, src_of(cdr(form)));
    return alloc_cons(car(form), new_cdr, src_of(form));
}

static Value rrif_bindings_parallel(const Value& bindings, const RenameTable& table) {
    if (table.empty()) return bindings;
    auto rename_pair = [&table](const Value& pair) -> Value {
        if (is_cons(pair) && is_cons(cdr(pair))) {
            Value new_init = rename_refs_in_form(car(cdr(pair)), table);
            return alloc_cons(car(pair),
                              alloc_cons(new_init, cdr(cdr(pair)), src_of(cdr(pair))), src_of(pair));
        }
        return pair;
    };
    return map_list_cars(bindings, rename_pair);
}

static Value rrif_bindings_let_star(const Value& bindings, RenameTable table) {
    // table is passed by value; the lambda mutates it as bindings are processed.
    if (table.empty()) return bindings;
    auto rename_pair = [&table](const Value& pair) -> Value {
        if (is_cons(pair) && is_symbol(car(pair)) && is_cons(cdr(pair))) {
            uint32_t name_id = as_symbol_id(car(pair));
            Value new_init = rename_refs_in_form(car(cdr(pair)), table);
            table.erase(name_id);
            return alloc_cons(car(pair),
                              alloc_cons(new_init, cdr(cdr(pair)), src_of(cdr(pair))), src_of(pair));
        }
        return pair;
    };
    return map_list_cars(bindings, rename_pair);
}

// ── Body expansion ────────────────────────────────────────────────────────────

static void ensure_local_env(bool& env_created, Environment*& saved_outer) {
    if (!env_created) {
        saved_outer = g_runtime_env;
        env_created = true;
        g_runtime_env = gc_alloc_environment(g_runtime_env);
    }
}

static void prescan_syntax(const std::vector<Value>& raw_forms,
                            bool& env_created, Environment*& saved_outer);
static std::vector<Value> collect_body_forms(const Value& body_cons,
                                              bool& env_created, Environment*& saved_outer);

static void prescan_syntax(const std::vector<Value>& raw_forms,
                            bool& env_created, Environment*& saved_outer) {
    for (const Value& raw : raw_forms) {
        if (is_head(raw, sid_define_syntax)) {
            ensure_local_env(env_created, saved_outer);
            expand(raw);
        } else if (is_head(raw, sid_begin)) {
            std::vector<Value> sub;
            Value cur = cdr(raw);
            while (is_cons(cur)) { sub.push_back(car(cur)); cur = cdr(cur); }
            prescan_syntax(sub, env_created, saved_outer);
        } else if (is_cons(raw) && is_symbol(car(raw)) &&
                   is_syntax_transformer(lookup_macro(car(raw)))) {
            Value expanded;
            try { expanded = expand(raw); } catch (...) { continue; }
            if (is_head(expanded, sid_begin)) {
                std::vector<Value> sub;
                Value cur = cdr(expanded);
                while (is_cons(cur)) { sub.push_back(car(cur)); cur = cdr(cur); }
                prescan_syntax(sub, env_created, saved_outer);
            } else if (is_head(expanded, sid_define_syntax)) {
                ensure_local_env(env_created, saved_outer);
                expand(expanded);
            }
        }
    }
}

static std::vector<Value> collect_body_forms(const Value& body_cons,
                                              bool& env_created, Environment*& saved_outer) {
    std::vector<Value> raw_forms;
    Value cur = body_cons;
    while (is_cons(cur)) { raw_forms.push_back(car(cur)); cur = cdr(cur); }
    prescan_syntax(raw_forms, env_created, saved_outer);
    std::vector<Value> out;
    for (const Value& raw : raw_forms) {
        if (is_head(raw, sid_define_syntax)) {
            // already processed; skip
        } else if (is_head(raw, sid_define_values)) {
            out.push_back(raw);
        } else {
            Value form = expand(raw);
            if (is_head(form, sid_begin)) {
                std::vector<Value> sub = collect_body_forms(cdr(form), env_created, saved_outer);
                for (Value& v : sub) out.push_back(std::move(v));
            } else {
                out.push_back(form);
            }
        }
    }
    return out;
}

static Value expand_body(const Value& body_cons) {
    bool         env_created  = false;
    Environment* saved_outer  = nullptr;
    SourceInfo*  src          = src_of(body_cons);
    struct BodyEnvGuard {
        bool& env_created;
        Environment*& saved_outer;
        ~BodyEnvGuard() { if (env_created) g_runtime_env = saved_outer; }
    } _guard{env_created, saved_outer};

    std::vector<Value> forms = collect_body_forms(body_cons, env_created, saved_outer);

    // Separate leading defines/define-values from the rest
    std::vector<std::tuple<Value, Value>> bindings;  // (name-sym, init-expr)
    std::vector<Value> body_prefix;
    size_t i = 0;
    while (i < forms.size()) {
        const Value& f = forms[i];
        if (is_head(f, sid_define)) {
            if (!is_cons(cdr(f)) || !is_symbol(car(cdr(f))) ||
                !is_cons(cdr(cdr(f))) || !is_nil(cdr(cdr(cdr(f))))) break;
            bindings.emplace_back(car(cdr(f)), car(cdr(cdr(f))));
            i++; continue;
        }
        if (is_head(f, sid_define_values)) {
            if (!is_cons(cdr(f)) || !is_cons(cdr(cdr(f))) || !is_nil(cdr(cdr(cdr(f))))) break;
            Value formals_sexpr = car(cdr(f));
            Value expr          = car(cdr(cdr(f)));
            // _mv_collect_formals inline
            std::vector<std::string> fixed;
            std::string rest_name;
            bool has_rest = false;
            bool mv_ok = true;
            if (is_symbol(formals_sexpr)) {
                has_rest = true;
                rest_name = as_symbol(formals_sexpr);
            } else {
                Value fcur = formals_sexpr;
                while (is_cons(fcur)) {
                    if (!is_symbol(car(fcur))) { mv_ok = false; break; }
                    fixed.push_back(as_symbol(car(fcur)));
                    fcur = cdr(fcur);
                }
                if (mv_ok && !is_nil(fcur)) {
                    if (is_symbol(fcur)) { has_rest = true; rest_name = as_symbol(fcur); }
                    else mv_ok = false;
                }
            }
            if (!mv_ok) break;
            SourceInfo* fsrc = src_of(f);
            for (const std::string& fn : fixed)
                bindings.emplace_back(make_symbol(fn, fsrc), VOID_VALUE);
            if (has_rest)
                bindings.emplace_back(make_symbol(rest_name, fsrc), VOID_VALUE);
            // dv_build_setter
            {
                Value expanded_expr = expand(expr);
                // Build setter
                std::vector<Value> tmp_fixed_syms;
                for (size_t k = 0; k < fixed.size(); k++)
                    tmp_fixed_syms.push_back(make_symbol(hygiene_gensym("mv-tmp"), fsrc));
                Value tmp_rest_sym = has_rest ?
                    make_symbol(hygiene_gensym("mv-tmp-rest"), fsrc) : NIL_VALUE;
                std::vector<Value> body_items;
                for (size_t k = 0; k < fixed.size(); k++)
                    body_items.push_back(list_from_items(
                        {make_symbol("set!", fsrc), make_symbol(fixed[k], fsrc),
                         tmp_fixed_syms[k]}, fsrc));
                if (has_rest)
                    body_items.push_back(list_from_items(
                        {make_symbol("set!", fsrc), make_symbol(rest_name, fsrc),
                         tmp_rest_sym}, fsrc));
                Value consumer_formals;
                if (!has_rest)
                    consumer_formals = list_from_items(tmp_fixed_syms, fsrc);
                else if (tmp_fixed_syms.empty())
                    consumer_formals = tmp_rest_sym;
                else {
                    consumer_formals = tmp_rest_sym;
                    for (int k = (int)tmp_fixed_syms.size() - 1; k >= 0; k--)
                        consumer_formals = alloc_cons(tmp_fixed_syms[k], consumer_formals, fsrc);
                }
                std::vector<Value> consumer_items = {make_symbol("lambda", fsrc), consumer_formals};
                for (Value& v : body_items) consumer_items.push_back(v);
                Value consumer = list_from_items(consumer_items, fsrc);
                Value producer = list_from_items(
                    {make_symbol("lambda", fsrc), NIL_VALUE, expanded_expr}, fsrc);
                body_prefix.push_back(list_from_items(
                    {make_symbol("call-with-values", fsrc), producer, consumer}, fsrc));
            }
            i++; continue;
        }
        break;
    }

    if (bindings.empty()) {
        // No hoisting needed
        Value result = NIL_VALUE;
        for (int j = (int)forms.size() - 1; j >= 0; j--)
            result = alloc_cons(forms[j], result, src);
        return result;
    }

    // Build rest forms: body_prefix + forms[i..]
    std::vector<Value> rest_forms;
    for (Value& v : body_prefix) rest_forms.push_back(std::move(v));
    for (; i < forms.size(); i++) rest_forms.push_back(forms[i]);

    // Count name occurrences, build rename_table
    std::unordered_map<uint32_t, int> name_counts;
    for (auto& [name_sym, init] : bindings) {
        uint32_t sid = as_symbol_id(name_sym);
        name_counts[sid]++;
    }
    std::vector<std::string> gensym_names;
    for (auto& [name_sym, init] : bindings)
        gensym_names.push_back(hygiene_gensym(as_symbol(name_sym)));

    RenameTable rename_table;
    for (size_t j = 0; j < bindings.size(); j++) {
        uint32_t sid = as_symbol_id(std::get<0>(bindings[j]));
        if (name_counts[sid] == 1)
            rename_table[sid] = intern_symbol(gensym_names[j]);
    }

    // Build (letrec* ((gs init) ...) rest...)
    Value bindings_chain = NIL_VALUE;
    for (int j = (int)bindings.size() - 1; j >= 0; j--) {
        Value gs_sym  = make_symbol(gensym_names[j], src);
        Value init    = rename_refs_in_form(std::get<1>(bindings[j]), rename_table);
        Value pair    = list_from_items({gs_sym, init}, src);
        bindings_chain = alloc_cons(pair, bindings_chain, src);
    }
    Value rest_chain = NIL_VALUE;
    for (int j = (int)rest_forms.size() - 1; j >= 0; j--) {
        Value renamed = rename_refs_in_form(rest_forms[j], rename_table);
        rest_chain = alloc_cons(renamed, rest_chain, src);
    }
    Value letrec_sym  = make_symbol("letrec*", src);
    Value letrec_form = alloc_cons(letrec_sym,
                                   alloc_cons(bindings_chain, rest_chain, src),
                                   src);
    return alloc_cons(letrec_form, NIL_VALUE, src);
}

// ── Quasiquote helpers ────────────────────────────────────────────────────────

static Value qq_walk(const Value& x, int level, SourceInfo* default_src);

static Value qq_make_cons(const Value& car_v, const Value& cdr_v) {
    return list_from_items({make_symbol("cons", nullptr), car_v, cdr_v}, nullptr);
}
static Value qq_make_append(const Value& a, const Value& b) {
    return list_from_items({make_symbol("append", nullptr), a, b}, nullptr);
}
static Value qq_make_list(const Value& a, const Value& b) {
    return list_from_items({make_symbol("list", nullptr), a, b}, nullptr);
}
static Value qq_quote(const Value& x) {
    return list_from_items({make_symbol("quote", nullptr), x}, nullptr);
}

static Value qq_walk(const Value& x, int level, SourceInfo* /*default_src*/) {
    if (is_cons(x)) {
        Value head = car(x);
        if (is_symbol(head)) {
            uint32_t hid = as_symbol_id(head);
            if (hid == sid_unquote) {
                if (!is_cons(cdr(x)) || !is_nil(cdr(cdr(x))))
                    throw SchemeSyntaxError("unquote requires exactly one argument", src_of(x));
                Value e = car(cdr(x));
                if (level == 1) return expand(e);
                return qq_make_list(qq_quote(make_symbol("unquote", nullptr)),
                                    qq_walk(e, level - 1, nullptr));
            }
            if (hid == sid_quasiquote_kw) {
                if (!is_cons(cdr(x)) || !is_nil(cdr(cdr(x))))
                    throw SchemeSyntaxError("quasiquote requires exactly one argument", src_of(x));
                Value e = car(cdr(x));
                return qq_make_list(qq_quote(make_symbol("quasiquote", nullptr)),
                                    qq_walk(e, level + 1, nullptr));
            }
            if (hid == sid_unquote_splicing)
                throw SchemeSyntaxError(
                    "unquote-splicing must appear inside a list, not at the top of a template",
                    src_of(x));
        }
        // Splicing in element position: car is (unquote-splicing e)
        if (is_cons(head) && is_symbol(car(head)) &&
            as_symbol_id(car(head)) == sid_unquote_splicing) {
            if (!is_cons(cdr(head)) || !is_nil(cdr(cdr(head))))
                throw SchemeSyntaxError("unquote-splicing requires exactly one argument",
                                        src_of(head));
            Value e    = car(cdr(head));
            Value tail = qq_walk(cdr(x), level, nullptr);
            if (level == 1) return qq_make_append(expand(e), tail);
            Value spliced = qq_make_list(
                qq_quote(make_symbol("unquote-splicing", nullptr)),
                qq_walk(e, level - 1, nullptr));
            return qq_make_cons(spliced, tail);
        }
        return qq_make_cons(qq_walk(car(x), level, nullptr),
                            qq_walk(cdr(x), level, nullptr));
    }
    if (is_vector(x)) {
        const auto& items = as_vector_items_const(x);
        Value list_chain = list_from_items(items, nullptr);
        Value list_expr  = qq_walk(list_chain, level, nullptr);
        return list_from_items({make_symbol("list->vector", nullptr), list_expr}, nullptr);
    }
    return qq_quote(x);
}

// ── Multi-value helpers ───────────────────────────────────────────────────────

struct MvFormals { std::vector<std::string> fixed; std::string rest; bool has_rest = false; bool ok = true; };

static MvFormals mv_collect_formals(const Value& formals_sexpr) {
    MvFormals r;
    if (is_symbol(formals_sexpr)) {
        r.has_rest = true;
        r.rest = as_symbol(formals_sexpr);
        return r;
    }
    Value cur = formals_sexpr;
    while (is_cons(cur)) {
        if (!is_symbol(car(cur))) { r.ok = false; return r; }
        r.fixed.push_back(as_symbol(car(cur)));
        cur = cdr(cur);
    }
    if (is_nil(cur)) return r;
    if (is_symbol(cur)) { r.has_rest = true; r.rest = as_symbol(cur); return r; }
    r.ok = false;
    return r;
}

static Value mv_thunk(const Value& init) {
    return list_from_items({make_symbol("lambda", nullptr), NIL_VALUE, init}, nullptr);
}
static Value mv_lambda(const Value& formals_sexpr, const std::vector<Value>& body_items) {
    std::vector<Value> items = {make_symbol("lambda", nullptr), formals_sexpr};
    for (const Value& v : body_items) items.push_back(v);
    return list_from_items(items, nullptr);
}
static Value mv_cwv(const Value& producer, const Value& consumer) {
    return list_from_items({make_symbol("call-with-values", nullptr), producer, consumer}, nullptr);
}
static Value mv_cdr_expr(const Value& expr) {
    return list_from_items({make_symbol("cdr", nullptr), expr}, nullptr);
}
static Value mv_car_expr(const Value& expr) {
    return list_from_items({make_symbol("car", nullptr), expr}, nullptr);
}
static Value mv_nth_ref(const Value& tmp_sym, int index) {
    Value expr = tmp_sym;
    for (int i = 0; i < index; i++) expr = mv_cdr_expr(expr);
    return mv_car_expr(expr);
}
static Value mv_tail_ref(const Value& tmp_sym, int skip) {
    Value expr = tmp_sym;
    for (int i = 0; i < skip; i++) expr = mv_cdr_expr(expr);
    return expr;
}

// ── define-values setter builder ─────────────────────────────────────────────

static Value dv_build_setter(const std::vector<std::string>& fixed,
                              bool has_rest, const std::string& rest_name,
                              const Value& expanded_expr, SourceInfo* src) {
    std::vector<Value> tmp_fixed;
    for (size_t i = 0; i < fixed.size(); i++)
        tmp_fixed.push_back(make_symbol(hygiene_gensym("mv-tmp"), src));
    Value tmp_rest = has_rest ? make_symbol(hygiene_gensym("mv-tmp-rest"), src) : NIL_VALUE;
    std::vector<Value> body_items;
    for (size_t i = 0; i < fixed.size(); i++)
        body_items.push_back(list_from_items(
            {make_symbol("set!", src), make_symbol(fixed[i], src), tmp_fixed[i]},
            src));
    if (has_rest)
        body_items.push_back(list_from_items(
            {make_symbol("set!", src), make_symbol(rest_name, src), tmp_rest},
            src));
    Value consumer_formals;
    if (!has_rest)
        consumer_formals = list_from_items(tmp_fixed, src);
    else if (tmp_fixed.empty())
        consumer_formals = tmp_rest;
    else {
        consumer_formals = tmp_rest;
        for (int k = (int)tmp_fixed.size() - 1; k >= 0; k--)
            consumer_formals = alloc_cons(tmp_fixed[k], consumer_formals, src);
    }
    Value consumer = mv_lambda(consumer_formals, body_items);
    Value producer = mv_thunk(expanded_expr);
    return mv_cwv(producer, consumer);
}

// ── cond-expand features ──────────────────────────────────────────────────────

bool feature_req_matches(const Value& req) {
    if (is_symbol(req)) {
        static const std::unordered_set<std::string> FEATURES = {
            "r7rs", "exact-closed", "exact-rational", "ratios",
            "ieee-float", "full-unicode", "pyscheme", "cppscheme2",
#ifdef _WIN32
            "windows",
#else
            "posix",
#  if defined(__linux__)
            "linux",
#  elif defined(__APPLE__)
            "darwin",
#  endif
#endif
        };
        std::string name = as_symbol(req);
        if (name == "else") return true;
        return FEATURES.count(name) > 0;
    }
    if (!is_cons(req)) return false;
    Value head = car(req);
    if (!is_symbol(head)) return false;
    uint32_t op = as_symbol_id(head);
    if (op == sid_and_kw) {
        Value cur = cdr(req);
        while (is_cons(cur)) {
            if (!feature_req_matches(car(cur))) return false;
            cur = cdr(cur);
        }
        return true;
    }
    if (op == sid_or_kw) {
        Value cur = cdr(req);
        while (is_cons(cur)) {
            if (feature_req_matches(car(cur))) return true;
            cur = cdr(cur);
        }
        return false;
    }
    if (op == sid_not_kw) {
        if (!is_cons(cdr(req))) return false;
        return !feature_req_matches(car(cdr(req)));
    }
    if (op == sid_library_kw) {
        if (!is_cons(cdr(req))) return false;
        try {
            return library_registered_p(library_name_to_key(car(cdr(req))));
        } catch (...) {
            return false;
        }
    }
    return false;
}

// ── include helpers ───────────────────────────────────────────────────────────

static Value case_fold(const Value& form) {
    if (is_cons(form))
        return alloc_cons(case_fold(car(form)), case_fold(cdr(form)), src_of(form));
    if (is_symbol(form)) {
        std::string name = as_symbol(form);
        for (char& c : name) c = (char)std::tolower((unsigned char)c);
        return make_symbol(name, nullptr);
    }
    return form;
}

std::string include_base_dir(SourceInfo* src) {
    if (src == nullptr) return g_include_fallback_dir;
    if (src->filename.empty() || src->filename == REPL_FILENAME)
        return g_include_fallback_dir;
    return fs::path(src->filename).parent_path().string();
}

static std::vector<Value> read_and_expand_file(const std::string& filename,
                                                SourceInfo* src, bool fold) {
    std::ifstream f(filename);
    if (!f.is_open())
        throw SchemeSyntaxError("include: file not found: " + filename, src);
    std::string source((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
    std::vector<Value> raw = scheme_parse(source, filename);
    std::vector<Value> expanded;
    for (Value form : raw) {
        if (fold) form = case_fold(form);
        expanded.push_back(expand(form));
    }
    return expanded;
}

// ── Sugar handlers ────────────────────────────────────────────────────────────

static Value expand_define(const Value& sexpr) {
    if (!is_cons(cdr(sexpr))) return expand_list(sexpr);
    Value signature = car(cdr(sexpr));
    Value body_cons  = cdr(cdr(sexpr));
    if (!is_cons(signature) || is_nil(body_cons)) return expand_list(sexpr);
    Value name_sexpr  = car(signature);
    Value params_node = cdr(signature);
    SourceInfo* sig_src    = src_of(signature);
    SourceInfo* define_src = src_of(sexpr);
    Value body_chain  = expand_body(body_cons);
    Value params_and_body = alloc_cons(params_node, body_chain, sig_src);
    Value lambda_form     = alloc_cons(make_symbol("lambda", sig_src), params_and_body, sig_src);
    Value lambda_cons     = alloc_cons(lambda_form, NIL_VALUE, define_src);
    Value name_cons       = alloc_cons(name_sexpr, lambda_cons, define_src);
    return alloc_cons(make_symbol("define", define_src), name_cons, define_src);
}

static Value expand_lambda(const Value& sexpr) {
    if (!is_cons(cdr(sexpr))) return expand_list(sexpr);
    Value formals = car(cdr(sexpr));
    Value body    = cdr(cdr(sexpr));
    if (is_nil(body)) return expand_list(sexpr);
    SourceInfo* src = src_of(sexpr);
    RenameTable rename_table;
    Value new_formals    = gensym_rename_formals(formals, rename_table);
    Value renamed_body   = rename_refs_in_form(body, rename_table);
    Value expanded_body  = expand_body(renamed_body);
    return alloc_cons(make_symbol("lambda", src),
                      alloc_cons(new_formals, expanded_body, src),
                      src);
}

static Value expand_let_family(const Value& sexpr, const std::string& head_name) {
    if (!is_cons(cdr(sexpr))) return expand_list(sexpr);
    Value first = car(cdr(sexpr));
    Value rest  = cdr(cdr(sexpr));
    Value named_name_sym;
    bool  has_named = false;
    Value bindings_form, body_cons;
    if (head_name == "let" && is_symbol(first) && is_cons(rest)) {
        has_named     = true;
        named_name_sym = first;
        bindings_form  = car(rest);
        body_cons      = cdr(rest);
    } else {
        bindings_form = first;
        body_cons     = rest;
    }
    if (!is_cons(bindings_form) && !is_nil(bindings_form)) return expand_list(sexpr);
    if (is_nil(body_cons)) return expand_list(sexpr);

    struct RawPair { std::string name; Value init; SourceInfo* pair_src; };
    std::vector<RawPair> raw_pairs;
    Value cur = bindings_form;
    while (is_cons(cur)) {
        Value pair = car(cur);
        if (!is_cons(pair) || !is_symbol(car(pair)) ||
            !is_cons(cdr(pair)) || !is_nil(cdr(cdr(pair))))
            return expand_list(sexpr);
        raw_pairs.push_back({as_symbol(car(pair)), car(cdr(pair)), src_of(pair)});
        cur = cdr(cur);
    }
    if (!is_nil(cur)) return expand_list(sexpr);

    if (head_name != "let*") {
        std::unordered_set<std::string> seen;
        for (const auto& rp : raw_pairs) {
            if (seen.count(rp.name))
                throw SchemeSyntaxError("duplicate variable name in " + head_name +
                                        " bindings: " + rp.name, src_of(sexpr));
            seen.insert(rp.name);
        }
    }

    RenameTable rename_table;
    for (const auto& rp : raw_pairs)
        rename_table[intern_symbol(rp.name)] = intern_symbol(hygiene_gensym(rp.name));

    SourceInfo* src = src_of(sexpr);

    Value new_named_sym;
    bool  has_new_named = false;
    if (has_named) {
        has_new_named = true;
        std::string loop_name = as_symbol(named_name_sym);
        std::string loop_gs   = hygiene_gensym(loop_name);
        rename_table[intern_symbol(loop_name)] = intern_symbol(loop_gs);
        new_named_sym = make_symbol(loop_gs, src_of(named_name_sym));
    }

    std::vector<Value> new_pairs;
    if (head_name == "let") {
        for (const auto& rp : raw_pairs) {
            Value gs_sym = make_symbol(symbol_name(rename_table.at(intern_symbol(rp.name))), src);
            new_pairs.push_back(list_from_items({gs_sym, expand(rp.init)}, rp.pair_src));
        }
    } else if (head_name == "let*") {
        RenameTable progressive;
        for (const auto& rp : raw_pairs) {
            Value renamed_init = rename_refs_in_form(rp.init, progressive);
            Value new_init     = expand(renamed_init);
            Value gs_sym = make_symbol(symbol_name(rename_table.at(intern_symbol(rp.name))), src);
            new_pairs.push_back(list_from_items({gs_sym, new_init}, rp.pair_src));
            progressive[intern_symbol(rp.name)] = rename_table.at(intern_symbol(rp.name));
        }
    } else {
        for (const auto& rp : raw_pairs) {
            Value renamed_init = rename_refs_in_form(rp.init, rename_table);
            Value new_init     = expand(renamed_init);
            Value gs_sym = make_symbol(symbol_name(rename_table.at(intern_symbol(rp.name))), src);
            new_pairs.push_back(list_from_items({gs_sym, new_init}, rp.pair_src));
        }
    }

    Value new_bindings = NIL_VALUE;
    for (int j = (int)new_pairs.size() - 1; j >= 0; j--)
        new_bindings = alloc_cons(new_pairs[j], new_bindings, src);

    Value renamed_body   = rename_refs_in_form(body_cons, rename_table);
    Value expanded_body  = expand_body(renamed_body);

    Value head_sym = make_symbol(head_name, src);
    Value tail;
    if (has_new_named) {
        tail = alloc_cons(new_named_sym,
               alloc_cons(new_bindings, expanded_body, src), src);
    } else {
        tail = alloc_cons(new_bindings, expanded_body, src);
    }
    return alloc_cons(head_sym, tail, src);
}

static Value expand_let(const Value& s)         { return expand_let_family(s, "let"); }
static Value expand_let_star(const Value& s)    { return expand_let_family(s, "let*"); }
static Value expand_letrec(const Value& s)      { return expand_let_family(s, "letrec"); }
static Value expand_letrec_star(const Value& s) { return expand_let_family(s, "letrec*"); }

static Value expand_when_unless(const Value& sexpr, const std::string& head_name) {
    if (!is_cons(cdr(sexpr))) return expand_list(sexpr);
    Value test = car(cdr(sexpr));
    Value body = cdr(cdr(sexpr));
    if (is_nil(body)) return expand_list(sexpr);
    SourceInfo* src = src_of(sexpr);
    Value expanded_body = expand_body(body);
    return alloc_cons(make_symbol(head_name, src),
                      alloc_cons(expand(test), expanded_body, src),
                      src);
}
static Value expand_when(const Value& s)   { return expand_when_unless(s, "when"); }
static Value expand_unless(const Value& s) { return expand_when_unless(s, "unless"); }

static Value expand_case_lambda(const Value& sexpr) {
    if (!is_cons(cdr(sexpr))) return expand_list(sexpr);
    SourceInfo* src = src_of(sexpr);
    std::vector<Value> expanded_clauses;
    Value cur = cdr(sexpr);
    while (is_cons(cur)) {
        Value clause = car(cur);
        if (!is_cons(clause) || !is_cons(cdr(clause))) return expand_list(sexpr);
        Value formals = car(clause);
        Value body    = cdr(clause);
        RenameTable rt;
        Value new_formals   = gensym_rename_formals(formals, rt);
        Value renamed_body  = rename_refs_in_form(body, rt);
        Value expanded_body = expand_body(renamed_body);
        expanded_clauses.push_back(alloc_cons(new_formals, expanded_body, src_of(clause)));
        cur = cdr(cur);
    }
    if (!is_nil(cur)) return expand_list(sexpr);
    Value tail = NIL_VALUE;
    for (int i = (int)expanded_clauses.size() - 1; i >= 0; i--)
        tail = alloc_cons(expanded_clauses[i], tail, src);
    return alloc_cons(make_symbol("case-lambda", src), tail, src);
}

static Value expand_if(const Value& sexpr) {
    if (list_length(sexpr) != 3) return expand_list(sexpr);
    Value test    = car(cdr(sexpr));
    Value then_br = car(cdr(cdr(sexpr)));
    SourceInfo* src = src_of(sexpr);
    Value tail3 = alloc_cons(VOID_VALUE, NIL_VALUE, src);
    Value tail2 = alloc_cons(expand(then_br), tail3, src);
    Value tail1 = alloc_cons(expand(test), tail2, src);
    return alloc_cons(make_symbol("if", src), tail1, src);
}

static Value expand_quote(const Value& sexpr) { return sexpr; }

static Value expand_do(const Value& sexpr) {
    if (list_length(sexpr) < 3) return expand_list(sexpr);
    Value bindings_cons = car(cdr(sexpr));
    Value test_cons     = car(cdr(cdr(sexpr)));
    Value body_cons     = cdr(cdr(cdr(sexpr)));
    if (list_length(bindings_cons) < 0) return expand_list(sexpr);
    if (!is_cons(test_cons) || list_length(test_cons) < 1) return expand_list(sexpr);

    std::vector<Value> vars_list, inits_list, steps_list;
    std::unordered_set<std::string> seen_vars;
    Value cur = bindings_cons;
    while (is_cons(cur)) {
        Value b = car(cur);
        int blen = list_length(b);
        if (blen != 2 && blen != 3) return expand_list(sexpr);
        if (!is_symbol(car(b))) return expand_list(sexpr);
        std::string vname = as_symbol(car(b));
        if (seen_vars.count(vname)) return expand_list(sexpr);
        seen_vars.insert(vname);
        vars_list.push_back(car(b));
        inits_list.push_back(expand(car(cdr(b))));
        if (blen == 3) steps_list.push_back(expand(car(cdr(cdr(b)))));
        else           steps_list.push_back(NIL_VALUE);  // sentinel: use var itself
        cur = cdr(cur);
    }
    Value test_expr = expand(car(test_cons));
    Value loop_sym  = make_symbol(hygiene_gensym("do-loop"), nullptr);

    // Recursive call
    std::vector<Value> call_items = {loop_sym};
    for (size_t i = 0; i < vars_list.size(); i++) {
        if (!is_nil(steps_list[i])) call_items.push_back(steps_list[i]);
        else                         call_items.push_back(vars_list[i]);
    }
    Value recur_call = list_from_items(call_items, nullptr);

    // Body form
    std::vector<Value> body_items;
    cur = body_cons;
    while (is_cons(cur)) { body_items.push_back(expand(car(cur))); cur = cdr(cur); }
    Value body_form;
    if (body_items.empty()) {
        body_form = recur_call;
    } else {
        std::vector<Value> seq = {make_symbol("begin", nullptr)};
        for (Value& v : body_items) seq.push_back(v);
        seq.push_back(recur_call);
        body_form = list_from_items(seq, nullptr);
    }

    // Result form
    std::vector<Value> result_items;
    cur = cdr(test_cons);
    while (is_cons(cur)) { result_items.push_back(expand(car(cur))); cur = cdr(cur); }
    Value result_form;
    if (result_items.empty())       result_form = VOID_VALUE;
    else if (result_items.size() == 1) result_form = result_items[0];
    else {
        std::vector<Value> seq = {make_symbol("begin", nullptr)};
        for (Value& v : result_items) seq.push_back(v);
        result_form = list_from_items(seq, nullptr);
    }

    Value if_form = list_from_items(
        {make_symbol("if", nullptr), test_expr, result_form, body_form}, nullptr);

    std::vector<Value> binding_forms;
    for (size_t i = 0; i < vars_list.size(); i++)
        binding_forms.push_back(list_from_items({vars_list[i], inits_list[i]}, nullptr));
    Value let_bindings = list_from_items(binding_forms, nullptr);

    return list_from_items({make_symbol("let", nullptr), loop_sym, let_bindings, if_form}, nullptr);
}

static Value expand_include_form(const Value& sexpr, bool fold) {
    if (!is_cons(cdr(sexpr))) return expand_list(sexpr);
    SourceInfo* src = src_of(sexpr);
    std::string base_dir = include_base_dir(src);
    std::vector<Value> all_forms;
    Value cur = cdr(sexpr);
    while (is_cons(cur)) {
        Value path_val = car(cur);
        if (!is_string(path_val)) return expand_list(sexpr);
        std::string requested = as_string(path_val);
        std::string resolved  = base_dir.empty() ? requested
                                                   : (fs::path(base_dir) / requested).string();
        std::vector<Value> from_file = read_and_expand_file(resolved, src, fold);
        for (Value& v : from_file) all_forms.push_back(std::move(v));
        cur = cdr(cur);
    }
    if (all_forms.empty()) return expand_list(sexpr);
    std::vector<Value> items = {make_symbol("begin", nullptr)};
    for (Value& v : all_forms) items.push_back(std::move(v));
    return list_from_items(items, nullptr);
}
static Value expand_include(const Value& s)    { return expand_include_form(s, false); }
static Value expand_include_ci(const Value& s) { return expand_include_form(s, true); }

static Value expand_cond_expand(const Value& sexpr) {
    if (!is_cons(cdr(sexpr))) return expand_list(sexpr);
    Value cur = cdr(sexpr);
    while (is_cons(cur)) {
        Value clause = car(cur);
        if (!is_cons(clause)) return expand_list(sexpr);
        Value req  = car(clause);
        Value body = cdr(clause);
        if (feature_req_matches(req)) {
            if (is_nil(body)) return VOID_VALUE;
            std::vector<Value> items = {make_symbol("begin", nullptr)};
            Value bcur = body;
            while (is_cons(bcur)) { items.push_back(expand(car(bcur))); bcur = cdr(bcur); }
            return list_from_items(items, nullptr);
        }
        cur = cdr(cur);
    }
    return expand_list(sexpr);
}

static Value expand_quasiquote(const Value& sexpr) {
    if (!is_cons(cdr(sexpr)) || !is_nil(cdr(cdr(sexpr)))) return expand_list(sexpr);
    return qq_walk(car(cdr(sexpr)), 1, src_of(sexpr));
}

static Value expand_let_star_values(const Value& sexpr) {
    if (list_length(sexpr) < 3) return expand_list(sexpr);
    Value bindings_cons = car(cdr(sexpr));
    Value body_cons     = cdr(cdr(sexpr));
    if (list_length(bindings_cons) < 0) return expand_list(sexpr);
    std::vector<std::pair<Value,Value>> clauses;
    Value cur = bindings_cons;
    while (is_cons(cur)) {
        Value b = car(cur);
        if (list_length(b) != 2) return expand_list(sexpr);
        clauses.push_back({car(b), expand(car(cdr(b)))});
        cur = cdr(cur);
    }
    std::vector<Value> body_items;
    Value bcur = expand_body(body_cons);
    while (is_cons(bcur)) { body_items.push_back(car(bcur)); bcur = cdr(bcur); }
    if (body_items.empty()) return expand_list(sexpr);
    if (clauses.empty()) {
        std::vector<Value> items = {make_symbol("begin", nullptr)};
        for (Value& v : body_items) items.push_back(v);
        return list_from_items(items, nullptr);
    }
    Value inner = mv_lambda(clauses.back().first, body_items);
    Value result = mv_cwv(mv_thunk(clauses.back().second), inner);
    for (int i = (int)clauses.size() - 2; i >= 0; i--) {
        Value consumer = mv_lambda(clauses[i].first, {result});
        result = mv_cwv(mv_thunk(clauses[i].second), consumer);
    }
    return result;
}

static Value expand_let_values(const Value& sexpr) {
    if (list_length(sexpr) < 3) return expand_list(sexpr);
    Value bindings_cons = car(cdr(sexpr));
    Value body_cons     = cdr(cdr(sexpr));
    if (list_length(bindings_cons) < 0) return expand_list(sexpr);
    std::vector<std::pair<Value,Value>> clauses;
    Value cur = bindings_cons;
    while (is_cons(cur)) {
        Value b = car(cur);
        if (list_length(b) != 2) return expand_list(sexpr);
        clauses.push_back({car(b), expand(car(cdr(b)))});
        cur = cdr(cur);
    }
    std::vector<Value> body_items;
    Value bcur = expand_body(body_cons);
    while (is_cons(bcur)) { body_items.push_back(car(bcur)); bcur = cdr(bcur); }
    if (body_items.empty()) return expand_list(sexpr);
    if (clauses.empty()) {
        std::vector<Value> items = {make_symbol("begin", nullptr)};
        for (Value& v : body_items) items.push_back(v);
        return list_from_items(items, nullptr);
    }
    // Outer let: each clause -> gensym temp bound to (call-with-values thunk list)
    std::vector<Value> outer_binding_pairs;
    std::vector<Value> tmp_syms;
    for (const auto& [formals, init] : clauses) {
        std::string tmp_name = hygiene_gensym("mv");
        Value tmp_sym = make_symbol(tmp_name, nullptr);
        tmp_syms.push_back(tmp_sym);
        Value cwv = mv_cwv(mv_thunk(init), make_symbol("list", nullptr));
        outer_binding_pairs.push_back(list_from_items({tmp_sym, cwv}, nullptr));
    }
    Value outer_bindings = list_from_items(outer_binding_pairs, nullptr);
    // Inner let: extract each formal from tmp
    std::vector<Value> inner_binding_pairs;
    for (size_t idx = 0; idx < clauses.size(); idx++) {
        MvFormals mvf = mv_collect_formals(clauses[idx].first);
        if (!mvf.ok) return expand_list(sexpr);
        Value tmp = tmp_syms[idx];
        for (size_t j = 0; j < mvf.fixed.size(); j++)
            inner_binding_pairs.push_back(list_from_items(
                {make_symbol(mvf.fixed[j], nullptr), mv_nth_ref(tmp, (int)j)}, nullptr));
        if (mvf.has_rest)
            inner_binding_pairs.push_back(list_from_items(
                {make_symbol(mvf.rest, nullptr),
                 mv_tail_ref(tmp, (int)mvf.fixed.size())}, nullptr));
    }
    Value inner_bindings = inner_binding_pairs.empty() ? NIL_VALUE
                         : list_from_items(inner_binding_pairs, nullptr);
    std::vector<Value> inner_items = {make_symbol("let", nullptr), inner_bindings};
    for (Value& v : body_items) inner_items.push_back(v);
    Value inner_let = list_from_items(inner_items, nullptr);
    return list_from_items({make_symbol("let", nullptr), outer_bindings, inner_let}, nullptr);
}

static Value expand_guard(const Value& sexpr) {
    if (list_length(sexpr) < 3) return expand_list(sexpr);
    Value guard_spec = car(cdr(sexpr));
    Value body_cons  = cdr(cdr(sexpr));
    if (list_length(guard_spec) < 1) return expand_list(sexpr);
    Value var_sexpr = car(guard_spec);
    if (!is_symbol(var_sexpr)) return expand_list(sexpr);
    Value clauses_cons = cdr(guard_spec);
    if (list_length(clauses_cons) < 0) return expand_list(sexpr);

    bool has_else = false;
    Value last_clause;
    Value cur = clauses_cons;
    while (is_cons(cur)) { last_clause = car(cur); cur = cdr(cur); }
    if (is_cons(last_clause) && is_symbol(car(last_clause)) &&
        as_symbol_id(car(last_clause)) == sid_else)
        has_else = true;

    SourceInfo* src = src_of(sexpr);
    std::vector<Value> body_items;
    cur = expand_body(body_cons);
    while (is_cons(cur)) { body_items.push_back(car(cur)); cur = cdr(cur); }
    if (body_items.empty()) return expand_list(sexpr);

    std::vector<Value> cond_items = {make_symbol("cond", src)};
    cur = clauses_cons;
    while (is_cons(cur)) { cond_items.push_back(expand_list(car(cur))); cur = cdr(cur); }
    if (!has_else) {
        Value raise_call = list_from_items(
            {make_symbol("raise", src), var_sexpr}, src);
        cond_items.push_back(list_from_items(
            {make_symbol("else", src), raise_call}, src));
    }
    Value cond_form = list_from_items(cond_items, src);
    Value handler_params = alloc_cons(var_sexpr, NIL_VALUE, src);
    Value handler = list_from_items(
        {make_symbol("lambda", src), handler_params, cond_form}, src);
    std::vector<Value> thunk_items = {make_symbol("lambda", src), NIL_VALUE};
    for (Value& v : body_items) thunk_items.push_back(v);
    Value thunk = list_from_items(thunk_items, src);
    return list_from_items({make_symbol("%guard-eval", src), handler, thunk}, src);
}

static Value expand_define_record_type(const Value& sexpr) {
    if (list_length(sexpr) < 4) return expand_list(sexpr);
    Value type_name_sexpr = car(cdr(sexpr));
    Value ctor_spec       = car(cdr(cdr(sexpr)));
    Value pred_sexpr      = car(cdr(cdr(cdr(sexpr))));
    Value field_specs     = cdr(cdr(cdr(cdr(sexpr))));
    if (!is_symbol(type_name_sexpr) || !is_symbol(pred_sexpr)) return expand_list(sexpr);
    if (list_length(ctor_spec) < 1) return expand_list(sexpr);
    Value ctor_name = car(ctor_spec);
    if (!is_symbol(ctor_name)) return expand_list(sexpr);

    std::unordered_set<std::string> ctor_field_names_set;
    std::vector<Value> ctor_fields;
    Value cur = cdr(ctor_spec);
    while (is_cons(cur)) {
        if (!is_symbol(car(cur))) return expand_list(sexpr);
        ctor_fields.push_back(car(cur));
        ctor_field_names_set.insert(as_symbol(car(cur)));
        cur = cdr(cur);
    }
    if (!is_nil(cur)) return expand_list(sexpr);

    struct FieldEntry { Value fname; Value accessor; Value mutator; bool has_mutator; };
    std::vector<FieldEntry> field_entries;
    cur = field_specs;
    while (is_cons(cur)) {
        Value spec = car(cur);
        int slen = list_length(spec);
        if (slen != 2 && slen != 3) return expand_list(sexpr);
        Value fname    = car(spec);
        Value accessor = car(cdr(spec));
        if (!is_symbol(fname) || !is_symbol(accessor)) return expand_list(sexpr);
        if (slen == 3) {
            Value mutator = car(cdr(cdr(spec)));
            if (!is_symbol(mutator)) return expand_list(sexpr);
            field_entries.push_back({fname, accessor, mutator, true});
        } else {
            field_entries.push_back({fname, accessor, NIL_VALUE, false});
        }
        cur = cdr(cur);
    }
    if (!is_nil(cur)) return expand_list(sexpr);

    std::string type_name = as_symbol(type_name_sexpr);
    std::vector<Value> all_field_names;
    for (const auto& fe : field_entries) all_field_names.push_back(fe.fname);

    std::string rt_sym_name = hygiene_gensym("record-type-" + type_name);
    SourceInfo* src = src_of(sexpr);
    Value rt_sym = make_symbol(rt_sym_name, src);

    auto mk_sym = [src](const std::string& n) { return make_symbol(n, src); };
    auto mk_define = [&](Value name_v, Value value_v) -> Value {
        return list_from_items({mk_sym("define"), name_v, value_v}, src);
    };
    auto mk_define_proc = [&](Value name_v, Value params_v, Value body_v) -> Value {
        Value lam = list_from_items({mk_sym("lambda"), params_v, body_v}, src);
        return list_from_items({mk_sym("define"), name_v, lam}, src);
    };
    auto mk_quote = [&](const Value& datum) -> Value {
        return list_from_items({mk_sym("quote"), datum}, src);
    };
    auto mk_list_of_syms = [&](const std::vector<Value>& sym_list) -> Value {
        return mk_quote(list_from_items(sym_list, src));
    };

    std::vector<Value> forms = {mk_sym("begin")};

    Value rt_init = list_from_items(
        {mk_sym("%make-record-type"), mk_quote(type_name_sexpr), mk_list_of_syms(all_field_names)},
        src);
    forms.push_back(mk_define(rt_sym, rt_init));

    std::vector<Value> list_items = {mk_sym("list")};
    for (const Value& fn : all_field_names) {
        if (ctor_field_names_set.count(as_symbol(fn))) list_items.push_back(fn);
        else                                            list_items.push_back(VOID_VALUE);
    }
    Value ctor_body = list_from_items(
        {mk_sym("%make-record"), rt_sym, list_from_items(list_items, src)}, src);
    Value params = list_from_items(ctor_fields, src);
    forms.push_back(mk_define_proc(ctor_name, params, ctor_body));

    Value obj_sym    = mk_sym("obj");
    Value pred_body  = list_from_items({mk_sym("%record-of-type?"), obj_sym, rt_sym}, src);
    Value pred_params = alloc_cons(obj_sym, NIL_VALUE, src);
    forms.push_back(mk_define_proc(pred_sexpr, pred_params, pred_body));

    for (int i = 0; i < (int)field_entries.size(); i++) {
        Value idx_val  = make_integer((int64_t)i, src);
        Value acc_init = list_from_items(
            {mk_sym("%make-record-accessor"), rt_sym, idx_val,
             mk_quote(field_entries[i].accessor)}, nullptr);
        forms.push_back(mk_define(field_entries[i].accessor, acc_init));
        if (field_entries[i].has_mutator) {
            Value mut_init = list_from_items(
                {mk_sym("%make-record-mutator"), rt_sym, idx_val,
                 mk_quote(field_entries[i].mutator)}, nullptr);
            forms.push_back(mk_define(field_entries[i].mutator, mut_init));
        }
    }

    return list_from_items(forms, nullptr);
}

static Value expand_define_values(const Value& sexpr) {
    if (list_length(sexpr) != 3) return expand_list(sexpr);
    Value formals_sexpr = car(cdr(sexpr));
    Value expr          = car(cdr(cdr(sexpr)));
    MvFormals mvf = mv_collect_formals(formals_sexpr);
    if (!mvf.ok) return expand_list(sexpr);
    SourceInfo* src = src_of(sexpr);
    Value define_sym = make_symbol("define", src);
    std::vector<Value> items = {make_symbol("begin", src)};
    for (const std::string& fn : mvf.fixed)
        items.push_back(list_from_items(
            {define_sym, make_symbol(fn, src), VOID_VALUE}, src));
    if (mvf.has_rest)
        items.push_back(list_from_items(
            {define_sym, make_symbol(mvf.rest, src), VOID_VALUE}, src));
    items.push_back(dv_build_setter(mvf.fixed, mvf.has_rest, mvf.rest, expand(expr), src));
    return list_from_items(items, src);
}

static Value expand_parameterize(const Value& sexpr) {
    if (list_length(sexpr) < 3) return expand_list(sexpr);
    Value bindings_cons = car(cdr(sexpr));
    Value body_cons     = cdr(cdr(sexpr));
    if (list_length(bindings_cons) < 0) return expand_list(sexpr);
    SourceInfo* src = src_of(sexpr);
    std::vector<Value> params_exprs, values_exprs;
    Value cur = bindings_cons;
    while (is_cons(cur)) {
        Value b = car(cur);
        if (list_length(b) != 2) return expand_list(sexpr);
        params_exprs.push_back(expand(car(b)));
        values_exprs.push_back(expand(car(cdr(b))));
        cur = cdr(cur);
    }
    std::vector<Value> body_items;
    cur = expand_body(body_cons);
    while (is_cons(cur)) { body_items.push_back(car(cur)); cur = cdr(cur); }
    if (body_items.empty()) return expand_list(sexpr);
    Value list_sym = make_symbol("list", src);
    std::vector<Value> pl = {list_sym};
    for (Value& v : params_exprs) pl.push_back(v);
    std::vector<Value> vl = {list_sym};
    for (Value& v : values_exprs) vl.push_back(v);
    std::vector<Value> lambda_items = {make_symbol("lambda", src), NIL_VALUE};
    for (Value& v : body_items) lambda_items.push_back(v);
    return list_from_items(
        {make_symbol("%with-parameters", src),
         list_from_items(pl, src),
         list_from_items(vl, src),
         list_from_items(lambda_items, src)}, src);
}

static Value expand_define_library(const Value& sexpr) { return sexpr; }
static Value expand_import(const Value& sexpr)         { return sexpr; }

// ── Core expansion ────────────────────────────────────────────────────────────

static Value expand_list(const Value& cell) {
    std::vector<Value> items;
    std::vector<SourceInfo*> srcs;
    Value cur = cell;
    while (is_cons(cur)) {
        items.push_back(expand(car(cur)));
        srcs.push_back(src_of(cur));
        cur = cdr(cur);
    }
    Value tail = is_nil(cur) ? NIL_VALUE : expand(cur);
    Value result = tail;
    for (int i = (int)items.size() - 1; i >= 0; i--)
        result = alloc_cons(items[i], result, srcs[i]);
    return result;
}

static Value expand_define_syntax(const Value& sexpr) {
    if (!is_cons(cdr(sexpr)) || !is_symbol(car(cdr(sexpr))))
        throw SchemeSyntaxError(
            "define-syntax: expected (define-syntax <name> <transformer>)",
            src_of(sexpr));
    Value macro_sym  = car(cdr(sexpr));
    std::string macro_name = as_symbol(macro_sym);
    if (!is_cons(cdr(cdr(sexpr))))
        throw SchemeSyntaxError("define-syntax: missing transformer", src_of(sexpr));
    Value tr_expr = car(cdr(cdr(sexpr)));
    if (!is_cons(tr_expr) || !is_symbol(car(tr_expr)) ||
        as_symbol_id(car(tr_expr)) != sid_syntax_rules)
        throw SchemeSyntaxError(
            "define-syntax: transformer must be (syntax-rules ...)", src_of(tr_expr));
    Value t = parse_syntax_rules(cdr(tr_expr), g_runtime_env, macro_name);
    if (g_runtime_env != nullptr)
        g_runtime_env->bind(macro_name, t);
    return VOID_VALUE;
}

static Value expand_let_syntax(const Value& sexpr, bool is_letrec) {
    if (!is_cons(cdr(sexpr)))
        throw SchemeSyntaxError("let-syntax: malformed", src_of(sexpr));
    Value bindings = car(cdr(sexpr));
    Value body     = cdr(cdr(sexpr));
    if (!is_cons(body))
        throw SchemeSyntaxError("let-syntax: empty body", src_of(sexpr));

    Environment* outer_env = g_runtime_env;
    Environment* child_env = gc_alloc_environment(outer_env);

    if (is_letrec) g_runtime_env = child_env;
    struct EnvRestorer { Environment* saved; ~EnvRestorer() { g_runtime_env = saved; } }
    _guard{outer_env};

    std::vector<Value> transformers;
    RenameTable let_rename_table;
    Value cur = bindings;
    while (is_cons(cur)) {
        Value b = car(cur);
        if (!is_cons(b) || !is_symbol(car(b)) || !is_cons(cdr(b)))
            throw SchemeSyntaxError("let-syntax: malformed binding", src_of(b));
        std::string bname = as_symbol(car(b));
        Value tr_expr = car(cdr(b));
        if (!is_cons(tr_expr) || !is_symbol(car(tr_expr)) ||
            as_symbol_id(car(tr_expr)) != sid_syntax_rules)
            throw SchemeSyntaxError(
                "let-syntax: transformer must be (syntax-rules ...)", src_of(tr_expr));
        Value t = parse_syntax_rules(cdr(tr_expr), g_runtime_env, bname);
        if (is_letrec) {
            child_env->bind(bname, t);
        } else {
            std::string gs = hygiene_gensym(bname);
            child_env->bind(gs, t);
            let_rename_table[intern_symbol(bname)] = intern_symbol(gs);
        }
        transformers.push_back(t);
        cur = cdr(cur);
    }

    // letrec-syntax: fix up self/mutual references
    if (is_letrec && !transformers.empty()) {
        Environment* global_env = child_env->getGlobalEnv();
        for (const Value& t_obj : transformers) {
            for (const auto& [fid_sid, gs_sid] : as_syntax_transformer_free_id_map(t_obj)) {
                auto it = child_env->_bindings.find(fid_sid);
                if (it != child_env->_bindings.end())
                    global_env->bind_id(gs_sid, it->second);
            }
        }
    }

    g_runtime_env = child_env;
    // Wrap multi-form body in begin
    Value wrapped;
    if (is_cons(cdr(body))) {
        std::vector<Value> body_items = {make_symbol("begin", nullptr)};
        Value bcur = body;
        while (is_cons(bcur)) { body_items.push_back(car(bcur)); bcur = cdr(bcur); }
        wrapped = list_from_items(body_items, nullptr);
    } else {
        wrapped = car(body);
    }
    if (!let_rename_table.empty())
        wrapped = rename_refs_in_form(wrapped, let_rename_table);
    return expand(wrapped);
    // _guard restores outer_env
}

static Value expand_inner(Value sexpr) {
    std::call_once(s_init_flag, init_sugar_handlers);
    for (int iterations = 0; ; iterations++) {
        if (iterations >= MAX_EXPAND_ITER)
            throw SchemeSyntaxError("macro expansion limit exceeded - possible infinite loop",
                                    src_of(sexpr));
        if (!is_cons(sexpr)) return sexpr;
        Value head = car(sexpr);
        if (is_symbol(head)) {
            uint32_t name_id = as_symbol_id(head);
            if (name_id == sid_define_syntax)  return expand_define_syntax(sexpr);
            if (name_id == sid_let_syntax)     return expand_let_syntax(sexpr, false);
            if (name_id == sid_letrec_syntax)  return expand_let_syntax(sexpr, true);
            // User macro
            Value tr = lookup_macro(head);
            if (!is_nil(tr)) {
                sexpr = apply_syntax_transformer(tr, sexpr);
                continue;
            }
            // Sugar
            auto it = s_sugar_handlers.find(name_id);
            if (it != s_sugar_handlers.end()) return it->second(sexpr);
        }
        if (is_syntax_transformer(head)) {
            sexpr = apply_syntax_transformer(head, sexpr);
            continue;
        }
        return expand_list(sexpr);
    }
}

// ── Public expand ─────────────────────────────────────────────────────────────

Value expand(const Value& sexpr) {
    g_expand_depth++;
    struct DepthGuard { ~DepthGuard() { g_expand_depth--; } } _guard;
    if (g_expand_depth > MAX_EXPAND_DEPTH)
        throw SchemeSyntaxError(
            "macro expansion depth exceeded - possible mutually-recursive macro loop",
            src_of(sexpr));
    return expand_inner(sexpr);
}
