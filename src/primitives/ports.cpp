// primitives/ports.cpp -- port / I/O primitives.
// Direct port of pyscheme/primitives/ports.py.
#include "ports.h"
#include "primitives.h"
#include "../AST.h"
#include "../Environment.h"
#include "../Context.h"
#include "../gc.h"
#include "../Evaluator.h"
#include "../Parser.h"
#include "../PrettyPrinter.h"
#include <cstdio>
#include <cstring>
#include <optional>
#include <filesystem>
#include <fstream>

static const char* CATEGORY = "ports";

static SourceInfo* _src(const Value* a)
   {
   return a ? src_of(*a) : nullptr;
   }

// ── Static current-port parameter storage ────────────────────────────────────
// Mirrors Python's module-level _current_*_param / _current_*_default lists.

// The current-port parameters hold Parameter Values that are GC-managed.
// They must be registered as GC roots so the GC traces them across cek_eval
// calls.  Use plain Value + a has_value flag (rather than std::optional<Value>)
// so we can take a stable address for gc_root_push.
static Value s_current_input_param;
static Value s_current_output_param;
static Value s_current_error_param;
static bool s_current_input_has = false;
static bool s_current_output_has = false;
static bool s_current_error_has = false;
static Port* s_output_default_ptr = nullptr;
static Value s_capture_out_port; // one stable <capture> port (see _get_current_output)
static bool s_capture_out_has = false;
static bool s_port_roots_registered = false;

static void _register_port_roots_once()
   {
   if (s_port_roots_registered)
      return;
   gc_root_push(&s_current_input_param);
   gc_root_push(&s_current_output_param);
   gc_root_push(&s_current_error_param);
   gc_root_push(&s_capture_out_port);
   s_port_roots_registered = true;
   }

void reset_current_port_params()
   {
   s_current_input_param = Value{};
   s_current_output_param = Value{};
   s_current_error_param = Value{};
   s_current_input_has = false;
   s_current_output_has = false;
   s_current_error_has = false;
   s_output_default_ptr = nullptr;
   s_capture_out_port = Value{};
   s_capture_out_has = false;
   }

// ── Port construction helpers ─────────────────────────────────────────────────

static Value _make_stdio_port(bool is_input, bool is_text, std::FILE* fh, const std::string& name)
   {
   Value v = make_port(is_input, is_text, name);
   as_port(v)->file_h = fh;
   return v;
   }

static Value _stdin_port()
   {
   return _make_stdio_port(true, true, stdin, "<stdin>");
   }
static Value _stdout_port()
   {
   return _make_stdio_port(false, true, stdout, "<stdout>");
   }
static Value _stderr_port()
   {
   return _make_stdio_port(false, true, stderr, "<stderr>");
   }

// ── Current-port accessors ────────────────────────────────────────────────────

static Value _get_current_input(Context*)
   {
   _register_port_roots_once();
   if (!s_current_input_has)
      {
      s_current_input_param = make_parameter(_stdin_port(), NIL_VALUE);
      s_current_input_has = true;
      }
   return as_parameter_value(s_current_input_param);
   }

static Value _get_current_output(Context* ctx)
   {
   _register_port_roots_once();
   if (!s_current_output_has)
      {
      Value default_port = _stdout_port();
      s_output_default_ptr = as_port(default_port);
      s_current_output_param = make_parameter(default_port, NIL_VALUE);
      s_current_output_has = true;
      }
   Value p = as_parameter_value(s_current_output_param);
   // If ctx has a capture stream and parameter is still at the default, return
   // a capture port so writes go through ctx->write().  Return ONE stable port
   // object (not a fresh one each call) so (current-output-port) is eq?-
   // consistent across calls -- otherwise (eq? (current-output-port) saved)
   // is always #f.  The <capture> port resolves the live ctx at write time
   // (see _emit_to_port), so a single object stays correct.  R7RS 6.13.1.
   if (ctx && ctx->outStrm && as_port(p) == s_output_default_ptr)
      {
      if (!s_capture_out_has)
         {
         s_capture_out_port = make_port(false, true, "<capture>");
         s_capture_out_has = true;
         }
      return s_capture_out_port;
      }
   return p;
   }

static Value _get_current_error(Context*)
   {
   _register_port_roots_once();
   if (!s_current_error_has)
      {
      s_current_error_param = make_parameter(_stderr_port(), NIL_VALUE);
      s_current_error_has = true;
      }
   return as_parameter_value(s_current_error_param);
   }

// Return the internal parameter object backing a current-*-port accessor
// primitive, or VOID_VALUE if `name` is not one of them.  R7RS 6.13.1 specifies
// current-output-port / current-input-port / current-error-port as parameter
// objects; they are exposed as 0-arg accessor primitives (so the harness can
// redirect output), but parameterize must be able to rebind them.  Called by
// the evaluator's parameterize wind builder.  (Declared extern in Evaluator.cpp.)
Value port_parameter_for_accessor(const std::string& name, Context* ctx)
   {
   if (name == "current-output-port")
      {
      _get_current_output(ctx);
      return s_current_output_param;
      }
   if (name == "current-input-port")
      {
      _get_current_input(ctx);
      return s_current_input_param;
      }
   if (name == "current-error-port")
      {
      _get_current_error(ctx);
      return s_current_error_param;
      }
   return VOID_VALUE;
   }

// ── Port check helpers ────────────────────────────────────────────────────────

static Port* _check_port(const Value& v, const char* name, const Value* app, int idx = 1)
   {
   if (!is_port(v))
      throw SchemeTypeError(
          std::string(name) + ": argument " + std::to_string(idx) + " must be a port",
          _src(app));
   Port* p = as_port(v);
   if (!p->is_open)
      throw SchemeTypeError(std::string(name) + ": port is closed", _src(app));
   return p;
   }

static Port* _check_input_port(const Value& v, const char* name, const Value* app, int idx = 1)
   {
   Port* p = _check_port(v, name, app, idx);
   if (!p->is_input)
      throw SchemeTypeError(
          std::string(name) + ": argument " + std::to_string(idx) + " must be an input port",
          _src(app));
   return p;
   }

static Port* _check_output_port(const Value& v, const char* name, const Value* app, int idx = 1)
   {
   Port* p = _check_port(v, name, app, idx);
   if (p->is_input)
      throw SchemeTypeError(
          std::string(name) + ": argument " + std::to_string(idx) + " must be an output port",
          _src(app));
   return p;
   }

