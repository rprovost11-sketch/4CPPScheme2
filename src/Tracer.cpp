// Tracer.cpp -- function call tracer.
// Direct port of pyscheme/Tracer.py.
#include "Tracer.h"
#include "Context.h"
#include "PrettyPrinter.h"
#include <ostream>
#include <string>

void Tracer::reset()
   {
   _fns_to_trace.clear();
   _depth = 0;
   _active = false;
   }

void Tracer::add_fn(const std::string& name)
   {
   _fns_to_trace.insert(name);
   _active = true;
   if (_ctx)
      _ctx->_update_instrumented();
   }

void Tracer::remove_fn(const std::string& name)
   {
   _fns_to_trace.erase(name);
   _active = !_fns_to_trace.empty();
   if (_ctx)
      _ctx->_update_instrumented();
   }

void Tracer::remove_all()
   {
   _fns_to_trace.clear();
   _active = false;
   if (_ctx)
      _ctx->_update_instrumented();
   }

std::unordered_set<std::string> Tracer::get_fns() const
   {
   return _fns_to_trace;
   }

bool Tracer::trace_enter(const std::string& name, const std::vector<Value>& args,
                         int depth, std::ostream* out)
   {
   if (_fns_to_trace.find(name) == _fns_to_trace.end())
      return false;
   std::string indent;
   for (int i = 0; i < depth; ++i)
      indent += "  ";
   std::string line;
   if (args.empty())
      {
      line = std::to_string(depth);
      while ((int)line.size() < 2)
         line = " " + line;
      line += ": " + indent + "(" + name + ")";
      }
   else
      {
      std::string arg_str;
      for (size_t i = 0; i < args.size(); ++i)
         {
         if (i > 0)
            arg_str += " ";
         arg_str += scheme_pretty_print(args[i]);
         }
      line = std::to_string(depth);
      while ((int)line.size() < 2)
         line = " " + line;
      line += ": " + indent + "(" + name + " " + arg_str + ")";
      }
   *out << line << "\n";
   return true;
   }

void Tracer::trace_exit(const std::string& name, const Value& val,
                        int depth, std::ostream* out)
   {
   std::string indent;
   for (int i = 0; i < depth; ++i)
      indent += "  ";
   std::string line = std::to_string(depth);
   while ((int)line.size() < 2)
      line = " " + line;
   line += ": " + indent + name + " returned " + scheme_pretty_print(val);
   *out << line << "\n";
   }
