// primitives/debug.cpp -- debug / inspection primitives.
// Direct port of pyscheme/primitives/debug.py.
#include "debug.h"
#include "primitives.h"
#include "../AST.h"
#include "../Environment.h"
#include "../Context.h"
#include "../PrettyPrinter.h"
#include <string>
#include <sstream>

static const char* CATEGORY = "debug";

static SourceInfo* _src(const Value* a) { return a ? src_of(*a) : nullptr; }

// ── Type name helper ──────────────────────────────────────────────────────────

static std::string _scheme_type_name(const Value& val) {
    if (is_nil(val))             return "null";
    if (is_void(val))            return "void";
    if (is_boolean(val))         return "boolean";
    if (is_integer(val))         return "integer";
    if (is_real(val))            return "real";
    if (is_rational(val))        return "rational";
    if (is_complex(val))         return "complex";
    if (is_exact_complex(val))   return "exact-complex";
    if (is_character(val))       return "character";
    if (is_string(val))          return "string";
    if (is_symbol(val))          return "symbol";
    if (is_closure(val))         return "procedure";
    if (is_case_closure(val))    return "procedure";
    if (is_primitive(val))       return "primitive";
    if (is_promise(val))         return "promise";
    if (is_multi_values(val))    return "values";
    if (is_continuation(val))    return "continuation";
    if (is_environment(val))     return "environment";
    if (is_record_type(val))     return "record-type";
    if (is_record(val))          return "record";
    if (is_record_accessor(val)) return "record-accessor";
    if (is_record_mutator(val))  return "record-mutator";
    if (is_parameter(val))       return "parameter";
    if (is_vector(val))          return "vector";
    if (is_bytevector(val))      return "bytevector";
    if (is_port(val))            return "port";
    if (is_cons(val))            return "pair";
    if (is_eof(val))             return "eof";
    return "unknown";
}

// ── Object description ────────────────────────────────────────────────────────