static Port* _check_textual_input(const Value& v, const char* name, const Value* app, int idx = 1)
   {
   Port* p = _check_input_port(v, name, app, idx);
   if (!p->is_text)
      throw SchemeTypeError(
          std::string(name) + ": argument " + std::to_string(idx) + " must be a textual input port",
          _src(app));
   return p;
   }

static Port* _check_binary_input(const Value& v, const char* name, const Value* app, int idx = 1)
   {
   Port* p = _check_input_port(v, name, app, idx);
   if (p->is_text)
      throw SchemeTypeError(
          std::string(name) + ": argument " + std::to_string(idx) + " must be a binary input port",
          _src(app));
   return p;
   }

static Port* _check_textual_output(const Value& v, const char* name, const Value* app, int idx = 1)
   {
   Port* p = _check_output_port(v, name, app, idx);
   if (!p->is_text)
      throw SchemeTypeError(
          std::string(name) + ": argument " + std::to_string(idx) + " must be a textual output port",
          _src(app));
   return p;
   }

static Port* _check_binary_output(const Value& v, const char* name, const Value* app, int idx = 1)
   {
   Port* p = _check_output_port(v, name, app, idx);
   if (p->is_text)
      throw SchemeTypeError(
          std::string(name) + ": argument " + std::to_string(idx) + " must be a binary output port",
          _src(app));
   return p;
   }

// ── Emit helpers ──────────────────────────────────────────────────────────────

static void _emit_to_port(Context* ctx, Port* p, const std::string& text)
   {
   if (p->name == "<capture>")
      {
      if (ctx)
         ctx->write(text);
      }
   else if (p->file_h)
      {
      std::fputs(text.c_str(), p->file_h);
      }
   else
      {
      p->buf_text += text;
      }
   }

static void _emit_bytes_to_port(Port* p, const uint8_t* data, size_t n)
   {
   if (p->file_h)
      {
      std::fwrite(data, 1, n, p->file_h);
      }
   else
      {
      p->buf_binary.insert(p->buf_binary.end(), data, data + n);
      }
   }

static std::string _char32_to_utf8(char32_t c)
   {
   std::string s;
   if (c < 0x80)
      {
      s += static_cast<char>(c);
      }
   else if (c < 0x800)
      {
      s += static_cast<char>(0xC0 | (c >> 6));
      s += static_cast<char>(0x80 | (c & 0x3F));
      }
   else if (c < 0x10000)
      {
      s += static_cast<char>(0xE0 | (c >> 12));
      s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      s += static_cast<char>(0x80 | (c & 0x3F));
      }
   else
      {
      s += static_cast<char>(0xF0 | (c >> 18));
      s += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
      s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      s += static_cast<char>(0x80 | (c & 0x3F));
      }
   return s;
   }

static char32_t _decode_utf8_char(const std::string& s, size_t& pos)
   {
   auto b0 = static_cast<unsigned char>(s[pos]);
   if (b0 < 0x80)
      {
      pos++;
      return char32_t(b0);
      }
   if ((b0 & 0xE0) == 0xC0 && pos + 1 < s.size())
      {
      char32_t c = ((b0 & 0x1F) << 6) | (static_cast<unsigned char>(s[pos + 1]) & 0x3F);
      pos += 2;
      return c;
      }
   if ((b0 & 0xF0) == 0xE0 && pos + 2 < s.size())
      {
      char32_t c = ((b0 & 0x0F) << 12) | ((static_cast<unsigned char>(s[pos + 1]) & 0x3F) << 6) | (static_cast<unsigned char>(s[pos + 2]) & 0x3F);
      pos += 3;
      return c;
      }
   if ((b0 & 0xF8) == 0xF0 && pos + 3 < s.size())
      {
      char32_t c = ((b0 & 0x07) << 18) | ((static_cast<unsigned char>(s[pos + 1]) & 0x3F) << 12) | ((static_cast<unsigned char>(s[pos + 2]) & 0x3F) << 6) | (static_cast<unsigned char>(s[pos + 3]) & 0x3F);
      pos += 4;
      return c;
      }
   pos++;
   return char32_t(b0); // invalid UTF-8: return as Latin-1
   }

// ── display rendering ─────────────────────────────────────────────────────────

static std::string _render_display(const Value& val)
   {
   if (is_string(val))
      return as_string(val);
   if (is_character(val))
      return _char32_to_utf8(as_character(val));
   if (is_void(val))
      return "";
   if (is_cons(val) || is_vector(val))
      {
      // Cyclic values keep the existing behaviour: route to the write-style
      // datum-label renderer.  Acyclic ones render iteratively via the shared
      // task-stack helper, passing _render_display as the leaf renderer so list
      // and vector elements inherit display semantics (heap-bounded depth).
      if (scheme_has_cycle(val))
         return scheme_pretty_print(val);
      return scheme_render_structure(val, _render_display);
      }
   return scheme_pretty_print(val);
   }

// ── resolve output port helper ────────────────────────────────────────────────

static Value _resolve_output_port(Context* ctx, const std::vector<Value>& args, size_t idx)
   {
   if (args.size() > idx)
      return args[idx];
   return _get_current_output(ctx);
   }

// ── Predicates ────────────────────────────────────────────────────────────────

static Value _prim_port_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   return make_boolean(is_port(args[0]));
   }

static Value _prim_input_port_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   return make_boolean(is_port(args[0]) && as_port(args[0])->is_input);
   }

static Value _prim_output_port_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   return make_boolean(is_port(args[0]) && !as_port(args[0])->is_input);
   }

static Value _prim_textual_port_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   return make_boolean(is_port(args[0]) && as_port(args[0])->is_text);
   }

static Value _prim_binary_port_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   return make_boolean(is_port(args[0]) && !as_port(args[0])->is_text);
   }

static Value _prim_input_port_open_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   if (!is_port(args[0]))
      return make_boolean(false);
   Port* p = as_port(args[0]);
   return make_boolean(p->is_input && p->is_open);
   }

static Value _prim_output_port_open_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   if (!is_port(args[0]))
      return make_boolean(false);
   Port* p = as_port(args[0]);
   return make_boolean(!p->is_input && p->is_open);
   }

