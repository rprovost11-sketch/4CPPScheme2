#pragma once
// primitives/ports.h -- port and I/O primitives.
// Direct port of pyscheme/primitives/ports.py.
#include "../scheme_export.h"

// Port of ports.py reset_current_port_params().
// Called by the Interpreter/library layer to reset module-level port state.
CPPSCHEME2_API void reset_current_port_params();
