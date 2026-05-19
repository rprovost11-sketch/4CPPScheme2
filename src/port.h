#pragma once
#include "value.h"
#include <string>
#include <vector>

// SchemePort — R7RS I/O port.
// Full port state (buf, pos, file handle, etc.) will be fleshed out in Phase 2.
struct SchemePort
   {
   GcHeader    header{GcType::Port};
   bool        is_input  = false;
   bool        is_output = false;
   bool        is_open   = true;
   bool        is_text   = true;    // true=textual, false=binary
   std::string name;
   };