static Value _prim_port_open_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   if (!is_port(args[0]))
      return make_boolean(false);
   return make_boolean(as_port(args[0])->is_open);
   }

static Value _prim_eof_object_p(Context*, Environment*, std::vector<Value>& args, const Value*)
   {
   return make_boolean(is_eof(args[0]));
   }

static Value _prim_eof_object(Context*, Environment*, std::vector<Value>&, const Value*)
   {
   return make_eof();
   }

// ── Constructors / closers ────────────────────────────────────────────────────

static Value _prim_open_input_file(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_string(args[0]))
      throw SchemeTypeError("open-input-file: filename must be a string", _src(app));
   const std::string path = as_string(args[0]);
   std::FILE* fh = std::fopen(path.c_str(), "r");
   if (!fh)
      throw SchemeFileError("open-input-file: cannot open " + path, _src(app));
   // Slurp the entire file into the port buffer (matching Python's snapshot model).
   std::string buf;
   char tmp[4096];
   while (std::fgets(tmp, sizeof(tmp), fh))
      buf += tmp;
   Value v = make_port(true, true, path);
   Port* p = as_port(v);
   p->buf_text = std::move(buf);
   p->file_h = fh;
   return v;
   }

static Value _prim_open_output_file(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_string(args[0]))
      throw SchemeTypeError("open-output-file: filename must be a string", _src(app));
   const std::string path = as_string(args[0]);
   std::FILE* fh = std::fopen(path.c_str(), "w");
   if (!fh)
      throw SchemeFileError("open-output-file: cannot open " + path, _src(app));
   Value v = make_port(false, true, path);
   as_port(v)->file_h = fh;
   return v;
   }

static Value _prim_open_input_string(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_string(args[0]))
      throw SchemeTypeError("open-input-string: argument must be a string", _src(app));
   Value v = make_port(true, true, "<input-string>");
   as_port(v)->buf_text = as_string(args[0]);
   return v;
   }

static Value _prim_open_output_string(Context*, Environment*, std::vector<Value>&, const Value*)
   {
   return make_port(false, true, "<output-string>");
   }

static Value _prim_get_output_string(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   Port* p = _check_textual_output(args[0], "get-output-string", app);
   if (p->file_h)
      throw SchemeTypeError("get-output-string: port is not a string output port", _src(app));
   return make_string(p->buf_text);
   }

static Value _prim_open_input_bytevector(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_bytevector(args[0]))
      throw SchemeTypeError("open-input-bytevector: argument must be a bytevector", _src(app));
   const auto& src = as_bytevector_items_const(args[0]);
   Value v = make_port(true, false, "<input-bytevector>");
   as_port(v)->buf_binary = std::vector<uint8_t>(src.begin(), src.end());
   return v;
   }

static Value _prim_open_output_bytevector(Context*, Environment*, std::vector<Value>&, const Value*)
   {
   return make_port(false, false, "<output-bytevector>");
   }

static Value _prim_get_output_bytevector(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   Port* p = _check_binary_output(args[0], "get-output-bytevector", app);
   if (p->file_h)
      throw SchemeTypeError("get-output-bytevector: port is not a bytevector output port", _src(app));
   return make_bytevector(p->buf_binary);
   }

static Value _prim_open_binary_input_file(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_string(args[0]))
      throw SchemeTypeError("open-binary-input-file: filename must be a string", _src(app));
   const std::string path = as_string(args[0]);
   std::FILE* fh = std::fopen(path.c_str(), "rb");
   if (!fh)
      throw SchemeFileError("open-binary-input-file: cannot open " + path, _src(app));
   // Slurp content
   std::fseek(fh, 0, SEEK_END);
   long sz = std::ftell(fh);
   std::fseek(fh, 0, SEEK_SET);
   std::vector<uint8_t> data(sz > 0 ? static_cast<size_t>(sz) : 0);
   if (sz > 0)
      std::fread(data.data(), 1, static_cast<size_t>(sz), fh);
   Value v = make_port(true, false, path);
   Port* p = as_port(v);
   p->buf_binary = std::move(data);
   p->file_h = fh;
   return v;
   }

static Value _prim_open_binary_output_file(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_string(args[0]))
      throw SchemeTypeError("open-binary-output-file: filename must be a string", _src(app));
   const std::string path = as_string(args[0]);
   std::FILE* fh = std::fopen(path.c_str(), "wb");
   if (!fh)
      throw SchemeFileError("open-binary-output-file: cannot open " + path, _src(app));
   Value v = make_port(false, false, path);
   as_port(v)->file_h = fh;
   return v;
   }

static void _close_port_impl(Port* p)
   {
   if (p->is_open && p->file_h &&
       p->file_h != stdin && p->file_h != stdout && p->file_h != stderr)
      {
      std::fclose(p->file_h);
      }
   p->is_open = false;
   }

static Value _prim_close_port(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_port(args[0]))
      throw SchemeTypeError("close-port: argument must be a port", _src(app));
   _close_port_impl(as_port(args[0]));
   return VOID_VALUE;
   }

static Value _prim_close_input_port(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   // R7RS 6.13.1: close-input-port has no effect on an already-closed port,
   // so check the port TYPE (must be an input port) without requiring it to
   // be open, then delegate to the idempotent _close_port_impl.
   if (!is_port(args[0]))
      throw SchemeTypeError("close-input-port: argument must be a port", _src(app));
   if (!as_port(args[0])->is_input)
      throw SchemeTypeError("close-input-port: argument must be an input port", _src(app));
   _close_port_impl(as_port(args[0]));
   return VOID_VALUE;
   }

static Value _prim_close_output_port(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   // R7RS 6.13.1: close-output-port has no effect on an already-closed port.
   if (!is_port(args[0]))
      throw SchemeTypeError("close-output-port: argument must be a port", _src(app));
   if (as_port(args[0])->is_input)
      throw SchemeTypeError("close-output-port: argument must be an output port", _src(app));
   _close_port_impl(as_port(args[0]));
   return VOID_VALUE;
   }

// ── Read primitives ───────────────────────────────────────────────────────────

