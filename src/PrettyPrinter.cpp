// PrettyPrinter.cpp -- Scheme value renderer.
// Direct port of pyscheme/PrettyPrinter.py.
#include "PrettyPrinter.h"
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ── _format_float ─────────────────────────────────────────────────────────────
// Port of PrettyPrinter.py _format_float.
// Python uses repr(f): shortest-round-trip decimal, decimal notation for
// |f| in [1e-4, 1e16), scientific notation otherwise.

static std::string format_float(double f)
   {
   if (std::isnan(f))
      return "+nan.0";
   if (f == std::numeric_limits<double>::infinity())
      return "+inf.0";
   if (f == -std::numeric_limits<double>::infinity())
      return "-inf.0";

   // Find the shortest precision (1..17) that round-trips back to f.
   // This mirrors Python's repr shortest-decimal algorithm.
   char buf[64];
   std::string best;
   for (int prec = 1; prec <= 17; ++prec)
      {
      int n = std::snprintf(buf, sizeof(buf), "%.*g", prec, f);
      if (n <= 0)
         continue;
      double check = 0.0;
      auto res = std::from_chars(buf, buf + n, check);
      if (res.ec == std::errc{} && check == f)
         {
         best = std::string(buf, n);
         break;
         }
      }
   if (best.empty())
      {
      int n = std::snprintf(buf, sizeof(buf), "%.17g", f);
      best = std::string(buf, n);
      }

   // If shortest form is scientific (contains 'e'), check whether Python
   // would prefer decimal notation (|f| in [1e-4, 1e16)).
   double af = (f < 0.0) ? -f : f;
   if (best.find('e') != std::string::npos && af >= 1e-4 && af < 1e16)
      {
      // Re-format as decimal: snprintf %.17g gives decimal for this range.
      int n = std::snprintf(buf, sizeof(buf), "%.17g", f);
      std::string d(buf, n);
      // Trim trailing zeros after decimal point.
      auto dp = d.find('.');
      if (dp != std::string::npos)
         {
         size_t last = d.size();
         while (last > dp + 2 && d[last - 1] == '0')
            --last;
         d.resize(last);
         }
      else
         {
         d += ".0";
         }
      best = d;
      }

   // Ensure result has '.' or 'e' (Scheme requires float notation).
   if (best.find('.') == std::string::npos && best.find('e') == std::string::npos)
      best += ".0";
   return best;
   }

// ── _CHAR_NAMES_REVERSE ───────────────────────────────────────────────────────
// Port of PrettyPrinter.py _CHAR_NAMES_REVERSE.

static const std::unordered_map<char32_t, std::string> CHAR_NAMES_REVERSE = {
    {U' ', "space"},
    {U'\n', "newline"},
    {U'\t', "tab"},
    {U'\r', "return"},
    {U'\0', "null"},
    {U'\a', "alarm"},
    {U'\b', "backspace"},
    {U'\x7f', "delete"},
    {U'\x1b', "escape"},
};

// ── _escape_string ────────────────────────────────────────────────────────────
// Port of PrettyPrinter.py _escape_string.

static std::string escape_string(const std::string& s)
   {
   std::string r;
   r.reserve(s.size());
   for (char c : s)
      {
      if (c == '\n')
         r += "\\n";
      else if (c == '\t')
         r += "\\t";
      else if (c == '\r')
         r += "\\r";
      else if (c == '\\')
         r += "\\\\";
      else if (c == '"')
         r += "\\\"";
      else
         r += c;
      }
   return r;
   }

// ── _GENSYM_PFX / _display_symbol_name ───────────────────────────────────────
// Port of PrettyPrinter.py _GENSYM_PFX and _display_symbol_name.

static const std::string GENSYM_PFX = "\x01h.";

