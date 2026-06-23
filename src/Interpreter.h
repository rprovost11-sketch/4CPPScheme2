#pragma once
// Interpreter.h -- interpreter adapter: wires Parser + Expander + Analyzer + cek_eval.
// Direct port of pyscheme/Interpreter.py.
#include "Analyzer.h"
#include "Context.h"
#include "Listener.h"
#include "Tracer.h"
#include "scheme_export.h"
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

class CPPSCHEME2_API Interpreter : public InterpreterBase
   {
 public:
   // library_paths: extra -L/-I search-path directories from the command line,
   // prepended to SCHEME_LIBRARY_PATH when the current-library-path parameter is
   // built in reboot().
   // load_rc=false (the --no-rc CLI flag) boots a pristine global with no
   // ~/.cppscheme2rc -- the state the .log test runner reboots into, so a
   // subprocess interpreter can run the golden battery without rc pollution.
   Interpreter(std::vector<std::string> library_paths = {}, bool load_rc = true);
   ~Interpreter();

   void reboot(std::ostream* outStrm = nullptr, bool load_rc = true) override;

   // Parse, expand, analyze, and evaluate every top-level form in source.
   // Returns the value of the last form, or nullopt if source had no forms.
   std::optional<Value> rawEval(const std::string& source,
                                std::ostream* outStrm = nullptr,
                                const std::string& filename = "");

   std::string eval(const std::string& source,
                    std::ostream* outStrm = nullptr) override;

   void evalFile(const std::string& filename,
                 std::ostream* outStrm = nullptr) override;

   void set_debug_input_fn(
       std::function<std::string(const std::string&, const std::string&)> fn,
       void* rl = nullptr) override;

   Context* get_ctx() override
      {
      return &_ctx;
      }
   Environment* get_env() override
      {
      return _env;
      }

 private:
   Environment* _env = nullptr;
   StaticEnv _static_env;
   Context _ctx;
   Tracer _tracer;
   std::vector<std::string> _cli_library_paths;

   void _wire_ctx_leval();
   };