static Value _prim_read_char(Context* ctx, Environment*, std::vector<Value>& args, const Value* app)
   {
   Value port_val = args.empty() ? _get_current_input(ctx) : args[0];
   Port* p = _check_textual_input(port_val, "read-char", app);
   if (p->pos >= p->buf_text.size())
      return make_eof();
   char32_t c = _decode_utf8_char(p->buf_text, p->pos);
   return make_character(c);
   }

static Value _prim_peek_char(Context* ctx, Environment*, std::vector<Value>& args, const Value* app)
   {
   Value port_val = args.empty() ? _get_current_input(ctx) : args[0];
   Port* p = _check_textual_input(port_val, "peek-char", app);
   if (p->pos >= p->buf_text.size())
      return make_eof();
   size_t tmp = p->pos;
   char32_t c = _decode_utf8_char(p->buf_text, tmp);
   return make_character(c);
   }

static Value _prim_char_ready_p(Context* ctx, Environment*, std::vector<Value>& args, const Value* app)
   {
   Value port_val = args.empty() ? _get_current_input(ctx) : args[0];
   _check_textual_input(port_val, "char-ready?", app);
   return make_boolean(true); // buffered ports are always ready
   }

static Value _prim_read(Context* ctx, Environment*, std::vector<Value>& args, const Value* app)
   {
   Value port_val = args.empty() ? _get_current_input(ctx) : args[0];
   Port* p = _check_input_port(port_val, "read", app);
   // Use the text buffer for text ports; for binary, decode as UTF-8.
   const std::string& full = p->buf_text;
   if (p->pos >= full.size())
      return make_eof();
   // Check if remaining is all whitespace.
   bool has_nonws = false;
   for (size_t i = p->pos; i < full.size(); ++i)
      {
      unsigned char c = static_cast<unsigned char>(full[i]);
      if (c > ' ')
         {
         has_nonws = true;
         break;
         }
      }
   if (!has_nonws)
      return make_eof();
   std::string remaining = full.substr(p->pos);
   std::vector<Token> toks = scheme_tokenize(remaining, p->name);
   if (toks.empty() || toks[0].kind == TokenKind::END_OF_FILE)
      return make_eof();
   // Parse one datum and let the parser report where it ended, so the port is
   // advanced from the parser's own position rather than by re-walking nested
   // tokens (which recursed on nesting depth).  Mirrors pyScheme's read.
   bool at_eof = false;
   SourceInfo next_src{0, 0, "", ""};
   Value form = scheme_parse_first(remaining, p->name, at_eof, next_src);
   if (at_eof)
      {
      p->pos = full.size();
      }
   else
      {
      int target_line = next_src.line;
      int target_col = next_src.col;
      int lines_seen = 0;
      size_t i = 0;
      while (lines_seen < target_line - 1 && i < remaining.size())
         {
         if (remaining[i] == '\n')
            ++lines_seen;
         ++i;
         }
      i += static_cast<size_t>(target_col - 1);
      p->pos += i;
      }
   return form;
   }

static Value _prim_read_line(Context* ctx, Environment*, std::vector<Value>& args, const Value* app)
   {
   Value port_val = args.empty() ? _get_current_input(ctx) : args[0];
   Port* p = _check_textual_input(port_val, "read-line", app);
   if (p->pos >= p->buf_text.size())
      return make_eof();
   // R7RS 6.13.2: an end of line is a linefeed, a carriage return, or a
   // carriage return followed by a linefeed (CRLF counts as one ending).
   // CR/LF are ASCII and never occur as UTF-8 continuation bytes, so scanning
   // by byte is safe.
   size_t n = p->buf_text.size();
   size_t i = p->pos;
   while (i < n && p->buf_text[i] != '\n' && p->buf_text[i] != '\r')
      ++i;
   std::string line;
   if (i >= n)
      {
      line = p->buf_text.substr(p->pos);
      p->pos = n;
      }
   else
      {
      line = p->buf_text.substr(p->pos, i - p->pos);
      if (p->buf_text[i] == '\r' && i + 1 < n && p->buf_text[i + 1] == '\n')
         p->pos = i + 2; // CRLF
      else
         p->pos = i + 1; // lone LF or lone CR
      }
   return make_string(line);
   }

static Value _prim_read_string(Context* ctx, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_integer(args[0]))
      throw SchemeTypeError("read-string: count must be an integer", _src(app));
   int64_t k = as_integer(args[0]);
   if (k < 0)
      throw SchemeTypeError("read-string: count must be non-negative", _src(app));
   Value port_val = (args.size() >= 2) ? args[1] : _get_current_input(ctx);
   Port* p = _check_textual_input(port_val, "read-string", app, 2);
   if (p->pos >= p->buf_text.size() && k > 0)
      return make_eof();
   size_t end = std::min(p->pos + static_cast<size_t>(k), p->buf_text.size());
   std::string s = p->buf_text.substr(p->pos, end - p->pos);
   p->pos = end;
   return make_string(s);
   }

// ── Binary port read primitives ───────────────────────────────────────────────

static Value _prim_read_u8(Context* ctx, Environment*, std::vector<Value>& args, const Value* app)
   {
   Value port_val = args.empty() ? _get_current_input(ctx) : args[0];
   Port* p = _check_binary_input(port_val, "read-u8", app);
   if (p->pos >= p->buf_binary.size())
      return make_eof();
   return make_integer(static_cast<int64_t>(p->buf_binary[p->pos++]));
   }

static Value _prim_peek_u8(Context* ctx, Environment*, std::vector<Value>& args, const Value* app)
   {
   Value port_val = args.empty() ? _get_current_input(ctx) : args[0];
   Port* p = _check_binary_input(port_val, "peek-u8", app);
   if (p->pos >= p->buf_binary.size())
      return make_eof();
   return make_integer(static_cast<int64_t>(p->buf_binary[p->pos]));
   }

static Value _prim_u8_ready_p(Context* ctx, Environment*, std::vector<Value>& args, const Value* app)
   {
   Value port_val = args.empty() ? _get_current_input(ctx) : args[0];
   _check_binary_input(port_val, "u8-ready?", app);
   return make_boolean(true);
   }