static void _describe_object(const Value& val, Context* ctx) {
    auto wl = [&](const std::string& line) { ctx->write(line + "\n"); };

    if (is_nil(val))  { wl("() is the empty list (null)"); return; }
    if (is_void(val)) { wl("#<void> is void"); return; }

    if (is_cons(val)) {
        std::string preview = scheme_pretty_print(val);
        if (preview.size() > 60) preview = preview.substr(0, 57) + "...";
        int64_t length = 0;
        Value cur = val;
        while (is_cons(cur)) { ++length; cur = cdr(cur); }
        if (is_nil(cur))
            wl(preview + " is a proper list, length " + std::to_string(length));
        else
            wl(preview + " is an improper pair");
        return;
    }

    if (is_string(val)) {
        wl(scheme_pretty_print(val) + " is a string");
        wl("  Length: " + std::to_string(as_string(val).size()));
        return;
    }
    if (is_boolean(val)) { wl(std::string(as_boolean(val) ? "#t" : "#f") + " is a boolean"); return; }
    if (is_integer(val)) { wl(std::to_string(as_integer(val)) + " is an integer"); return; }
    if (is_real(val))    { wl(scheme_pretty_print(val) + " is a real"); return; }
    if (is_rational(val)) {
        wl(scheme_pretty_print(val) + " is a rational");
        wl("  Numerator:   " + std::to_string(as_rational_num(val)));
        wl("  Denominator: " + std::to_string(as_rational_den(val)));
        return;
    }
    if (is_complex(val) || is_exact_complex(val)) {
        wl(scheme_pretty_print(val) + " is a complex");
        return;
    }
    if (is_character(val)) { wl(scheme_pretty_print(val) + " is a character"); return; }
    if (is_symbol(val))    { wl(as_symbol(val) + " is a symbol"); return; }

    if (is_closure(val)) {
        const auto& param_ids = as_closure_params(val);
        uint32_t rest_id = as_closure_rest_name(val);
        std::vector<std::string> parts;
        for (uint32_t id : param_ids) parts.push_back(symbol_name(id));
        std::string params_str;
        if (rest_id != UINT32_MAX && parts.empty()) {
            params_str = symbol_name(rest_id);
        } else if (rest_id != UINT32_MAX) {
            std::string joined; for (auto& p : parts) joined += " " + p;
            params_str = "(" + joined.substr(1) + " . " + symbol_name(rest_id) + ")";
        } else if (!parts.empty()) {
            std::string joined; for (auto& p : parts) joined += " " + p;
            params_str = "(" + joined.substr(1) + ")";
        } else {
            params_str = "()";
        }
        wl("#<procedure " + params_str + "> is a procedure");
        const std::string& doc = as_closure_docstring(val);
        if (!doc.empty()) {
            wl("  Documentation:");
            std::istringstream ss(doc);
            std::string ln;
            while (std::getline(ss, ln)) wl("    " + ln);
        }
        return;
    }

    if (is_case_closure(val)) {
        const auto& clauses = as_case_closure_clauses(val);
        wl("#<case-lambda, " + std::to_string(clauses.size()) + " clause(s)> is a procedure");
        const std::string& doc = as_case_closure_docstring(val);
        if (!doc.empty()) {
            wl("  Documentation:");
            std::istringstream ss(doc);
            std::string ln;
            while (std::getline(ss, ln)) wl("    " + ln);
        }
        return;
    }

    if (is_primitive(val)) {
        const std::string& name = as_primitive_name(val);
        wl(name + " is a primitive procedure");
        const auto& ph = primitive_help();
        auto it = ph.find(name);
        if (it != ph.end()) {
            if (it->second.kind == "special") wl("  Kind: special form");
            wl("  Usage: " + it->second.usage);
            if (!it->second.doc.empty()) {
                wl("  Documentation:");
                std::istringstream ss(it->second.doc);
                std::string ln;
                while (std::getline(ss, ln)) wl("    " + ln);
            }
        }
        return;
    }

    if (is_promise(val)) {
        if (as_promise_is_done(val)) {
            wl("#<promise, forced> is a promise");
            wl("  Value: " + scheme_pretty_print(as_promise_payload(val)));
        } else {
            wl("#<promise, unforced> is a promise");
        }
        return;
    }
    if (is_continuation(val)) { wl("#<continuation> is a continuation"); return; }
    if (is_multi_values(val)) {
        const auto& vals = as_multi_values_list(val);
        wl("#<values> contains " + std::to_string(vals.size()) + " value(s)");
        for (size_t i = 0; i < vals.size(); ++i)
            wl("  " + std::to_string(i) + ": " + scheme_pretty_print(vals[i]));
        return;
    }
    if (is_record(val)) {
        const std::string& type_name = as_record_type_name(val);
        const auto& fields = as_record_fields_const(val);
        wl(scheme_pretty_print(val) + " is a " + type_name + " (record)");
        wl("  Fields: " + std::to_string(fields.size()));
        return;
    }
    if (is_parameter(val)) {
        wl("#<parameter> is a parameter object");
        wl("  Value: " + scheme_pretty_print(as_parameter_value(val)));
        return;
    }
    if (is_vector(val)) {
        wl("#(...) is a vector, length " + std::to_string(as_vector_items_const(val).size()));
        return;
    }
    if (is_bytevector(val)) {
        wl("#u8(...) is a bytevector, length " + std::to_string(as_bytevector_items_const(val).size()));
        return;
    }
    if (is_port(val)) {
        Port* p = as_port(val);
        wl(std::string("#<port> is a port, status: ") + (p->is_open ? "open" : "closed"));
        return;
    }
    wl(scheme_pretty_print(val) + " is of type " + _scheme_type_name(val));
}

// ── Inspectable children ──────────────────────────────────────────────────────

struct InspectChild { std::string label; Value child; };

