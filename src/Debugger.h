#pragma once
// Debugger.h -- interactive step debugger, breakpoints, watches.
// Direct port of pyscheme/Debugger.py.
#include "AST.h"
#include "Evaluator.h"
#include "scheme_export.h"
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct Context;
struct Environment;
struct ConsCell;

// ── _RestartRd ────────────────────────────────────────────────────────────────
// Port of Debugger.py _RestartRd.  Thrown by _cmd_rd to restart the current
// rd expression from the beginning.
struct _RestartRd {};

// ── StepHook ──────────────────────────────────────────────────────────────────
// Port of Debugger.py StepHook.
struct CEKSCHEME_API StepHook {
    std::optional<int> _skip_until_depth;  // nullopt = step every expression
    bool               _step_over_first = false;
};

// ── InnerTarget ───────────────────────────────────────────────────────────────
// One entry in _inner_targets: breakpoint on a specific call site in a body.
// Port of the 5-element list [display, cond, node, fn_name, fn_obj].
struct CEKSCHEME_API InnerTarget {
    std::string display;   // e.g. "fib:fib:1"
    std::string cond;      // empty = no condition (Python None)
    Value       node;      // the cons-cell call node
    std::string fn_name;
    Value       fn_obj;    // closure value for stale-check
};

// ── Debugger ──────────────────────────────────────────────────────────────────
// Port of Debugger.py Debugger.
class CEKSCHEME_API Debugger {
public:
    // Public state accessed by Interpreter::set_debug_input_fn.
    std::function<std::string(const std::string&, const std::string&)> input_fn;
    bool _has_readline = false;

    // Breakpoint tables (port of Python instance attributes).
    std::unordered_map<std::string, std::string>   breakpoints;      // name -> cond (empty=none)
    std::unordered_map<const ConsCell*, InnerTarget> _inner_targets;  // node ptr -> entry
    std::unordered_set<std::string>                _disabled;
    std::vector<std::string>                       _bp_index;
    std::vector<std::string>                       watch_list;
    std::unordered_set<std::string>                break_on;

    Debugger();

    // Called by CEK loop for every cons expression when instrumentation is on.
    void on_expr(const Value& C, Environment* E, const KStack& K, Context* ctx);

    // Main debugger REPL (entered via ]debug command).
    Value run_debugger_repl(Context* ctx, Environment* env);

    // Eval helpers called by command handlers.
    void print_watch(Environment* env, Context* ctx);
    void safe_eval   (Context* ctx, Environment* env, const std::string& source);
    void safe_inspect(Context* ctx, Environment* env, const std::string& source);
    void debug_eval  (Context* ctx, Environment* env, const std::string& source);

private:
    std::optional<StepHook>  _step_hook;
    std::vector<std::string> _history;
    std::vector<std::string> _saved_history;
    std::string              _last_rd_expr;    // empty = none
    const KStack*            _current_K = nullptr;

    // Command dispatch (port of _commands dict and _help_methods list).
    using CmdFn = std::function<void(const std::string&, const std::string&,
                                     Context*, Environment*)>;
    std::unordered_map<std::string, CmdFn>       _commands;
    std::unordered_map<std::string, std::string> _cmd_help;   // base -> docstring
    std::vector<std::string>                     _help_order; // ordered base names

    // Annotation map: ConsCell* -> tag string (used for body display).
    using AnnMap = std::unordered_map<const ConsCell*, std::string>;

    // ── Static helpers ────────────────────────────────────────────────────────
    static std::string _cmd_word(const std::string& line);
    static std::string _cmd_rest(const std::string& line);
    static bool        _is_callable(const Value& val);
    static bool        _value_identity_equal(const Value& a, const Value& b);

    static void _collect_locals(Environment* env,
                                std::unordered_map<uint32_t, Value>& result);
    static void _print_scoped_locals(Environment* env, int max_depth = -1);
    static void _print_named_vars(Environment* env,
                                  const std::vector<std::string>& names);

    static std::vector<std::pair<std::string, std::string>>
        _parse_bp_args(const std::string& rest);

    static std::optional<Value> _walk_form(const Value& cell,
                                            const std::string& callable_name,
                                            int& count, int target_n);
    static std::optional<Value> _find_nth_call(const Value& body_cons,
                                                const std::string& callable_name,
                                                int target_n);
    static void _collect_one(const Value& cell, const std::string& callable_name,
                              std::vector<Value>& results);
    static std::vector<Value> _collect_calls(const Value& body_cons,
                                              const std::string& callable_name);

    static bool        _ann_has_any(const Value& cell, const AnnMap& annotations);
    static std::string _ann_flat(const Value& cell, const AnnMap& annotations);
    static std::string _ann_fmt(const Value& cell, const AnnMap& annotations, int ind);
    static void        _print_annotated(const Value& form, const AnnMap& annotations,
                                         int indent = 0);

    static std::vector<std::pair<std::string, std::string>>
        _parse_help_entries(const std::string& docstring);
    static void _print_help_entries(
        const std::vector<std::pair<std::string, std::string>>& entries);

    static std::vector<std::string> _parse_watch_args(const std::string& source);

    // ── Instance helpers ──────────────────────────────────────────────────────
    void _swap_history_in();
    void _swap_history_out();
    std::string _resolve_bp_ref(const std::string& ref);
    void _prune_stale_inner(Environment* env);
    void _set_inner_breakpoint(const std::string& spec, const std::string& cond,
                                Environment* env);
    bool _dispatch(const std::string& line, Context* ctx, Environment* env);

    // ── Command handlers ──────────────────────────────────────────────────────
    void _cmd_abort(const std::string&, const std::string&, Context*, Environment*);
    void _cmd_b    (const std::string&, const std::string&, Context*, Environment*);
    void _cmd_bc   (const std::string&, const std::string&, Context*, Environment*);
    void _cmd_bo   (const std::string&, const std::string&, Context*, Environment*);
    void _cmd_bt   (const std::string&, const std::string&, Context*, Environment*);
    void _cmd_body (const std::string&, const std::string&, Context*, Environment*);
    void _cmd_c    (const std::string&, const std::string&, Context*, Environment*);
    void _cmd_e    (const std::string&, const std::string&, Context*, Environment*);
    void _cmd_h    (const std::string&, const std::string&, Context*, Environment*);
    void _cmd_i    (const std::string&, const std::string&, Context*, Environment*);
    void _cmd_n    (const std::string&, const std::string&, Context*, Environment*);
    void _cmd_o    (const std::string&, const std::string&, Context*, Environment*);
    void _cmd_q    (const std::string&, const std::string&, Context*, Environment*);
    void _cmd_rd   (const std::string&, const std::string&, Context*, Environment*);
    void _cmd_s    (const std::string&, const std::string&, Context*, Environment*);
    void _cmd_v    (const std::string&, const std::string&, Context*, Environment*);
    void _cmd_w    (const std::string&, const std::string&, Context*, Environment*);

    // ── Prompt loop ───────────────────────────────────────────────────────────
    // Returns "step", "continue", "abort", or "quit".  May throw _RestartRd.
    // depth=-1 means main debugger REPL (not during execution).
    std::string _prompt_loop(Context* ctx, Environment* env,
                              int depth = -1,
                              StepHook* step_hook = nullptr);
};