static Value _prim_read_bytevector(Context* ctx, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_integer(args[0]))
      throw SchemeTypeError("read-bytevector: count must be an integer", _src(app));
   int64_t k = as_integer(args[0]);
   if (k < 0)
      throw SchemeTypeError("read-bytevector: count must be non-negative", _src(app));
   Value port_val = (args.size() >= 2) ? args[1] : _get_current_input(ctx);
   Port* p = _check_binary_input(port_val, "read-bytevector", app, 2);
   if (p->pos >= p->buf_binary.size() && k > 0)
      return make_eof();
   size_t end = std::min(p->pos + static_cast<size_t>(k), p->buf_binary.size());
   std::vector<uint8_t> chunk(p->buf_binary.begin() + static_cast<ptrdiff_t>(p->pos),
                              p->buf_binary.begin() + static_cast<ptrdiff_t>(end));
   p->pos = end;
   return make_bytevector(std::move(chunk));
   }

static Value _prim_read_bytevector_bang(Context* ctx, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_bytevector(args[0]))
      throw SchemeTypeError("read-bytevector!: first argument must be a bytevector", _src(app));
   auto& dst = as_bytevector_items(args[0]);
   Value port_val = (args.size() >= 2) ? args[1] : _get_current_input(ctx);
   Port* p = _check_binary_input(port_val, "read-bytevector!", app, 2);
   int64_t start = 0, end = static_cast<int64_t>(dst.size());
   if (args.size() >= 3)
      {
      if (!is_integer(args[2]))
         throw SchemeTypeError("read-bytevector!: start must be an integer", _src(app));
      start = as_integer(args[2]);
      }
   if (args.size() >= 4)
      {
      if (!is_integer(args[3]))
         throw SchemeTypeError("read-bytevector!: end must be an integer", _src(app));
      end = as_integer(args[3]);
      }
   if (start < 0 || end > static_cast<int64_t>(dst.size()) || start > end)
      throw SchemeTypeError("read-bytevector!: range out of bounds", _src(app));
   int64_t want = end - start;
   if (p->pos >= p->buf_binary.size() && want > 0)
      return make_eof();
   int64_t avail = std::min(want, static_cast<int64_t>(p->buf_binary.size() - p->pos));
   for (int64_t i = 0; i < avail; ++i)
      dst[static_cast<size_t>(start + i)] = p->buf_binary[p->pos + static_cast<size_t>(i)];
   p->pos += static_cast<size_t>(avail);
   return make_integer(avail);
   }

// ── Write primitives ──────────────────────────────────────────────────────────

static Value _prim_write_char(Context* ctx, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_character(args[0]))
      throw SchemeTypeError("write-char: first argument must be a character", _src(app));
   Value port_val = _resolve_output_port(ctx, args, 1);
   Port* p = _check_textual_output(port_val, "write-char", app, 2);
   _emit_to_port(ctx, p, _char32_to_utf8(as_character(args[0])));
   return VOID_VALUE;
   }

static Value _prim_write_string(Context* ctx, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_string(args[0]))
      throw SchemeTypeError("write-string: first argument must be a string", _src(app));
   const std::string& text = as_string(args[0]);
   Value port_val = _resolve_output_port(ctx, args, 1);
   Port* p = _check_textual_output(port_val, "write-string", app, 2);
   int64_t start = 0, end = static_cast<int64_t>(text.size());
   if (args.size() >= 3)
      {
      if (!is_integer(args[2]))
         throw SchemeTypeError("write-string: start must be an integer", _src(app));
      start = as_integer(args[2]);
      }
   if (args.size() >= 4)
      {
      if (!is_integer(args[3]))
         throw SchemeTypeError("write-string: end must be an integer", _src(app));
      end = as_integer(args[3]);
      }
   if (start < 0 || end > static_cast<int64_t>(text.size()) || start > end)
      throw SchemeTypeError("write-string: range out of bounds", _src(app));
   _emit_to_port(ctx, p, text.substr(static_cast<size_t>(start), static_cast<size_t>(end - start)));
   return VOID_VALUE;
   }

static Value _prim_write_u8(Context* ctx, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_integer(args[0]))
      throw SchemeTypeError("write-u8: byte must be an integer", _src(app));
   int64_t b = as_integer(args[0]);
   if (b < 0 || b > 255)
      throw SchemeTypeError("write-u8: byte out of u8 range", _src(app));
   Value port_val = (args.size() >= 2) ? args[1] : _get_current_output(ctx);
   Port* p = _check_binary_output(port_val, "write-u8", app, 2);
   uint8_t byte = static_cast<uint8_t>(b);
   _emit_bytes_to_port(p, &byte, 1);
   return VOID_VALUE;
   }

static Value _prim_write_bytevector(Context* ctx, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_bytevector(args[0]))
      throw SchemeTypeError("write-bytevector: first argument must be a bytevector", _src(app));
   const auto& src = as_bytevector_items_const(args[0]);
   Value port_val = (args.size() >= 2) ? args[1] : _get_current_output(ctx);
   Port* p = _check_binary_output(port_val, "write-bytevector", app, 2);
   int64_t start = 0, end = static_cast<int64_t>(src.size());
   if (args.size() >= 3)
      {
      if (!is_integer(args[2]))
         throw SchemeTypeError("write-bytevector: start must be an integer", _src(app));
      start = as_integer(args[2]);
      }
   if (args.size() >= 4)
      {
      if (!is_integer(args[3]))
         throw SchemeTypeError("write-bytevector: end must be an integer", _src(app));
      end = as_integer(args[3]);
      }
   if (start < 0 || end > static_cast<int64_t>(src.size()) || start > end)
      throw SchemeTypeError("write-bytevector: range out of bounds", _src(app));
   _emit_bytes_to_port(p, src.data() + start, static_cast<size_t>(end - start));
   return VOID_VALUE;
   }

static Value _prim_newline(Context* ctx, Environment*, std::vector<Value>& args, const Value* app)
   {
   Value port_val = _resolve_output_port(ctx, args, 0);
   Port* p = _check_output_port(port_val, "newline", app);
   _emit_to_port(ctx, p, "\n");
   return VOID_VALUE;
   }

static Value _prim_display(Context* ctx, Environment*, std::vector<Value>& args, const Value* app)
   {
   Value port_val = _resolve_output_port(ctx, args, 1);
   Port* p = _check_textual_output(port_val, "display", app, 2);
   _emit_to_port(ctx, p, _render_display(args[0]));
   return VOID_VALUE;
   }

