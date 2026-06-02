// Context.cpp -- interpreter context.
// Direct port of pyscheme/Context.py.
#include "Context.h"
#include "Tracer.h"
#include <iostream>
#include <string>

Context::Context(std::ostream* out)
    : outStrm(out ? out : &std::cout) {}

void Context::_update_instrumented()
   {
   bool active = _debugging;
   if (tracer != nullptr && tracer->_active)
      active = true;
   _instrumented = active;
   }

void Context::write(const std::string& text)
   {
   outStrm->write(text.data(), (std::streamsize)text.size());
   }
