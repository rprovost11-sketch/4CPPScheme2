#pragma once
// Tracer.h -- function call tracer.
// Direct port of pyscheme/Tracer.py.
#include "AST.h"
#include "scheme_export.h"
#include <iosfwd>
#include <string>
#include <unordered_set>
#include <vector>

struct Context;

struct CEKSCHEME_API Tracer {
    std::unordered_set<std::string> _fns_to_trace;
    int      _depth  = 0;
    bool     _active = false;
    Context* _ctx    = nullptr;

    void reset();
    void add_fn(const std::string& name);
    void remove_fn(const std::string& name);
    void remove_all();
    std::unordered_set<std::string> get_fns() const;

    // Returns true if the entry line was printed (caller increments depth + pushes FRAME_TRACE_EXIT).
    bool trace_enter(const std::string& name, const std::vector<Value>& args,
                     int depth, std::ostream* out);
    void trace_exit(const std::string& name, const Value& val,
                    int depth, std::ostream* out);
};