static Value _prim_write(Context* ctx, Environment*, std::vector<Value>& args, const Value* app)
   {
   Value port_val = _resolve_output_port(ctx, args, 1);
   Port* p = _check_textual_output(port_val, "write", app, 2);
   _emit_to_port(ctx, p, scheme_pretty_print(args[0]));
   return VOID_VALUE;
   }

static Value _prim_write_shared(Context* ctx, Environment*, std::vector<Value>& args, const Value* app)
   {
   Value port_val = _resolve_output_port(ctx, args, 1);
   Port* p = _check_output_port(port_val, "write-shared", app, 2);
   _emit_to_port(ctx, p, scheme_pretty_print_shared(args[0]));
   return VOID_VALUE;
   }

static Value _prim_write_simple(Context* ctx, Environment*, std::vector<Value>& args, const Value* app)
   {
   // write-simple skips datum labels; same as write in this implementation.
   Value port_val = _resolve_output_port(ctx, args, 1);
   Port* p = _check_textual_output(port_val, "write-simple", app, 2);
   _emit_to_port(ctx, p, scheme_pretty_print(args[0]));
   return VOID_VALUE;
   }

static Value _prim_flush_output_port(Context* ctx, Environment*, std::vector<Value>& args, const Value* app)
   {
   Value port_val = _resolve_output_port(ctx, args, 0);
   Port* p = _check_output_port(port_val, "flush-output-port", app);
   if (p->file_h)
      std::fflush(p->file_h);
   return VOID_VALUE;
   }

// ── Current-port accessors ────────────────────────────────────────────────────

static Value _prim_current_input_port(Context* ctx, Environment*, std::vector<Value>&, const Value*)
   {
   return _get_current_input(ctx);
   }

static Value _prim_current_output_port(Context* ctx, Environment*, std::vector<Value>&, const Value*)
   {
   return _get_current_output(ctx);
   }

static Value _prim_current_error_port(Context* ctx, Environment*, std::vector<Value>&, const Value*)
   {
   return _get_current_error(ctx);
   }

static Value _prim_call_with_port(Context* ctx, Environment* env, std::vector<Value>& args, const Value* app)
   {
   Port* p = _check_port(args[0], "call-with-port", app);
   GcRootGuard port_val(args[0]);
   GcRootGuard proc(args[1]);
   Value result;
   try
      {
      std::vector<Value> call_args = {port_val.val};
      GcRootVec call_args_root(call_args);
      result = apply_scheme_proc(proc.val, call_args, ctx, env, app);
      }
   catch (...)
      {
      _close_port_impl(p);
      throw;
      }
   _close_port_impl(p);
   return result;
   }

// ── File I/O wrappers ─────────────────────────────────────────────────────────

static Value _prim_file_exists_p(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_string(args[0]))
      throw SchemeTypeError("file-exists?: argument must be a string", _src(app));
   return make_boolean(std::filesystem::exists(as_string(args[0])));
   }

static Value _prim_delete_file(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_string(args[0]))
      throw SchemeTypeError("delete-file: argument must be a string", _src(app));
   const std::string path = as_string(args[0]);
   std::error_code ec;
   bool removed = std::filesystem::remove(path, ec);
   if (ec)
      throw SchemeFileError("delete-file: " + ec.message(), _src(app));
   if (!removed)
      throw SchemeFileError("delete-file: file not found: " + path, _src(app));
   return VOID_VALUE;
   }

static Value _prim_rename_file(Context*, Environment*, std::vector<Value>& args, const Value* app)
   {
   if (!is_string(args[0]))
      throw SchemeTypeError("rename-file: first argument must be a string", _src(app));
   if (!is_string(args[1]))
      throw SchemeTypeError("rename-file: second argument must be a string", _src(app));
   std::error_code ec;
   std::filesystem::rename(as_string(args[0]), as_string(args[1]), ec);
   if (ec)
      throw SchemeFileError("rename-file: " + ec.message(), _src(app));
   return VOID_VALUE;
   }

static Value _prim_call_with_input_file(Context* ctx, Environment* env, std::vector<Value>& args, const Value* app)
   {
   if (!is_string(args[0]))
      throw SchemeTypeError("call-with-input-file: filename must be a string", _src(app));
   GcRootGuard proc(args[1]);
   std::vector<Value> open_args = {args[0]};
   GcRootGuard port_val(_prim_open_input_file(ctx, env, open_args, app));
   Port* p = as_port(port_val.val);
   Value result;
   try
      {
      std::vector<Value> call_args = {port_val.val};
      GcRootVec call_args_root(call_args);
      result = apply_scheme_proc(proc.val, call_args, ctx, env, app);
      }
   catch (...)
      {
      _close_port_impl(p);
      throw;
      }
   _close_port_impl(p);
   return result;
   }

static Value _prim_call_with_output_file(Context* ctx, Environment* env, std::vector<Value>& args, const Value* app)
   {
   if (!is_string(args[0]))
      throw SchemeTypeError("call-with-output-file: filename must be a string", _src(app));
   GcRootGuard proc(args[1]);
   std::vector<Value> open_args = {args[0]};
   GcRootGuard port_val(_prim_open_output_file(ctx, env, open_args, app));
   Port* p = as_port(port_val.val);
   Value result;
   try
      {
      std::vector<Value> call_args = {port_val.val};
      GcRootVec call_args_root(call_args);
      result = apply_scheme_proc(proc.val, call_args, ctx, env, app);
      }
   catch (...)
      {
      _close_port_impl(p);
      throw;
      }
   _close_port_impl(p);
   return result;
   }

static Value _prim_with_input_from_file(Context* ctx, Environment* env, std::vector<Value>& args, const Value* app)
   {
   if (!is_string(args[0]))
      throw SchemeTypeError("with-input-from-file: filename must be a string", _src(app));
   GcRootGuard thunk(args[1]);
   std::vector<Value> open_args = {args[0]};
   GcRootGuard port_val(_prim_open_input_file(ctx, env, open_args, app));
   _get_current_input(ctx); // ensure initialized
   GcRootGuard old_val(as_parameter_value(s_current_input_param));
   set_parameter_value(s_current_input_param, port_val.val);
   Value result;
   try
      {
      std::vector<Value> thunk_args;
      result = apply_scheme_proc(thunk.val, thunk_args, ctx, env, app);
      }
   catch (...)
      {
      set_parameter_value(s_current_input_param, old_val.val);
      _close_port_impl(as_port(port_val.val));
      throw;
      }
   set_parameter_value(s_current_input_param, old_val.val);
   _close_port_impl(as_port(port_val.val));
   return result;
   }