static std::string display_symbol_name(const std::string& name)
   {
   if (name.rfind(GENSYM_PFX, 0) != 0)
      return name;
   std::string rest = name.substr(GENSYM_PFX.size());
   auto dot = rest.rfind('.');
   if (dot != std::string::npos)
      {
      const std::string tail = rest.substr(dot + 1);
      bool all_digits = !tail.empty();
      for (char c : tail)
         if (c < '0' || c > '9')
            {
            all_digits = false;
            break;
            }
      if (all_digits)
         return rest.substr(0, dot);
      }
   return rest;
   }

// ── _is_safe_symbol_initial / _subsequent / _needs_vertical_bars ─────────────
// Port of PrettyPrinter.py symbol-quoting helpers.

static bool is_safe_symbol_initial(char c)
   {
   return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          c == '!' || c == '$' || c == '%' || c == '&' || c == '*' ||
          c == '/' || c == ':' || c == '<' || c == '=' || c == '>' ||
          c == '?' || c == '^' || c == '_' || c == '~';
   }

static bool is_safe_symbol_subsequent(char c)
   {
   return is_safe_symbol_initial(c) || (c >= '0' && c <= '9') ||
          c == '+' || c == '-' || c == '@' || c == '.';
   }

static bool needs_vertical_bars(const std::string& name)
   {
   if (name.empty())
      return true;
   char first = name[0];
   if (!is_safe_symbol_initial(first) &&
       first != '+' && first != '-' && first != '@' && first != '.')
      return true;
   for (char c : name)
      if (!is_safe_symbol_subsequent(c))
         return true;
   return false;
   }

static std::string escape_symbol_name(const std::string& name)
   {
   std::string r;
   r.reserve(name.size());
   for (char c : name)
      {
      if (c == '|')
         r += "\\|";
      else if (c == '\\')
         r += "\\\\";
      else
         r += c;
      }
   return r;
   }

// ── join helper ───────────────────────────────────────────────────────────────

static std::string join(const std::vector<std::string>& parts)
   {
   std::string r;
   for (size_t i = 0; i < parts.size(); ++i)
      {
      if (i > 0)
         r += ' ';
      r += parts[i];
      }
   return r;
   }

// ── scheme_render_structure ───────────────────────────────────────────────────
// Port of PrettyPrinter.py _render_structure.  Iterative ordered renderer: an
// explicit task stack of emit-literal / render-value entries replaces recursion
// on the car-chain and vector nesting (the cdr-spine is already a loop), so depth
// is heap-bounded.  No Scheme objects are allocated while rendering, so no GC can
// fire and the Values held in the task stack need no rooting.

std::string scheme_render_structure(
    const Value& val, const std::function<std::string(const Value&)>& render_leaf)
   {
   std::string out;
   struct RTask { bool emit; std::string text; Value val; };
   std::vector<RTask> stack;
   stack.push_back({false, std::string(), val});
   while (!stack.empty())
      {
      RTask t = std::move(stack.back());
      stack.pop_back();
      if (t.emit)
         {
         out += t.text;
         continue;
         }
      const Value& x = t.val;
      if (is_cons(x))
         {
         std::vector<Value> elems;
         Value cur = x;
         while (is_cons(cur))
            {
            elems.push_back(car(cur));
            cur = cdr(cur);
            }
         // push in reverse so tasks pop in print order: ( e0 e1 ... [. tail] )
         stack.push_back({true, ")", NIL_VALUE});
         if (!is_nil(cur))
            {
            stack.push_back({false, std::string(), cur});
            stack.push_back({true, " . ", NIL_VALUE});
            }
         for (size_t i = elems.size(); i-- > 0; )
            {
            stack.push_back({false, std::string(), elems[i]});
            if (i > 0)
               stack.push_back({true, " ", NIL_VALUE});
            }
         stack.push_back({true, "(", NIL_VALUE});
         }
      else if (is_vector(x))
         {
         const std::vector<Value>& items = as_vector_items_const(x);
         stack.push_back({true, ")", NIL_VALUE});
         for (size_t i = items.size(); i-- > 0; )
            {
            stack.push_back({false, std::string(), items[i]});
            if (i > 0)
               stack.push_back({true, " ", NIL_VALUE});
            }
         stack.push_back({true, "#(", NIL_VALUE});
         }
      else
         {
         out += render_leaf(x);
         }
      }
   return out;
   }