static std::vector<InspectChild> _inspectable_children(const Value& val) {
    std::vector<InspectChild> result;
    if (is_cons(val)) {
        Value cur = val;
        int idx = 0;
        while (is_cons(cur)) { result.push_back({ std::to_string(idx++), car(cur) }); cur = cdr(cur); }
        if (!is_nil(cur)) result.push_back({ "tail", cur });
        return result;
    }
    if (is_multi_values(val)) {
        const auto& vals = as_multi_values_list(val);
        for (size_t i = 0; i < vals.size(); ++i)
            result.push_back({ std::to_string(i), vals[i] });
        return result;
    }
    if (is_record(val)) {
        const auto& fields = as_record_fields_const(val);
        for (size_t i = 0; i < fields.size(); ++i)
            result.push_back({ std::to_string(i), fields[i] });
        return result;
    }
    if (is_vector(val)) {
        const auto& items = as_vector_items_const(val);
        for (size_t i = 0; i < items.size(); ++i)
            result.push_back({ std::to_string(i), items[i] });
        return result;
    }
    return result;
}

static void _print_inspect(const Value& val, Context* ctx) {
    auto wl = [&](const std::string& line) { ctx->write(line + "\n"); };
    auto children = _inspectable_children(val);
    if (children.empty()) { _describe_object(val, ctx); return; }
    if (is_cons(val)) {
        int64_t length = 0;
        Value cur = val;
        while (is_cons(cur)) { ++length; cur = cdr(cur); }
        if (is_nil(cur))
            wl("pair (proper list), " + std::to_string(length) + " elements");
        else
            wl("pair (improper), " + std::to_string(length) + " car(s) + tail");
    } else if (is_multi_values(val)) {
        wl("values, " + std::to_string(as_multi_values_list(val).size()) + " element(s)");
    } else if (is_record(val)) {
        wl(as_record_type_name(val) + " (record), " + std::to_string(children.size()) + " field(s)");
    } else if (is_vector(val)) {
        wl("vector, " + std::to_string(as_vector_items_const(val).size()) + " element(s)");
    }
    for (auto& ch : children)
        wl("  " + ch.label + ": " + scheme_pretty_print(ch.child));
}

// Non-interactive in C++: just print once (no interactive input loop).
static void _run_inspect(const Value& val, Context* ctx) {
    _print_inspect(val, ctx);
}

void debug_run_inspect(const Value& val, Context* ctx) {
    _run_inspect(val, ctx);
}

// ── trace / untrace stubs ─────────────────────────────────────────────────────

static Value _stub_trace(Context*, Environment*, std::vector<Value>&, const Value* app) {
    throw SchemeTypeError("trace: not callable as a procedure", _src(app));
}

static Value _stub_untrace(Context*, Environment*, std::vector<Value>&, const Value* app) {
    throw SchemeTypeError("untrace: not callable as a procedure", _src(app));
}

// ── Primitives ────────────────────────────────────────────────────────────────

static Value _prim_describe(Context* ctx, Environment*, std::vector<Value>& args, const Value*) {
    _describe_object(args[0], ctx);
    return VOID_VALUE;
}

static Value _prim_inspect(Context* ctx, Environment*, std::vector<Value>& args, const Value*) {
    _run_inspect(args[0], ctx);
    return VOID_VALUE;
}

static Value _prim_debug(Context*, Environment*, std::vector<Value>&, const Value* app) {
    throw SchemeTypeError("debug: debugger not implemented (Phase 0 stub)", _src(app));
}

// ── Registration ──────────────────────────────────────────────────────────────

void register_debug() {
    register_primitive("describe", 1, 1, _prim_describe, "",
        "Print a structured description of the argument: type, attributes, and documentation.",
        CATEGORY);
    register_primitive("inspect",  1, 1, _prim_inspect,  "",
        "Show a structured inspection view with numbered child elements.",
        CATEGORY);
    register_primitive("debug",    0, 0, _prim_debug,    "",
        "Open the interactive debugger.  (Not yet implemented in CPPScheme2.)",
        CATEGORY);
    register_primitive("trace",    0, -1, _stub_trace,
        "(trace . names)",
        "Enable call tracing for each named function.  Special form intercepted by evaluator.",
        CATEGORY, "special");
    register_primitive("untrace",  0, -1, _stub_untrace,
        "(untrace . names)",
        "Disable call tracing for each named function.  Special form intercepted by evaluator.",
        CATEGORY, "special");
}