static Value _prim_with_output_to_file(Context* ctx, Environment* env, std::vector<Value>& args, const Value* app)
   {
   if (!is_string(args[0]))
      throw SchemeTypeError("with-output-to-file: filename must be a string", _src(app));
   GcRootGuard thunk(args[1]);
   std::vector<Value> open_args = {args[0]};
   GcRootGuard port_val(_prim_open_output_file(ctx, env, open_args, app));
   _get_current_output(ctx); // ensure initialized
   GcRootGuard old_val(as_parameter_value(s_current_output_param));
   set_parameter_value(s_current_output_param, port_val.val);
   Value result;
   try
      {
      std::vector<Value> thunk_args;
      result = apply_scheme_proc(thunk.val, thunk_args, ctx, env, app);
      }
   catch (...)
      {
      set_parameter_value(s_current_output_param, old_val.val);
      _close_port_impl(as_port(port_val.val));
      throw;
      }
   set_parameter_value(s_current_output_param, old_val.val);
   _close_port_impl(as_port(port_val.val));
   return result;
   }

// Common (non-R7RS) extension: like with-input-from-file but reads from a string.
static Value _prim_with_input_from_string(Context* ctx, Environment* env, std::vector<Value>& args, const Value* app)
   {
   if (!is_string(args[0]))
      throw SchemeTypeError("with-input-from-string: first argument must be a string", _src(app));
   GcRootGuard thunk(args[1]);
   std::vector<Value> open_args = {args[0]};
   GcRootGuard port_val(_prim_open_input_string(ctx, env, open_args, app));
   _get_current_input(ctx); // ensure initialized
   GcRootGuard old_val(as_parameter_value(s_current_input_param));
   set_parameter_value(s_current_input_param, port_val.val);
   Value result;
   try
      {
      std::vector<Value> thunk_args;
      result = apply_scheme_proc(thunk.val, thunk_args, ctx, env, app);
      }
   catch (...)
      {
      set_parameter_value(s_current_input_param, old_val.val);
      _close_port_impl(as_port(port_val.val));
      throw;
      }
   set_parameter_value(s_current_input_param, old_val.val);
   _close_port_impl(as_port(port_val.val));
   return result;
   }

// ── Registration ──────────────────────────────────────────────────────────────