// scheme_pretty_print_shared is declared in PrettyPrinter.h; defined below.

// ── has_cycle ─────────────────────────────────────────────────────────────────
// Port of PrettyPrinter.py _has_cycle.  Iterative (no recursion-limit risk).
// Returns true if val contains any reference cycle in its cons/vector graph.

bool scheme_has_cycle(const Value& root)
   {
   std::unordered_set<uintptr_t> gray;
   std::vector<std::pair<Value, bool>> stack;
   stack.push_back({root, false});
   while (!stack.empty())
      {
      auto [v, exiting] = stack.back();
      stack.pop_back();
      if (!is_cons(v) && !is_vector(v))
         continue;
      auto key = reinterpret_cast<uintptr_t>(gc_value_header(v));
      if (exiting)
         {
         gray.erase(key);
         continue;
         }
      if (!gray.insert(key).second)
         return true;
      stack.push_back({v, true});
      if (is_cons(v))
         {
         stack.push_back({car(v), false});
         stack.push_back({cdr(v), false});
         }
      else
         {
         for (const Value& item : as_vector_items_const(v))
            stack.push_back({item, false});
         }
      }
   return false;
   }

// ── scheme_pretty_print ───────────────────────────────────────────────────────
// Port of PrettyPrinter.py pretty_print.  Order of cases matches Python.

