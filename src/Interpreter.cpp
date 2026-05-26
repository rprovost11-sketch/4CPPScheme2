// Interpreter.cpp -- interpreter adapter: wires Parser + Expander + Analyzer + cek_eval.
// Direct port of pyscheme/Interpreter.py.
#include "Interpreter.h"
#include "Debugger.h"
#include "Evaluator.h"
#include "Expander.h"
#include "Parser.h"
#include "PrettyPrinter.h"
#include "gc.h"
#include "library.h"
#include "primitives/ports.h"
#include "primitives/primitives.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

Interpreter::Interpreter() {
    _ctx.tracer = &_tracer;
    _tracer._ctx = &_ctx;
    _wire_ctx_leval();
    // Register _env as a GC root so it stays alive across cek_eval calls
    // (between calls, no trace hook protects it).
    gc_env_root_push(&_env);
    reboot();
}

Interpreter::~Interpreter() {
    gc_env_root_pop(&_env);
}


void Interpreter::_wire_ctx_leval() {
    _ctx.lEval = [this](Environment* env, const Value& expr) -> Value {
        return cek_eval(expr, env, &_ctx);
    };
}

void Interpreter::set_debug_input_fn(
    std::function<std::string(const std::string&, const std::string&)> fn,
    void* rl)
{
    _ctx.debugger->input_fn       = fn;
    _ctx.debugger->_has_readline  = (rl != nullptr);
}

void Interpreter::reboot(std::ostream* outStrm, bool load_rc) {
    _ctx.debugger = new Debugger();
    _ctx.tracer->reset();
    reset_current_port_params();
    _env = gc_alloc_environment(nullptr);
    install_primitives(_env);
    register_standard_libraries(_env);
    set_runtime_env(_env);
    set_global_env(_env);
    _static_env = StaticEnv(primitive_arities());
    if (outStrm != nullptr)
        _ctx.outStrm = outStrm;
    if (load_rc) {
        const char* home = std::getenv("USERPROFILE");
        if (!home) home = std::getenv("HOME");
        if (home) {
            std::filesystem::path rc =
                std::filesystem::path(home) / ".cekschemerc";
            if (std::filesystem::is_regular_file(rc)) {
                try {
                    evalFile(rc.string());
                } catch (const std::exception& e) {
                    std::cerr << "cekscheme: error loading ~/.cekschemerc: "
                              << e.what() << '\n';
                }
            }
        }
    }
}

std::optional<Value> Interpreter::rawEval(
    const std::string& source,
    std::ostream* outStrm,
    const std::string& filename)
{
    std::ostream* prev_out = _ctx.outStrm;
    if (outStrm != nullptr)
        _ctx.outStrm = outStrm;
    _ctx.shadow_stack.clear();
    try {
        std::vector<Value> forms = scheme_parse(source, filename);
        std::optional<Value> last;
        for (auto& form : forms) {
            Value expanded = expand(form);
            analyze(expanded, _static_env);
            extend_static_env_with_define(_static_env, expanded);
            last = cek_eval(expanded, _env, &_ctx);
        }
        _ctx.outStrm = prev_out;
        return last;
    } catch (...) {
        _ctx.outStrm = prev_out;
        throw;
    }
}

std::string Interpreter::eval(const std::string& source, std::ostream* outStrm) {
    auto raw = rawEval(source, outStrm);
    if (!raw || is_void(*raw))
        return "";
    return scheme_pretty_print(*raw);
}

void Interpreter::evalFile(const std::string& filename, std::ostream* outStrm) {
    std::filesystem::path abs_path = std::filesystem::absolute(filename);
    std::ifstream f(abs_path);
    if (!f.is_open())
        throw std::runtime_error("cannot open file: " + filename);
    std::string source((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    rawEval(source, outStrm, abs_path.string());
}