void register_ports()
   {
   // Predicates
   register_primitive("port?", 1, 1, _prim_port_p,
                      "", "Return #t if obj is a port.  R7RS 6.13.", CATEGORY);
   register_primitive("input-port?", 1, 1, _prim_input_port_p,
                      "", "Return #t if obj is an input port.  R7RS 6.13.", CATEGORY);
   register_primitive("output-port?", 1, 1, _prim_output_port_p,
                      "", "Return #t if obj is an output port.  R7RS 6.13.", CATEGORY);
   register_primitive("textual-port?", 1, 1, _prim_textual_port_p,
                      "", "Return #t if obj is a textual port.  R7RS 6.13.", CATEGORY);
   register_primitive("binary-port?", 1, 1, _prim_binary_port_p,
                      "", "Return #t if obj is a binary port.  R7RS 6.13.", CATEGORY);
   register_primitive("input-port-open?", 1, 1, _prim_input_port_open_p,
                      "", "Return #t if obj is an open input port.  R7RS 6.13.", CATEGORY);
   register_primitive("output-port-open?", 1, 1, _prim_output_port_open_p,
                      "", "Return #t if obj is an open output port.  R7RS 6.13.", CATEGORY);
   register_primitive("port-open?", 1, 1, _prim_port_open_p,
                      "", "Return #t if port is open (input or output).  Extension.", CATEGORY);
   register_primitive("eof-object?", 1, 1, _prim_eof_object_p,
                      "", "Return #t if obj is the eof object.  R7RS 6.13.", CATEGORY);
   register_primitive("eof-object", 0, 0, _prim_eof_object,
                      "", "Return the eof object.  R7RS 6.13.", CATEGORY);
   // Constructors / closers
   register_primitive("open-input-file", 1, 1, _prim_open_input_file,
                      "", "(open-input-file path) opens the file for reading.  R7RS 6.13.", CATEGORY);
   register_primitive("open-output-file", 1, 1, _prim_open_output_file,
                      "", "(open-output-file path) opens a file for writing.  R7RS 6.13.", CATEGORY);
   register_primitive("open-input-string", 1, 1, _prim_open_input_string,
                      "", "(open-input-string string) returns an input port reading from string.  R7RS 6.13.", CATEGORY);
   register_primitive("open-output-string", 0, 0, _prim_open_output_string,
                      "", "Return a fresh output string port.  R7RS 6.13.", CATEGORY);
   register_primitive("get-output-string", 1, 1, _prim_get_output_string,
                      "", "(get-output-string port) returns the accumulated string.  R7RS 6.13.", CATEGORY);
   register_primitive("open-input-bytevector", 1, 1, _prim_open_input_bytevector,
                      "", "(open-input-bytevector bv) returns a binary input port over a copy of bv.  R7RS 6.13.", CATEGORY);
   register_primitive("open-output-bytevector", 0, 0, _prim_open_output_bytevector,
                      "", "Return a fresh binary output port.  R7RS 6.13.", CATEGORY);
   register_primitive("get-output-bytevector", 1, 1, _prim_get_output_bytevector,
                      "", "(get-output-bytevector port) returns the accumulated bytevector.  R7RS 6.13.", CATEGORY);
   register_primitive("open-binary-input-file", 1, 1, _prim_open_binary_input_file,
                      "", "(open-binary-input-file path) opens path in binary mode.  R7RS 6.13.", CATEGORY);
   register_primitive("open-binary-output-file", 1, 1, _prim_open_binary_output_file,
                      "", "(open-binary-output-file path) opens path for binary writing.  R7RS 6.13.", CATEGORY);
   register_primitive("close-port", 1, 1, _prim_close_port,
                      "", "Close any port (input or output).  R7RS 6.13.", CATEGORY);
   register_primitive("close-input-port", 1, 1, _prim_close_input_port,
                      "", "Close an input port.  R7RS 6.13.", CATEGORY);
   register_primitive("close-output-port", 1, 1, _prim_close_output_port,
                      "", "Close an output port.  R7RS 6.13.", CATEGORY);
   // Read primitives
   register_primitive("read-char", 0, 1, _prim_read_char,
                      "", "(read-char [port]) reads one character.  R7RS 6.13.", CATEGORY);
   register_primitive("peek-char", 0, 1, _prim_peek_char,
                      "", "(peek-char [port]) returns the next character without consuming it.  R7RS 6.13.", CATEGORY);
   register_primitive("read", 0, 1, _prim_read,
                      "", "(read [port]) reads one S-expression from the port.  R7RS 6.13.", CATEGORY);
   register_primitive("read-line", 0, 1, _prim_read_line,
                      "", "(read-line [port]) reads characters up to the next newline.  R7RS 6.13.", CATEGORY);
   register_primitive("read-string", 1, 2, _prim_read_string,
                      "", "(read-string k [port]) reads up to k characters.  R7RS 6.13.", CATEGORY);
   register_primitive("char-ready?", 0, 1, _prim_char_ready_p,
                      "", "(char-ready? [port]) returns #t when read-char would not block.  R7RS 6.13.", CATEGORY);
   register_primitive("read-u8", 0, 1, _prim_read_u8,
                      "", "(read-u8 [port]) reads one byte from a binary input port.  R7RS 6.13.", CATEGORY);
   register_primitive("peek-u8", 0, 1, _prim_peek_u8,
                      "", "(peek-u8 [port]) returns the next byte without consuming it.  R7RS 6.13.", CATEGORY);
   register_primitive("u8-ready?", 0, 1, _prim_u8_ready_p,
                      "", "(u8-ready? [port]) returns #t when read-u8 would not block.  R7RS 6.13.", CATEGORY);
   register_primitive("read-bytevector", 1, 2, _prim_read_bytevector,
                      "", "(read-bytevector k [port]) reads up to k bytes.  R7RS 6.13.", CATEGORY);
   register_primitive("read-bytevector!", 1, 4, _prim_read_bytevector_bang,
                      "", "(read-bytevector! bv [port [start [end]]]) reads bytes into bv.  R7RS 6.13.", CATEGORY);
   // Write primitives
   register_primitive("write-u8", 1, 2, _prim_write_u8,
                      "", "(write-u8 byte port) writes one byte to a binary output port.  R7RS 6.13.", CATEGORY);
   register_primitive("write-bytevector", 1, 4, _prim_write_bytevector,
                      "", "(write-bytevector bv port [start [end]]) writes bytes.  R7RS 6.13.", CATEGORY);
   register_primitive("write-char", 1, 2, _prim_write_char,
                      "", "(write-char char [port]) writes char to the output port.  R7RS 6.13.", CATEGORY);
   register_primitive("write-string", 1, 4, _prim_write_string,
                      "", "(write-string string [port [start [end]]]) writes string.  R7RS 6.13.", CATEGORY);
   register_primitive("newline", 0, 1, _prim_newline,
                      "", "(newline [port]) writes a newline to the output port.  R7RS 6.13.", CATEGORY);
   register_primitive("display", 1, 2, _prim_display,
                      "", "(display obj [port]) writes a human-readable representation.  R7RS 6.13.", CATEGORY);
   register_primitive("write", 1, 2, _prim_write,
                      "", "(write obj [port]) writes a machine-readable representation.  R7RS 6.13.", CATEGORY);
   register_primitive("write-shared", 1, 2, _prim_write_shared,
                      "", "(write-shared obj [port]) like write, with datum labels for shared structure.  R7RS 6.13.", CATEGORY);
   register_primitive("write-simple", 1, 2, _prim_write_simple,
                      "", "(write-simple obj [port]) like write, no datum labels.  R7RS 6.13.", CATEGORY);
   register_primitive("flush-output-port", 0, 1, _prim_flush_output_port,
                      "", "(flush-output-port [port]) flushes any buffered output.  R7RS 6.13.", CATEGORY);
   // Current-port accessors
   register_primitive("current-input-port", 0, 0, _prim_current_input_port,
                      "", "Return the current input port.  R7RS 6.13.", CATEGORY);
   register_primitive("current-output-port", 0, 0, _prim_current_output_port,
                      "", "Return the current output port.  R7RS 6.13.", CATEGORY);
   register_primitive("current-error-port", 0, 0, _prim_current_error_port,
                      "", "Return the current error port.  R7RS 6.13.", CATEGORY);
   register_primitive("call-with-port", 2, 2, _prim_call_with_port,
                      "", "(call-with-port port proc) calls proc, closes port, returns result.  R7RS 6.13.", CATEGORY);
   register_primitive("call-with-input-file", 2, 2, _prim_call_with_input_file,
                      "", "(call-with-input-file path proc) opens path, calls proc, closes.  R7RS 6.13.", CATEGORY);
   register_primitive("call-with-output-file", 2, 2, _prim_call_with_output_file,
                      "", "(call-with-output-file path proc) opens path, calls proc, closes.  R7RS 6.13.", CATEGORY);
   register_primitive("with-input-from-file", 2, 2, _prim_with_input_from_file,
                      "", "(with-input-from-file path thunk) binds current-input-port during thunk.  R7RS 6.13.", CATEGORY);
   register_primitive("with-output-to-file", 2, 2, _prim_with_output_to_file,
                      "", "(with-output-to-file path thunk) binds current-output-port during thunk.  R7RS 6.13.", CATEGORY);
   register_primitive("with-input-from-string", 2, 2, _prim_with_input_from_string,
                      "", "(with-input-from-string str thunk) binds current-input-port to a string port during thunk.  Common extension (not R7RS).", CATEGORY);
   register_primitive("file-exists?", 1, 1, _prim_file_exists_p,
                      "", "(file-exists? path) returns #t if path names an existing file.  R7RS 6.14.", CATEGORY);
   register_primitive("delete-file", 1, 1, _prim_delete_file,
                      "", "(delete-file path) deletes the named file.  R7RS 6.14.", CATEGORY);
   register_primitive("rename-file", 2, 2, _prim_rename_file,
                      "", "(rename-file old new) renames old to new.  R7RS 6.14.", CATEGORY);
   }