std::string scheme_pretty_print(const Value& val)
   {
   if (is_cons(val) || is_vector(val))
      {
      if (scheme_has_cycle(val))
         return scheme_pretty_print_shared(val);
      return scheme_render_structure(val, scheme_pretty_print);
      }
   if (is_nil(val))
      return "()";
   if (is_void(val))
      return "#<void>";
   if (is_integer(val))
      return std::to_string(as_integer(val));
   if (is_bignum(val))
      return bignum_to_string(val, 10);
   if (is_real(val))
      return format_float(as_real(val));
   if (is_rational(val))
      return std::to_string(as_rational_num(val)) + '/' +
             std::to_string(as_rational_den(val));
   if (is_complex(val))
      {
      double re = as_complex_real(val);
      double im = as_complex_imag(val);
      std::string re_s = format_float(re);
      std::string im_s = format_float(im);
      if (std::isnan(im) || im >= 0.0)
         return re_s + '+' + im_s + 'i';
      return re_s + im_s + 'i';
      }
   if (is_exact_complex(val))
      {
      Value re_v = as_exact_complex_real(val);
      Value im_v = as_exact_complex_imag(val);
      std::string re_s = scheme_pretty_print(re_v);
      std::string im_s = scheme_pretty_print(im_v);
      int64_t im_py = is_integer(im_v) ? as_integer(im_v) : as_rational_num(im_v);
      if (im_py >= 0)
         return re_s + '+' + im_s + 'i';
      return re_s + im_s + 'i';
      }
   if (is_boolean(val))
      return as_boolean(val) ? "#t" : "#f";
   if (is_character(val))
      {
      char32_t c = as_character(val);
      auto it = CHAR_NAMES_REVERSE.find(c);
      if (it != CHAR_NAMES_REVERSE.end())
         return "#\\" + it->second;
      if (c >= 0x20 && c <= 0x7E)
         return std::string("#\\") + static_cast<char>(c);
      // Non-printable-ASCII and non-ASCII: hex escape.
      char buf[16];
      std::snprintf(buf, sizeof(buf), "#\\x%X", static_cast<unsigned>(c));
      return std::string(buf);
      }
   if (is_string(val))
      return '"' + escape_string(as_string(val)) + '"';
   if (is_closure(val))
      return "#<procedure>";
   if (is_case_closure(val))
      return "#<procedure>";
   if (is_record_accessor(val) || is_record_mutator(val))
      return "#<procedure>";
   if (is_primitive(val))
      return "#<primitive " + as_primitive_name(val) + '>';
   if (is_promise(val))
      return as_promise_is_done(val) ? "#<promise forced>" : "#<promise>";
   if (is_multi_values(val))
      {
      const std::vector<Value>& vs = as_multi_values_list(val);
      if (vs.empty())
         return "";
      std::vector<std::string> parts;
      for (const Value& v : vs)
         parts.push_back(scheme_pretty_print(v));
      return "#<values " + join(parts) + '>';
      }
   if (is_record(val))
      {
      RecordType* rt = as_record_type(val);
      const std::vector<Value>& fs = as_record_fields_const(val);
      const std::string& tname = as_record_type_name(Value{Value::Repr(rt)});
      if (fs.empty())
         return "#<" + tname + '>';
      std::vector<std::string> parts;
      for (const Value& f : fs)
         parts.push_back(scheme_pretty_print(f));
      return "#<" + tname + ' ' + join(parts) + '>';
      }
   if (is_record_type(val))
      return "#<record-type " + as_record_type_name(val) + '>';
   if (is_parameter(val))
      return "#<parameter>";
   if (is_continuation(val))
      return "#<continuation>";
   if (is_environment(val))
      return "#<environment>";
   if (is_bytevector(val))
      {
      const std::vector<uint8_t>& items = as_bytevector_items_const(val);
      std::vector<std::string> parts;
      for (uint8_t b : items)
         parts.push_back(std::to_string(b));
      return "#u8(" + join(parts) + ')';
      }
   if (is_port(val))
      return "#<port>";
   if (is_eof(val))
      return "#<eof>";
   if (is_syntax_transformer(val))
      return "#<syntax-rules " + as_syntax_transformer_name(val) + '>';
   if (is_error_object(val))
      {
      const std::string& msg = as_error_object_message(val);
      const std::vector<Value>& irs = as_error_object_irritants(val);
      if (irs.empty())
         return "#<error-object \"" + escape_string(msg) + "\">";
      std::vector<std::string> parts;
      for (const Value& ir : irs)
         parts.push_back(scheme_pretty_print(ir));
      return "#<error-object \"" + escape_string(msg) + "\" (" + join(parts) + ")>";
      }
   if (is_symbol(val))
      {
      std::string name = display_symbol_name(as_symbol(val));
      if (needs_vertical_bars(name))
         return '|' + escape_symbol_name(name) + '|';
      return name;
      }
   return "#<unknown>";
   }

// ── _shared_scan / _shared_render / pretty_print_shared ──────────────────────
// Port of PrettyPrinter.py _shared_scan, _shared_render, pretty_print_shared.
// Identity of heap objects is derived from their GcHeader address (gc_value_header).

using CountMap = std::unordered_map<uintptr_t, int>;
using LabelMap = std::unordered_map<uintptr_t, int>;
using SeenSet = std::unordered_set<uintptr_t>;

static void shared_scan(const Value& root, CountMap& counts)
   {
   // Iterative (explicit stack) so deep/long structures don't overflow the C
   // stack.  Label numbers are assigned by iterating `counts` (hash order), so
   // traversal order doesn't affect output; we push cdr-then-car for parity.
   std::vector<Value> stack;
   stack.push_back(root);
   while (!stack.empty())
      {
      Value v = stack.back();
      stack.pop_back();
      if (is_cons(v))
         {
         auto key = reinterpret_cast<uintptr_t>(gc_value_header(v));
         auto it = counts.find(key);
         if (it != counts.end())
            {
            ++it->second;
            continue;
            }
         counts[key] = 1;
         stack.push_back(cdr(v));
         stack.push_back(car(v));
         }
      else if (is_vector(v))
         {
         auto key = reinterpret_cast<uintptr_t>(gc_value_header(v));
         auto it = counts.find(key);
         if (it != counts.end())
            {
            ++it->second;
            continue;
            }
         counts[key] = 1;
         const std::vector<Value>& items = as_vector_items_const(v);
         for (size_t i = items.size(); i-- > 0; )
            stack.push_back(items[i]);
         }
      }
   }

static std::string shared_render(const Value& root, const LabelMap& labels,
                                 SeenSet& seen)
   {
   // Iterative analogue of the recursive renderer: an explicit task stack of
   // emit-literal / render-value entries.  A labeled node's "#n=" prefix is
   // emitted and its id added to `seen` when first popped -- BEFORE its children
   // are pushed -- so a back-reference within the children renders as "#n#",
   // matching the recursion.  Heap-bounded depth; no Scheme allocation, so the
   // Values held in the stack need no GC rooting.
   std::string out;
   struct RTask { bool emit; std::string text; Value val; };
   std::vector<RTask> stack;
   stack.push_back({false, std::string(), root});
   while (!stack.empty())
      {
      RTask t = std::move(stack.back());
      stack.pop_back();
      if (t.emit)
         {
         out += t.text;
         continue;
         }
      const Value& x = t.val;
      if (is_cons(x))
         {
         uintptr_t k = reinterpret_cast<uintptr_t>(gc_value_header(x));
         auto lit = labels.find(k);
         if (lit != labels.end())
            {
            int n = lit->second;
            if (seen.count(k))
               {
               out += '#' + std::to_string(n) + '#';
               continue;
               }
            seen.insert(k);
            out += '#' + std::to_string(n) + '=';
            }
         std::vector<Value> items;
         Value cur = x;
         Value tail = NIL_VALUE;
         bool found_tail = false;
         while (is_cons(cur))
            {
            uintptr_t ck = reinterpret_cast<uintptr_t>(gc_value_header(cur));
            if (ck != k && labels.count(ck))
               {
               tail = cur;
               found_tail = true;
               break;
               }
            items.push_back(car(cur));
            cur = cdr(cur);
            if (is_cons(cur) &&
                seen.count(reinterpret_cast<uintptr_t>(gc_value_header(cur))))
               {
               tail = cur;
               found_tail = true;
               break;
               }
            }
         if (!found_tail && !is_nil(cur))
            {
            tail = cur;
            found_tail = true;
            }
         stack.push_back({true, ")", NIL_VALUE});
         if (found_tail)
            {
            stack.push_back({false, std::string(), tail});
            stack.push_back({true, " . ", NIL_VALUE});
            }
         for (size_t i = items.size(); i-- > 0; )
            {
            stack.push_back({false, std::string(), items[i]});
            if (i > 0)
               stack.push_back({true, " ", NIL_VALUE});
            }
         stack.push_back({true, "(", NIL_VALUE});
         }
      else if (is_vector(x))
         {
         uintptr_t k = reinterpret_cast<uintptr_t>(gc_value_header(x));
         auto lit = labels.find(k);
         if (lit != labels.end())
            {
            int n = lit->second;
            if (seen.count(k))
               {
               out += '#' + std::to_string(n) + '#';
               continue;
               }
            seen.insert(k);
            out += '#' + std::to_string(n) + '=';
            }
         const std::vector<Value>& items = as_vector_items_const(x);
         stack.push_back({true, ")", NIL_VALUE});
         for (size_t i = items.size(); i-- > 0; )
            {
            stack.push_back({false, std::string(), items[i]});
            if (i > 0)
               stack.push_back({true, " ", NIL_VALUE});
            }
         stack.push_back({true, "#(", NIL_VALUE});
         }
      else
         {
         out += scheme_pretty_print(x);
         }
      }
   return out;
   }

std::string scheme_pretty_print_shared(const Value& val)
   {
   CountMap counts;
   shared_scan(val, counts);
   LabelMap labels;
   int next_label = 0;
   for (auto& [key, count] : counts)
      if (count > 1)
         labels[key] = next_label++;
   SeenSet seen;
   return shared_render(val, labels, seen);
   }
