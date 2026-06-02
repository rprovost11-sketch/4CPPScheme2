// Debugger.cpp -- interactive step debugger, breakpoints, watches.
// Direct port of pyscheme/Debugger.py.
#include "Debugger.h"
#include "Analyzer.h"
#include "Context.h"
#include "Environment.h"
#include "Expander.h"
#include "Parser.h"
#include "PrettyPrinter.h"
#include "primitives/debug.h"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#ifdef _WIN32
#include "readline_win.h"
#include <io.h>
static bool _is_tty()
   {
   return _isatty(1) != 0;
   }
#else
#include <unistd.h>
static bool _is_tty()
   {
   return isatty(1) != 0;
   }
#endif

// ── ANSI color helpers ────────────────────────────────────────────────────────

static const bool s_ansi = _is_tty();
static const char* _BOLD = s_ansi ? "\033[1m" : "";
static const char* _DIM = s_ansi ? "\033[2m" : "";
static const char* _RESET = s_ansi ? "\033[0m" : "";

// ── Default break-on set ──────────────────────────────────────────────────────

static const std::unordered_set<std::string> _DEFAULT_BREAK_ON =
    {"error", "raise", "raise-continuable"};

// ── Static helpers ────────────────────────────────────────────────────────────

std::string Debugger::_cmd_word(const std::string& line)
   {
   size_t i = line.find_first_not_of(" \t");
   if (i == std::string::npos)
      return "";
   size_t j = line.find_first_of(" \t", i);
   return line.substr(i, j == std::string::npos ? std::string::npos : j - i);
   }

std::string Debugger::_cmd_rest(const std::string& line)
   {
   size_t i = line.find_first_not_of(" \t");
   if (i == std::string::npos)
      return "";
   size_t j = line.find_first_of(" \t", i);
   if (j == std::string::npos)
      return "";
   size_t k = line.find_first_not_of(" \t", j);
   if (k == std::string::npos)
      return "";
   return line.substr(k);
   }

bool Debugger::_is_callable(const Value& val)
   {
   return is_closure(val) || is_primitive(val) ||
          is_case_closure(val) || is_continuation(val);
   }

bool Debugger::_value_identity_equal(const Value& a, const Value& b)
   {
   if (a.repr.index() != b.repr.index())
      return false;
   if (is_closure(a))
      return std::get<SchemeClosure*>(a.repr) == std::get<SchemeClosure*>(b.repr);
   if (is_case_closure(a))
      return std::get<CaseClosure*>(a.repr) == std::get<CaseClosure*>(b.repr);
   if (is_cons(a))
      return std::get<ConsCell*>(a.repr) == std::get<ConsCell*>(b.repr);
   return false;
   }

void Debugger::_collect_locals(Environment* env,
                               std::unordered_map<uint32_t, Value>& result)
   {
   Environment* current = env;
   Environment* global_env = env->_global_env;
   while (current != nullptr && current != global_env)
      {
      for (auto& [sid, val] : current->_bindings)
         {
         if (result.find(sid) == result.end())
            result[sid] = val;
         }
      current = current->_parent;
      }
   }

void Debugger::_print_scoped_locals(Environment* env, int max_depth)
   {
   Environment* current = env;
   Environment* global_env = env->_global_env;
   int scope_num = 0;
   while (current != nullptr && current != global_env)
      {
      if (max_depth >= 0 && scope_num >= max_depth)
         break;
      std::vector<uint32_t> names_in_scope;
      for (auto& [sid, val] : current->_bindings)
         {
         if (!_is_callable(val))
            names_in_scope.push_back(sid);
         }
      if (!names_in_scope.empty())
         {
         std::sort(names_in_scope.begin(), names_in_scope.end(),
                   [](uint32_t a, uint32_t b)
                   { return symbol_name(a) < symbol_name(b); });
         std::cout << "--- scope " << scope_num << " ---\n";
         for (uint32_t sid : names_in_scope)
            {
            std::cout << symbol_name(sid) << ":   "
                      << scheme_pretty_print(current->_bindings.at(sid)) << '\n';
            }
         scope_num++;
         }
      current = current->_parent;
      }
   }

void Debugger::_print_named_vars(Environment* env,
                                 const std::vector<std::string>& names)
   {
   for (const auto& name : names)
      {
      auto val = env->lookup_optional(name);
      if (val)
         std::cout << name << ":   " << scheme_pretty_print(*val) << '\n';
      else
         std::cout << name << ":   <unbound>\n";
      }
   }

std::vector<std::pair<std::string, std::string>>
Debugger::_parse_bp_args(const std::string& rest)
   {
   std::vector<std::pair<std::string, std::string>> pairs;
   size_t i = 0, n = rest.size();
   while (i < n)
      {
      while (i < n && rest[i] == ' ')
         i++;
      if (i >= n)
         break;
      size_t j = i;
      while (j < n && rest[j] != ' ' && rest[j] != '(')
         j++;
      std::string name = rest.substr(i, j - i);
      i = j;
      while (i < n && rest[i] == ' ')
         i++;
      std::string cond;
      if (i < n && rest[i] == '(')
         {
         int depth = 0;
         j = i;
         while (j < n)
            {
            if (rest[j] == '(')
               depth++;
            else if (rest[j] == ')')
               {
               depth--;
               if (depth == 0)
                  {
                  j++;
                  break;
                  }
               }
            j++;
            }
         cond = rest.substr(i, j - i);
         i = j;
         }
      // Append unconditionally (matching Python `pairs.append([name, cond])`);
      // an empty name from a token like "(foo)" is preserved, as in pyScheme.
      pairs.push_back({name, cond});
      }
   return pairs;
   }

std::optional<Value> Debugger::_walk_form(const Value& cell,
                                          const std::string& callable_name,
                                          int& count, int target_n)
   {
   if (!is_cons(cell))
      return std::nullopt;
   if (is_symbol(car(cell)) && as_symbol(car(cell)) == callable_name)
      {
      count++;
      if (count == target_n)
         return cell;
      }
   Value arg = cdr(cell);
   while (is_cons(arg))
      {
      auto result = _walk_form(car(arg), callable_name, count, target_n);
      if (result)
         return result;
      arg = cdr(arg);
      }
   return std::nullopt;
   }

std::optional<Value> Debugger::_find_nth_call(const Value& body_cons,
                                              const std::string& callable_name,
                                              int target_n)
   {
   int count = 0;
   Value forms = body_cons;
   while (is_cons(forms))
      {
      auto result = _walk_form(car(forms), callable_name, count, target_n);
      if (result)
         return result;
      forms = cdr(forms);
      }
   return std::nullopt;
   }

void Debugger::_collect_one(const Value& cell, const std::string& callable_name,
                            std::vector<Value>& results)
   {
   if (!is_cons(cell))
      return;
   if (is_symbol(car(cell)) && as_symbol(car(cell)) == callable_name)
      results.push_back(cell);
   Value arg = cdr(cell);
   while (is_cons(arg))
      {
      _collect_one(car(arg), callable_name, results);
      arg = cdr(arg);
      }
   }

std::vector<Value> Debugger::_collect_calls(const Value& body_cons,
                                            const std::string& callable_name)
   {
   std::vector<Value> results;
   Value forms = body_cons;
   while (is_cons(forms))
      {
      _collect_one(car(forms), callable_name, results);
      forms = cdr(forms);
      }
   return results;
   }

bool Debugger::_ann_has_any(const Value& cell, const AnnMap& annotations)
   {
   if (!is_cons(cell))
      return false;
   auto* ptr = std::get<ConsCell*>(cell.repr);
   if (annotations.count(ptr))
      return true;
   if (_ann_has_any(car(cell), annotations))
      return true;
   return _ann_has_any(cdr(cell), annotations);
   }

std::string Debugger::_ann_flat(const Value& cell, const AnnMap& annotations)
   {
   std::string tag;
   if (is_cons(cell))
      {
      auto it = annotations.find(std::get<ConsCell*>(cell.repr));
      if (it != annotations.end())
         tag = it->second;
      }
   if (!is_cons(cell))
      return tag + scheme_pretty_print(cell);
   if (is_nil(cell))
      return tag + "()";
   std::vector<std::string> parts;
   Value cur = cell;
   while (is_cons(cur))
      {
      parts.push_back(_ann_flat(car(cur), annotations));
      cur = cdr(cur);
      }
   std::string inner;
   for (size_t k = 0; k < parts.size(); k++)
      {
      if (k > 0)
         inner += ' ';
      inner += parts[k];
      }
   if (is_nil(cur))
      return tag + '(' + inner + ')';
   return tag + '(' + inner + " . " + _ann_flat(cur, annotations) + ')';
   }

std::string Debugger::_ann_fmt(const Value& cell, const AnnMap& annotations, int ind)
   {
   std::string tag;
   if (is_cons(cell))
      {
      auto it = annotations.find(std::get<ConsCell*>(cell.repr));
      if (it != annotations.end())
         tag = it->second;
      }
   if (!is_cons(cell))
      return tag + scheme_pretty_print(cell);
   if (is_nil(cell))
      return tag + "()";
   std::string flat = _ann_flat(cell, annotations);
   bool has_ann = false;
   Value cur = cdr(cell);
   while (is_cons(cur))
      {
      if (_ann_has_any(car(cur), annotations))
         {
         has_ann = true;
         break;
         }
      cur = cdr(cur);
      }
   if ((int)flat.size() + ind <= 50 && !has_ann)
      return flat;
   std::string head_str = _ann_flat(car(cell), annotations);
   int child_ind = ind + 2;
   std::string pad(child_ind, ' ');
   std::vector<std::string> lines;
   lines.push_back(tag + '(' + head_str);
   cur = cdr(cell);
   while (is_cons(cur))
      {
      lines.push_back(pad + _ann_fmt(car(cur), annotations, child_ind));
      cur = cdr(cur);
      }
   if (!is_nil(cur))
      lines.push_back(pad + ". " + _ann_fmt(cur, annotations, child_ind));
   lines.back() += ')';
   std::string result;
   for (size_t k = 0; k < lines.size(); k++)
      {
      if (k > 0)
         result += '\n';
      result += lines[k];
      }
   return result;
   }

void Debugger::_print_annotated(const Value& form, const AnnMap& annotations,
                                int indent)
   {
   std::cout << _ann_fmt(form, annotations, indent) << '\n';
   }

std::vector<std::pair<std::string, std::string>>
Debugger::_parse_help_entries(const std::string& docstring)
   {
   std::vector<std::pair<std::string, std::string>> entries;
   std::istringstream ss(docstring);
   std::string line;
   while (std::getline(ss, line))
      {
      size_t start = line.find_first_not_of(" \t");
      if (start == std::string::npos)
         continue;
      line = line.substr(start);
      size_t end = line.find_last_not_of(" \t\r\n");
      if (end != std::string::npos)
         line = line.substr(0, end + 1);
      if (line.empty())
         continue;
      // split on first run of 2+ spaces
      size_t sep = std::string::npos;
      for (size_t k = 0; k + 1 < line.size(); k++)
         {
         if (line[k] == ' ' && line[k + 1] == ' ')
            {
            sep = k;
            break;
            }
         }
      if (sep == std::string::npos)
         continue;
      std::string usage = line.substr(0, sep);
      size_t desc_start = line.find_first_not_of(" \t", sep);
      std::string desc = (desc_start != std::string::npos) ? line.substr(desc_start) : "";
      entries.push_back({usage, desc});
      }
   return entries;
   }

void Debugger::_print_help_entries(
    const std::vector<std::pair<std::string, std::string>>& entries)
   {
   if (entries.empty())
      return;
   size_t max_w = 0;
   for (auto& [usage, desc] : entries)
      if (usage.size() > max_w)
         max_w = usage.size();
   for (auto& [usage, desc] : entries)
      {
      std::string padded = usage;
      while (padded.size() < max_w)
         padded += ' ';
      std::cout << "  " << padded << "  " << desc << '\n';
      }
   }

std::vector<std::string> Debugger::_parse_watch_args(const std::string& source)
   {
   try
      {
      auto forms = scheme_parse(source, "<watch>");
      std::vector<std::string> result;
      for (auto& f : forms)
         result.push_back(scheme_pretty_print(f));
      return result;
      }
   catch (...)
      {
      return {source};
      }
   }

// ── History swapping ──────────────────────────────────────────────────────────

void Debugger::_swap_history_in()
   {
   if (!_has_readline)
      return;
#ifdef _WIN32
   _saved_history = readline_win_get_history();
   readline_win_set_history(_history);
#endif
   }

void Debugger::_swap_history_out()
   {
   if (!_has_readline)
      return;
#ifdef _WIN32
   _history = readline_win_get_history();
   readline_win_set_history(_saved_history);
#endif
   }

// ── Breakpoint resolution ─────────────────────────────────────────────────────

std::string Debugger::_resolve_bp_ref(const std::string& ref)
   {
   bool all_digits = !ref.empty();
   for (char c : ref)
      if (!isdigit((unsigned char)c))
         {
         all_digits = false;
         break;
         }
   if (all_digits)
      {
      int idx = std::stoi(ref);
      if (idx < 1 || idx > (int)_bp_index.size())
         {
         std::cout << "No breakpoint #" << idx << ".  Use b to list breakpoints.\n";
         return "";
         }
      return _bp_index[idx - 1];
      }
   if (breakpoints.count(ref))
      return ref;
   for (auto& [ptr, entry] : _inner_targets)
      if (entry.display == ref)
         return ref;
   std::cout << "No breakpoint on " << ref << ".\n";
   return "";
   }

// ── Internal helpers ──────────────────────────────────────────────────────────

void Debugger::_prune_stale_inner(Environment* env)
   {
   if (!env || _inner_targets.empty())
      return;
   std::vector<std::pair<const ConsCell*, std::string>> stale;
   for (auto& [ptr, entry] : _inner_targets)
      {
      auto current = env->lookup_optional(entry.fn_name);
      bool same = current && _value_identity_equal(*current, entry.fn_obj);
      if (!same)
         stale.push_back({ptr, entry.display});
      }
   for (auto& [ptr, display] : stale)
      {
      _inner_targets.erase(ptr);
      _disabled.erase(display);
      std::cout << "  (stale breakpoint " << display << " removed)\n";
      }
   }

void Debugger::_set_inner_breakpoint(const std::string& spec, const std::string& cond,
                                     Environment* env)
   {
   auto parts = [&]() -> std::vector<std::string>
   {
      std::vector<std::string> v;
      std::string cur;
      for (char c : spec)
         {
         if (c == ':')
            {
            v.push_back(cur);
            cur.clear();
            }
         else
            cur += c;
         }
      v.push_back(cur);
      return v;
   }();

   std::string fn_name, call_name;
   std::string index_str;
   int idx = 1; // validated numeric index (for the 2-part and 3-part numeric cases)
   if (parts.size() == 2)
      {
      fn_name = parts[0];
      call_name = parts[1];
      index_str = "1";
      idx = 1;
      }
   else if (parts.size() == 3)
      {
      fn_name = parts[0];
      call_name = parts[1];
      index_str = parts[2];
      // Validate the numeric index HERE (before the env/lookup/closure checks),
      // matching Python; '?' and '*' are selection modes handled later.
      if (index_str != "?" && index_str != "*")
         {
         try
            {
            idx = std::stoi(index_str);
            }
         catch (...)
            {
            std::cout << "Invalid index: " << index_str << '\n';
            return;
            }
         if (idx < 1)
            {
            std::cout << "Index must be >= 1.\n";
            return;
            }
         }
      }
   else
      {
      std::cout << "Invalid breakpoint spec: " << spec << '\n';
      return;
      }

   if (!env)
      {
      std::cout << "Cannot resolve inner breakpoint without an environment.\n";
      return;
      }

   auto fn_opt = env->lookup_optional(fn_name);
   if (!fn_opt)
      {
      std::cout << fn_name << " is not defined.\n";
      return;
      }
   if (!is_closure(*fn_opt))
      {
      std::cout << fn_name << " is not a user-defined procedure.\n";
      return;
      }
   Value fn = *fn_opt;
   Value body = as_closure_body(fn);

   // Interactive selection mode.
   if (index_str == "?")
      {
      auto calls = _collect_calls(body, call_name);
      if (calls.empty())
         {
         std::cout << "No calls to " << call_name << " in " << fn_name << ".\n";
         return;
         }
      AnnMap annotations;
      for (size_t k = 0; k < calls.size(); k++)
         annotations[std::get<ConsCell*>(calls[k].repr)] = "[" + std::to_string(k + 1) + "]";
      std::cout << "Body of " << fn_name << " - calls to " << call_name << " numbered:\n";
      Value forms = body;
      while (is_cons(forms))
         {
         _print_annotated(car(forms), annotations, 2);
         forms = cdr(forms);
         }
      std::string choice;
      try
         {
         choice = input_fn("Breakpoint at #? ", "");
         size_t s = choice.find_first_not_of(" \t\r\n");
         if (s != std::string::npos)
            choice = choice.substr(s);
         size_t e = choice.find_last_not_of(" \t\r\n");
         if (e != std::string::npos)
            choice = choice.substr(0, e + 1);
         }
      catch (...)
         {
         std::cout << "\nSet breakpoint cancelled.\n";
         return;
         }
      if (choice.empty())
         {
         std::cout << "Set breakpoint cancelled.\n";
         return;
         }
      std::vector<int> indices;
      std::istringstream css(choice);
      std::string tok;
      while (css >> tok)
         {
         try
            {
            int n = std::stoi(tok);
            if (n < 1 || n > (int)calls.size())
               {
               std::cout << "Selection out of range: " << n
                         << " (1-" << calls.size() << ").\n";
               return;
               }
            indices.push_back(n);
            }
         catch (...)
            {
            std::cout << "Invalid selection: " << tok << '\n';
            return;
            }
         }
      for (int n : indices)
         {
         Value target = calls[n - 1];
         std::string display = fn_name + ':' + call_name + ':' + std::to_string(n);
         _inner_targets[std::get<ConsCell*>(target.repr)] = {display, cond, target, fn_name, fn};
         _disabled.erase(display);
         if (cond.empty())
            std::cout << "Breakpoint set on " << display << ".\n";
         else
            std::cout << "Breakpoint set on " << display << " :when " << cond << '\n';
         }
      return;
      }

   // Set-all mode.
   if (index_str == "*")
      {
      auto calls = _collect_calls(body, call_name);
      if (calls.empty())
         {
         std::cout << "No calls to " << call_name << " in " << fn_name << ".\n";
         return;
         }
      for (size_t k = 0; k < calls.size(); k++)
         {
         Value target = calls[k];
         std::string display = fn_name + ':' + call_name + ':' + std::to_string(k + 1);
         _inner_targets[std::get<ConsCell*>(target.repr)] = {display, cond, target, fn_name, fn};
         _disabled.erase(display);
         if (cond.empty())
            std::cout << "Breakpoint set on " << display << ".\n";
         else
            std::cout << "Breakpoint set on " << display << " :when " << cond << '\n';
         }
      return;
      }

   // Single-index mode.  idx was validated in the spec-parsing block above.
   auto target_opt = _find_nth_call(body, call_name, idx);
   if (!target_opt)
      {
      std::cout << "No call to " << call_name << " at index " << idx
                << " in " << fn_name << ".\n";
      return;
      }
   Value target = *target_opt;
   std::string display = fn_name + ':' + call_name + ':' + std::to_string(idx);
   _inner_targets[std::get<ConsCell*>(target.repr)] = {display, cond, target, fn_name, fn};
   _disabled.erase(display);
   if (cond.empty())
      std::cout << "Breakpoint set on " << display << ".\n";
   else
      std::cout << "Breakpoint set on " << display << " :when " << cond << '\n';
   }

// ── Command dispatch ──────────────────────────────────────────────────────────

bool Debugger::_dispatch(const std::string& line, Context* ctx, Environment* env)
   {
   std::string word = _cmd_word(line);
   std::string rest = _cmd_rest(line);
   auto it = _commands.find(word);
   if (it == _commands.end())
      return false;
   it->second(word, rest, ctx, env);
   return true;
   }

// ── Command handlers ──────────────────────────────────────────────────────────

void Debugger::_cmd_abort(const std::string&, const std::string&,
                          Context*, Environment*)
   {
   // handled directly in _prompt_loop
   }

void Debugger::_cmd_b(const std::string& cmd, const std::string& rest,
                      Context* ctx, Environment* env)
   {
   if (cmd == "b!")
      {
      if (rest.empty())
         {
         std::cout << "Usage: b! <name-or-number> ...\n";
         return;
         }
      std::istringstream ss(rest);
      std::string ref;
      while (ss >> ref)
         {
         std::string name = _resolve_bp_ref(ref);
         if (name.empty())
            continue;
         if (_disabled.count(name))
            {
            _disabled.erase(name);
            std::cout << "Breakpoint on " << name << " enabled.\n";
            }
         else
            {
            _disabled.insert(name);
            std::cout << "Breakpoint on " << name << " disabled.\n";
            }
         }
      return;
      }

   if (cmd == "b")
      {
      if (rest.empty() || rest == "*")
         {
         _prune_stale_inner(env);
         _bp_index.clear();
         bool has_any = false;
         int idx = 1;
         std::vector<std::string> bp_names;
         for (auto& [n, c] : breakpoints)
            bp_names.push_back(n);
         std::sort(bp_names.begin(), bp_names.end());
         for (const auto& name : bp_names)
            {
            has_any = true;
            const auto& cond = breakpoints.at(name);
            _bp_index.push_back(name);
            bool disabled = _disabled.count(name) > 0;
            std::string tag = cond.empty() ? "" : "  :when " + cond;
            if (disabled)
               std::cout << "  " << idx << ": " << _DIM << name << tag << "  [disabled]" << _RESET << '\n';
            else
               std::cout << "  " << idx << ": " << _BOLD << name << _RESET << tag << '\n';
            idx++;
            }
         std::vector<std::pair<std::string, const ConsCell*>> inner_pairs;
         for (auto& [ptr, entry] : _inner_targets)
            inner_pairs.push_back({entry.display, ptr});
         std::sort(inner_pairs.begin(), inner_pairs.end(),
                   [](auto& a, auto& b)
                   { return a.first < b.first; });
         for (auto& [display, ptr] : inner_pairs)
            {
            has_any = true;
            const auto& entry = _inner_targets.at(ptr);
            _bp_index.push_back(display);
            bool disabled = _disabled.count(display) > 0;
            std::string tag = entry.cond.empty() ? "" : "  :when " + entry.cond;
            if (disabled)
               std::cout << "  " << idx << ": " << _DIM << display << tag << "  [disabled]" << _RESET << '\n';
            else
               std::cout << "  " << idx << ": " << _BOLD << display << _RESET << tag << '\n';
            idx++;
            }
         if (!break_on.empty())
            {
            has_any = true;
            std::vector<std::string> bo(break_on.begin(), break_on.end());
            std::sort(bo.begin(), bo.end());
            for (const auto& n : bo)
               std::cout << "  " << n << "  [break-on]\n";
            }
         if (!has_any)
            std::cout << "No breakpoints set.\n";
         return;
         }
      auto pairs = _parse_bp_args(rest);
      for (auto& [name, cond] : pairs)
         {
         if (name.find(':') != std::string::npos)
            {
            _set_inner_breakpoint(name, cond, env);
            }
         else
            {
            breakpoints[name] = cond;
            _disabled.erase(name);
            if (cond.empty())
               std::cout << "Breakpoint set on " << name << ".\n";
            else
               std::cout << "Breakpoint set on " << name << " :when " << cond << '\n';
            }
         }
      return;
      }

   // cmd == "b-"
   if (rest.empty())
      {
      std::cout << "Usage: b- <name-or-number> ... or b- *\n";
      return;
      }
   if (rest == "*")
      {
      breakpoints.clear();
      _inner_targets.clear();
      _disabled.clear();
      _bp_index.clear();
      std::cout << "All breakpoints cleared.\n";
      return;
      }
   std::istringstream ss(rest);
   std::string ref;
   while (ss >> ref)
      {
      if (ref.find(':') != std::string::npos && ref.size() >= 2 &&
          ref[ref.size() - 1] == '*' && ref[ref.size() - 2] == ':')
         {
         // wildcard: fn:* or fn:call:*
         std::vector<std::string> pparts;
         std::string cur;
         for (char c : ref)
            {
            if (c == ':')
               {
               pparts.push_back(cur);
               cur.clear();
               }
            else
               cur += c;
            }
         pparts.push_back(cur); // last part is "*"
         int count = 0;
         if (pparts.size() == 2)
            {
            // fn:* — remove all inner in fn
            for (auto it = _inner_targets.begin(); it != _inner_targets.end();)
               {
               if (it->second.fn_name == pparts[0])
                  {
                  _disabled.erase(it->second.display);
                  it = _inner_targets.erase(it);
                  count++;
                  }
               else
                  ++it;
               }
            }
         else if (pparts.size() == 3)
            {
            // fn:call:* — remove all breakpoints on that call in fn
            std::string prefix = pparts[0] + ':' + pparts[1] + ':';
            for (auto it = _inner_targets.begin(); it != _inner_targets.end();)
               {
               if (it->second.fn_name == pparts[0] &&
                   it->second.display.substr(0, prefix.size()) == prefix)
                  {
                  _disabled.erase(it->second.display);
                  it = _inner_targets.erase(it);
                  count++;
                  }
               else
                  ++it;
               }
            }
         else
            {
            std::cout << "Invalid spec: " << ref << '\n';
            continue;
            }
         if (count)
            std::cout << count << " breakpoint" << (count > 1 ? "s" : "") << " removed.\n";
         else
            std::cout << "No matching breakpoints.\n";
         continue;
         }
      std::string name = _resolve_bp_ref(ref);
      if (name.empty())
         continue;
      if (breakpoints.count(name))
         {
         breakpoints.erase(name);
         _disabled.erase(name);
         std::cout << "Breakpoint on " << name << " removed.\n";
         }
      else
         {
         bool removed = false;
         for (auto it = _inner_targets.begin(); it != _inner_targets.end(); ++it)
            {
            if (it->second.display == name)
               {
               _disabled.erase(name);
               _inner_targets.erase(it);
               std::cout << "Breakpoint on " << name << " removed.\n";
               removed = true;
               break;
               }
            }
         if (!removed)
            std::cout << "No breakpoint on " << name << ".\n";
         }
      }
   }

void Debugger::_cmd_bc(const std::string& cmd, const std::string& rest,
                       Context*, Environment*)
   {
   if (rest.empty())
      {
      std::cout << "Usage: bc <name-or-#> (cond) ... | bc- <name-or-#> ...\n";
      return;
      }
   if (cmd == "bc-")
      {
      std::istringstream ss(rest);
      std::string ref;
      while (ss >> ref)
         {
         std::string name = _resolve_bp_ref(ref);
         if (name.empty())
            continue;
         if (breakpoints.count(name))
            {
            breakpoints[name] = "";
            std::cout << "Condition cleared on " << name << ".\n";
            }
         else
            {
            bool found = false;
            for (auto& [ptr, entry] : _inner_targets)
               {
               if (entry.display == name)
                  {
                  entry.cond = "";
                  std::cout << "Condition cleared on " << name << ".\n";
                  found = true;
                  break;
                  }
               }
            if (!found)
               std::cout << "No breakpoint on " << name << ".\n";
            }
         }
      return;
      }
   // bc: set/change condition(s)
   auto pairs = _parse_bp_args(rest);
   for (auto& [ref, cond] : pairs)
      {
      if (cond.empty())
         {
         std::cout << "Missing condition for " << ref << ".\n";
         continue;
         }
      std::string name = _resolve_bp_ref(ref);
      if (name.empty())
         continue;
      if (breakpoints.count(name))
         {
         breakpoints[name] = cond;
         std::cout << "Condition set on " << name << " :when " << cond << '\n';
         }
      else
         {
         bool found = false;
         for (auto& [ptr, entry] : _inner_targets)
            {
            if (entry.display == name)
               {
               entry.cond = cond;
               std::cout << "Condition set on " << name << " :when " << cond << '\n';
               found = true;
               break;
               }
            }
         if (!found)
            std::cout << "No breakpoint on " << name << ".\n";
         }
      }
   }

void Debugger::_cmd_bo(const std::string& cmd, const std::string& rest,
                       Context*, Environment*)
   {
   if (cmd == "bo+")
      {
      break_on = std::unordered_set<std::string>(_DEFAULT_BREAK_ON);
      std::vector<std::string> bo(break_on.begin(), break_on.end());
      std::sort(bo.begin(), bo.end());
      std::string joined;
      for (size_t k = 0; k < bo.size(); k++)
         {
         if (k)
            joined += ", ";
         joined += bo[k];
         }
      std::cout << "Break-on restored: " << joined << '\n';
      }
   else if (cmd == "bo-")
      {
      if (rest.empty())
         {
         std::cout << "Usage: bo- <name> or bo- *\n";
         return;
         }
      if (rest == "*")
         {
         break_on.clear();
         std::cout << "Break-on list cleared.\n";
         return;
         }
      std::istringstream ss(rest);
      std::string n;
      while (ss >> n)
         break_on.erase(n);
      if (!break_on.empty())
         {
         std::vector<std::string> bo(break_on.begin(), break_on.end());
         std::sort(bo.begin(), bo.end());
         std::string joined;
         for (size_t k = 0; k < bo.size(); k++)
            {
            if (k)
               joined += ", ";
            joined += bo[k];
            }
         std::cout << "Break-on: " << joined << '\n';
         }
      else
         {
         std::cout << "Break-on list cleared.\n";
         }
      }
   else if (rest.empty() || rest == "*")
      {
      if (!break_on.empty())
         {
         std::vector<std::string> bo(break_on.begin(), break_on.end());
         std::sort(bo.begin(), bo.end());
         std::string joined;
         for (size_t k = 0; k < bo.size(); k++)
            {
            if (k)
               joined += ", ";
            joined += bo[k];
            }
         std::cout << "Break-on: " << joined << '\n';
         }
      else
         {
         std::cout << "Break-on list is empty.\n";
         }
      }
   else
      {
      std::istringstream ss(rest);
      std::string n;
      while (ss >> n)
         break_on.insert(n);
      std::vector<std::string> bo(break_on.begin(), break_on.end());
      std::sort(bo.begin(), bo.end());
      std::string joined;
      for (size_t k = 0; k < bo.size(); k++)
         {
         if (k)
            joined += ", ";
         joined += bo[k];
         }
      std::cout << "Break-on: " << joined << '\n';
      }
   }

void Debugger::_cmd_bt(const std::string&, const std::string&,
                       Context* ctx, Environment*)
   {
   const auto& stack = ctx->shadow_stack;
   if (stack.empty())
      {
      std::cout << "No backtrace available.\n";
      return;
      }
   for (size_t i = 0; i < stack.size(); i++)
      {
      std::string label = stack[i].label;
      if (stack[i].count > 1)
         label += " [x" + std::to_string(stack[i].count) + "]";
      std::cout << "  " << i << ": " << format_with_caret(label, stack[i].src) << '\n';
      }
   }

void Debugger::_cmd_body(const std::string&, const std::string& rest,
                         Context*, Environment* env)
   {
   if (rest.empty())
      {
      std::cout << "Usage: body <fn>\n";
      return;
      }
   std::string fn_name = rest;
      {
      size_t s = fn_name.find_first_not_of(" \t");
      if (s != std::string::npos)
         fn_name = fn_name.substr(s);
      size_t e = fn_name.find_last_not_of(" \t");
      if (e != std::string::npos)
         fn_name = fn_name.substr(0, e + 1);
      }
   auto fn_opt = env->lookup_optional(fn_name);
   if (!fn_opt)
      {
      std::cout << fn_name << " is not defined.\n";
      return;
      }
   if (!is_closure(*fn_opt))
      {
      std::cout << fn_name << " is not a user-defined procedure.\n";
      return;
      }
   Value fn = *fn_opt;
   const auto& params = as_closure_params(fn);
   uint32_t rest_name = as_closure_rest_name(fn);
   Value body = as_closure_body(fn);

   std::string params_str;
   if (rest_name != UINT32_MAX && params.empty())
      {
      params_str = symbol_name(rest_name);
      }
   else if (rest_name != UINT32_MAX)
      {
      params_str = "(";
      for (size_t k = 0; k < params.size(); k++)
         {
         if (k)
            params_str += ' ';
         params_str += symbol_name(params[k]);
         }
      params_str += " . " + symbol_name(rest_name) + ")";
      }
   else if (!params.empty())
      {
      params_str = "(";
      for (size_t k = 0; k < params.size(); k++)
         {
         if (k)
            params_str += ' ';
         params_str += symbol_name(params[k]);
         }
      params_str += ")";
      }
   else
      {
      params_str = "()";
      }
   std::cout << "Body of " << fn_name << " " << params_str << ":\n";
   AnnMap empty_ann;
   Value forms = body;
   while (is_cons(forms))
      {
      std::cout << "  " << _ann_fmt(car(forms), empty_ann, 2) << '\n';
      forms = cdr(forms);
      }
   }

void Debugger::_cmd_c(const std::string&, const std::string&, Context*, Environment*)
   {
   // handled directly in _prompt_loop
   }

void Debugger::_cmd_e(const std::string&, const std::string& rest,
                      Context* ctx, Environment* env)
   {
   if (!rest.empty())
      safe_eval(ctx, env, rest);
   }

void Debugger::_cmd_h(const std::string&, const std::string& rest,
                      Context*, Environment*)
   {
   if (!rest.empty())
      {
      std::string base = rest;
      size_t s = base.find_first_not_of(" \t");
      if (s != std::string::npos)
         base = base.substr(s);
      size_t e = base.find_last_not_of(" \t");
      if (e != std::string::npos)
         base = base.substr(0, e + 1);
      // Gate on _commands membership (Python looks up self._commands, which
      // deliberately EXCLUDES the flow-control commands s/n/o/c/abort/q), then
      // use its help text from _cmd_help.
      auto cmd_it = _commands.find(base);
      auto it = _cmd_help.find(base);
      if (cmd_it != _commands.end() && it != _cmd_help.end())
         {
         auto entries = _parse_help_entries(it->second);
         if (!entries.empty())
            _print_help_entries(entries);
         else
            std::cout << it->second << '\n';
         }
      else
         {
         std::cout << "No help for \"" << rest << "\".\n";
         }
      return;
      }
   std::vector<std::pair<std::string, std::string>> all_entries;
   for (const auto& base : _help_order)
      {
      auto it = _cmd_help.find(base);
      if (it == _cmd_help.end())
         continue;
      auto these = _parse_help_entries(it->second);
      for (auto& e2 : these)
         all_entries.push_back(e2);
      }
   _print_help_entries(all_entries);
   }

void Debugger::_cmd_i(const std::string&, const std::string& rest,
                      Context* ctx, Environment* env)
   {
   if (!rest.empty())
      safe_inspect(ctx, env, rest);
   }

void Debugger::_cmd_n(const std::string&, const std::string&, Context*, Environment*)
   {
   // handled directly in _prompt_loop
   }

void Debugger::_cmd_o(const std::string&, const std::string&, Context*, Environment*)
   {
   // handled directly in _prompt_loop
   }

void Debugger::_cmd_q(const std::string&, const std::string&, Context*, Environment*)
   {
   // handled directly in _prompt_loop
   }

void Debugger::_cmd_rd(const std::string&, const std::string& rest,
                       Context* ctx, Environment* env)
   {
   if (ctx->_debugging && !_last_rd_expr.empty())
      throw _RestartRd{};
   std::string expr;
   if (rest.empty())
      {
      if (_last_rd_expr.empty())
         {
         std::cout << "No previous rd expression.\n";
         return;
         }
      expr = _last_rd_expr;
      }
   else
      {
      expr = rest;
      _last_rd_expr = expr;
      }
   ctx->_debugging = true;
   ctx->_update_instrumented();
   try
      {
      while (true)
         {
         try
            {
            debug_eval(ctx, env, expr);
            break;
            }
         catch (_RestartRd&)
            {
            std::cout << "Restarting: " << expr << '\n';
            }
         }
      }
   catch (...)
      {
      ctx->_debugging = false;
      ctx->_update_instrumented();
      _step_hook = std::nullopt;
      _current_K = nullptr;
      throw;
      }
   ctx->_debugging = false;
   ctx->_update_instrumented();
   _step_hook = std::nullopt;
   _current_K = nullptr;
   }

void Debugger::_cmd_s(const std::string&, const std::string&, Context*, Environment*)
   {
   // handled directly in _prompt_loop
   }

void Debugger::_cmd_v(const std::string&, const std::string& rest,
                      Context*, Environment* env)
   {
   if (rest.empty())
      {
      _print_scoped_locals(env);
      }
   else
      {
      bool all_digits = true;
      for (char c : rest)
         if (!isdigit((unsigned char)c))
            {
            all_digits = false;
            break;
            }
      if (all_digits && !rest.empty())
         {
         _print_scoped_locals(env, std::stoi(rest));
         }
      else
         {
         std::vector<std::string> names;
         std::istringstream ss(rest);
         std::string n;
         while (ss >> n)
            names.push_back(n);
         _print_named_vars(env, names);
         }
      }
   }

void Debugger::_cmd_w(const std::string& cmd, const std::string& rest,
                      Context* ctx, Environment* env)
   {
   if (cmd == "w-")
      {
      if (rest.empty())
         {
         std::cout << "Usage: w- <expr-or-number> or w- *\n";
         return;
         }
      if (rest == "*")
         {
         watch_list.clear();
         std::cout << "Watch list cleared.\n";
         return;
         }
      bool all_digits = true;
      for (char c : rest)
         if (!isdigit((unsigned char)c))
            {
            all_digits = false;
            break;
            }
      if (all_digits && !rest.empty())
         {
         int idx = std::stoi(rest);
         if (idx < 1 || idx > (int)watch_list.size())
            {
            std::cout << "No watch #" << idx << ".  Use w to list watches.\n";
            }
         else
            {
            std::string removed = watch_list[idx - 1];
            watch_list.erase(watch_list.begin() + (idx - 1));
            std::cout << "Removed watch: " << removed << '\n';
            }
         }
      else
         {
         for (auto& canonical : _parse_watch_args(rest))
            {
            auto it = std::find(watch_list.begin(), watch_list.end(), canonical);
            if (it != watch_list.end())
               watch_list.erase(it);
            else
               std::cout << canonical << " not in watch list.\n";
            }
         if (!watch_list.empty())
            {
            for (size_t k = 0; k < watch_list.size(); k++)
               std::cout << "  " << (k + 1) << ": " << watch_list[k] << '\n';
            }
         else
            {
            std::cout << "Watch list cleared.\n";
            }
         }
      return;
      }
   // cmd == "w"
   if (rest.empty() || rest == "*")
      {
      if (!watch_list.empty())
         {
         for (size_t k = 0; k < watch_list.size(); k++)
            std::cout << "  " << (k + 1) << ": " << watch_list[k] << '\n';
         }
      else
         {
         std::cout << "Watch list is empty.\n";
         }
      }
   else
      {
      for (auto& canonical : _parse_watch_args(rest))
         {
         if (std::find(watch_list.begin(), watch_list.end(), canonical) == watch_list.end())
            watch_list.push_back(canonical);
         }
      std::string joined;
      for (size_t k = 0; k < watch_list.size(); k++)
         {
         if (k)
            joined += ", ";
         joined += watch_list[k];
         }
      std::cout << "Watching: " << joined << '\n';
      }
   }

// ── Eval helpers ──────────────────────────────────────────────────────────────

void Debugger::print_watch(Environment* env, Context* ctx)
   {
   if (watch_list.empty())
      return;
   auto saved_hook = _step_hook;
   _step_hook = std::nullopt;
   std::cout << "  [watch]\n";
   for (const auto& entry : watch_list)
      {
      try
         {
         auto forms = scheme_parse(entry, "<watch>");
         Value result = NIL_VALUE;
         for (auto& form : forms)
            {
            Value expanded = expand(form);
            analyze(expanded);
            if (ctx->lEval)
               result = ctx->lEval(env, expanded);
            }
         std::cout << "    " << entry << " = " << scheme_pretty_print(result) << '\n';
         }
      catch (const std::exception& ex)
         {
         std::cout << "    " << entry << " = %%% " << ex.what() << '\n';
         }
      }
   _step_hook = saved_hook;
   }

// Port of Python `str(ex)` for debugger error display: positioned Scheme errors
// render with their source position + caret (PositionedSchemeError::str), other
// exceptions render their plain message.
static std::string _debug_err_str(const std::exception& ex)
   {
   if (auto* p = dynamic_cast<const PositionedSchemeError*>(&ex))
      return p->str();
   return ex.what();
   }

void Debugger::safe_eval(Context* ctx, Environment* env, const std::string& source)
   {
   auto saved_hook = _step_hook;
   _step_hook = std::nullopt;
   try
      {
      auto forms = scheme_parse(source, "<debug>");
      Value result = NIL_VALUE;
      for (auto& form : forms)
         {
         Value expanded = expand(form);
         analyze(expanded);
         if (ctx->lEval)
            result = ctx->lEval(env, expanded);
         }
      std::cout << "==> " << scheme_pretty_print(result) << '\n';
      }
   catch (_RestartRd&)
      {
      // Python's `except Exception` catches _RestartRd (str() is empty).
      std::cout << "%%% " << '\n';
      }
   catch (const std::exception& ex)
      {
      std::cout << "%%% " << _debug_err_str(ex) << '\n';
      }
   _step_hook = saved_hook; // finally
   }

void Debugger::safe_inspect(Context* ctx, Environment* env, const std::string& source)
   {
   auto saved_hook = _step_hook;
   _step_hook = std::nullopt;
   try
      {
      auto forms = scheme_parse(source, "<debug>");
      Value result = NIL_VALUE;
      for (auto& form : forms)
         {
         Value expanded = expand(form);
         analyze(expanded);
         if (ctx->lEval)
            result = ctx->lEval(env, expanded);
         }
      debug_run_inspect(result, ctx);
      }
   catch (_RestartRd&)
      {
      std::cout << "%%% " << '\n';
      }
   catch (const std::exception& ex)
      {
      std::cout << "%%% " << _debug_err_str(ex) << '\n';
      }
   _step_hook = saved_hook; // finally
   }

void Debugger::debug_eval(Context* ctx, Environment* env, const std::string& source)
   {
   try
      {
      auto forms = scheme_parse(source, "<debug>");
      Value result = NIL_VALUE;
      for (auto& form : forms)
         {
         Value expanded = expand(form);
         analyze(expanded);
         if (ctx->lEval)
            result = ctx->lEval(env, expanded);
         }
      std::cout << "==> " << scheme_pretty_print(result) << '\n';
      }
   catch (_RestartRd&)
      {
      throw;
      }
   catch (const std::exception& ex)
      {
      std::cout << "%%% " << _debug_err_str(ex) << '\n';
      }
   }

// ── Unified prompt loop ───────────────────────────────────────────────────────

std::string Debugger::_prompt_loop(Context* ctx, Environment* env,
                                   int depth, StepHook* step_hook)
   {
   bool in_execution = (depth >= 0);
   _swap_history_in();
   try
      {
      while (true)
         {
         std::string line;
         try
            {
            line = input_fn("debug> ", "");
            size_t s = line.find_first_not_of(" \t\r\n");
            if (s == std::string::npos)
               line = "";
            else
               {
               size_t e = line.find_last_not_of(" \t\r\n");
               line = line.substr(s, e - s + 1);
               }
            }
#ifdef _WIN32
         catch (const ReadlineEOFError&)
            {
            std::cout << '\n';
            _swap_history_out();
            return in_execution ? "abort" : "quit";
            }
         catch (const ReadlineInterruptError&)
            {
            std::cout << '\n';
            if (in_execution)
               {
               _swap_history_out();
               return "abort";
               }
            continue;
            }
#else
         catch (const std::exception&)
            {
            std::cout << '\n';
            _swap_history_out();
            return in_execution ? "abort" : "quit";
            }
#endif

         if (line.empty())
            {
            if (in_execution)
               {
               _swap_history_out();
               return "step";
               }
            continue;
            }

         if (_has_readline && !line.empty())
            {
#ifdef _WIN32
            readline_win_add_history(line);
#endif
            }

         if (line == "q" || line == "quit")
            {
            _swap_history_out();
            return in_execution ? "abort" : "quit";
            }
         if (line == "s")
            {
            if (!in_execution)
               {
               std::cout << "Not in execution.  Use rd to run.\n";
               continue;
               }
            // Test the PARAMETER step_hook (matching Python `if step_hook is None`
            // and the sibling n/o branches), so a fresh StepHook resets any
            // residual _skip_until_depth when 's' is pressed in breakpoint mode.
            if (!step_hook)
               _step_hook = StepHook{};
            _swap_history_out();
            return "step";
            }
         if (line == "n")
            {
            if (!in_execution)
               {
               std::cout << "Not in execution.  Use rd to run.\n";
               continue;
               }
            if (step_hook)
               {
               step_hook->_skip_until_depth = depth;
               }
            else
               {
               StepHook hook;
               hook._skip_until_depth = depth;
               _step_hook = hook;
               }
            _swap_history_out();
            return "step";
            }
         if (line == "o")
            {
            if (!in_execution)
               {
               std::cout << "Not in execution.  Use rd to run.\n";
               continue;
               }
            if (depth == 0)
               {
               std::cout << "Already at top level.\n";
               continue;
               }
            if (step_hook)
               {
               step_hook->_skip_until_depth = depth - 1;
               }
            else
               {
               StepHook hook;
               hook._skip_until_depth = depth - 1;
               _step_hook = hook;
               }
            _swap_history_out();
            return "step";
            }
         if (line == "c")
            {
            if (!in_execution)
               {
               std::cout << "Not in execution.  Use rd to run.\n";
               continue;
               }
            _swap_history_out();
            return "continue";
            }
         if (line == "abort")
            {
            if (!in_execution)
               {
               std::cout << "Not in execution.\n";
               continue;
               }
            _swap_history_out();
            return "abort";
            }

         try
            {
            bool handled = _dispatch(line, ctx, env);
            if (!handled)
               std::cout << "Unknown command.  Type h for help.\n";
            }
         catch (_RestartRd&)
            {
            _swap_history_out();
            throw;
            }
         catch (const std::exception& e2)
            {
            std::cout << "%%% internal debugger error: " << e2.what() << '\n';
            }
         }
      }
   catch (...)
      {
      _swap_history_out();
      throw;
      }
   }

// ── CEK loop entry point ──────────────────────────────────────────────────────

void Debugger::on_expr(const Value& C, Environment* E, const KStack& K, Context* ctx)
   {
   // ── Breakpoint check ──
   bool bp_fired = false;
   std::string bp_name;
   std::string cond_src;
   bool has_bp = !breakpoints.empty() || !_inner_targets.empty();
   bool has_bo = !break_on.empty();

   if ((has_bp || has_bo) && is_cons(C))
      {
      auto* cid = std::get<ConsCell*>(C.repr);
      auto inner_it = _inner_targets.find(cid);
      if (inner_it != _inner_targets.end())
         {
         const std::string& display_name = inner_it->second.display;
         if (!_disabled.count(display_name))
            {
            bp_fired = true;
            bp_name = display_name;
            cond_src = inner_it->second.cond;
            }
         }
      if (!bp_fired && is_symbol(car(C)))
         {
         std::string sym_name = as_symbol(car(C));
         auto bp_it = breakpoints.find(sym_name);
         if (bp_it != breakpoints.end() && !_disabled.count(sym_name))
            {
            cond_src = bp_it->second;
            bp_name = sym_name;
            bp_fired = true;
            }
         else if (has_bo && break_on.count(sym_name))
            {
            cond_src = "";
            bp_name = sym_name + " [break-on]";
            bp_fired = true;
            }
         }
      }

   if (bp_fired && !cond_src.empty())
      {
      auto saved_hook = _step_hook;
      _step_hook = std::nullopt;
      try
         {
         auto forms = scheme_parse(cond_src, "<cond>");
         Value result = NIL_VALUE;
         for (auto& form : forms)
            {
            Value expanded = expand(form);
            analyze(expanded);
            if (ctx->lEval)
               result = ctx->lEval(E, expanded);
            }
         bool is_false_val = is_boolean(result) && !as_boolean(result);
         if (is_false_val || is_nil(result))
            bp_fired = false;
         }
      catch (...)
         {
         bp_fired = false;
         }
      _step_hook = saved_hook;
      }

   if (bp_fired)
      {
      int depth = (int)K.size();
      std::string indent(std::min(depth, 20) * 2, ' ');
      std::cout << "\n*** Breakpoint: " << bp_name << " ***\n";
      std::cout << indent << scheme_pretty_print(C) << '\n';
      print_watch(E, ctx);
      _current_K = &K;
      std::string result = _prompt_loop(ctx, E, depth);
      _current_K = nullptr;
      if (result == "abort")
         {
         _step_hook = std::nullopt;
         throw SchemeRuntimeError("Aborted from breakpoint.");
         }
      return;
      }

   // ── Step check ──
   if (!_step_hook)
      return;
   StepHook& sh = *_step_hook;
   int depth = (int)K.size();

   if (sh._skip_until_depth.has_value())
      {
      if (depth > *sh._skip_until_depth)
         return;
      sh._skip_until_depth = std::nullopt;
      }

   if (sh._step_over_first)
      {
      sh._step_over_first = false;
      sh._skip_until_depth = depth;
      std::string indent(std::min(depth, 20) * 2, ' ');
      std::cout << indent << scheme_pretty_print(C) << '\n';
      print_watch(E, ctx);
      return;
      }

   std::string indent(std::min(depth, 20) * 2, ' ');
   std::cout << indent << scheme_pretty_print(C) << '\n';
   print_watch(E, ctx);
   _current_K = &K;
   std::string action = _prompt_loop(ctx, E, depth, &sh);
   _current_K = nullptr;
   if (action == "continue")
      {
      _step_hook = std::nullopt;
      }
   else if (action == "abort")
      {
      _step_hook = std::nullopt;
      throw SchemeRuntimeError("Stepped execution aborted.");
      }
   }

// ── Main debugger REPL ────────────────────────────────────────────────────────

Value Debugger::run_debugger_repl(Context* ctx, Environment* env)
   {
   std::cout << "\n*** Debugger ***\nType h for command help.\n";
   if (!breakpoints.empty())
      {
      std::cout << "Breakpoints:\n";
      std::vector<std::string> names;
      for (auto& [n, c] : breakpoints)
         names.push_back(n);
      std::sort(names.begin(), names.end());
      for (const auto& name : names)
         {
         const auto& cond = breakpoints.at(name);
         if (cond.empty())
            std::cout << "  " << name << '\n';
         else
            std::cout << "  " << name << "  :when " << cond << '\n';
         }
      }
   if (!watch_list.empty())
      {
      std::string joined;
      for (size_t k = 0; k < watch_list.size(); k++)
         {
         if (k)
            joined += ", ";
         joined += watch_list[k];
         }
      std::cout << "Watching: " << joined << '\n';
      }
   if (!break_on.empty())
      {
      std::vector<std::string> bo(break_on.begin(), break_on.end());
      std::sort(bo.begin(), bo.end());
      std::string joined;
      for (size_t k = 0; k < bo.size(); k++)
         {
         if (k)
            joined += ", ";
         joined += bo[k];
         }
      std::cout << "Break-on: " << joined << '\n';
      }
   std::cout << '\n';

   while (true)
      {
      std::string result = _prompt_loop(ctx, env);
      if (result == "quit")
         break;
      }
   return NIL_VALUE;
   }

// ── Constructor ───────────────────────────────────────────────────────────────

Debugger::Debugger()
    : break_on(_DEFAULT_BREAK_ON)
   {
   // Default input_fn: read from stdin.
   input_fn = [](const std::string& prompt, const std::string&) -> std::string
   {
      std::cout << prompt << std::flush;
      std::string line;
      if (!std::getline(std::cin, line))
         throw std::runtime_error("EOF");
      return line;
   };

   // Bind command handlers.
   auto bind = [this](const std::string& name, auto mem_fn)
   {
      _commands[name] = [this, mem_fn](const std::string& cmd, const std::string& rest,
                                       Context* ctx, Environment* env)
      {
         (this->*mem_fn)(cmd, rest, ctx, env);
      };
   };
   bind("b", &Debugger::_cmd_b);
   bind("b-", &Debugger::_cmd_b);
   bind("b!", &Debugger::_cmd_b);
   bind("bc", &Debugger::_cmd_bc);
   bind("bc-", &Debugger::_cmd_bc);
   bind("bo", &Debugger::_cmd_bo);
   bind("bo-", &Debugger::_cmd_bo);
   bind("bo+", &Debugger::_cmd_bo);
   bind("bt", &Debugger::_cmd_bt);
   bind("body", &Debugger::_cmd_body);
   bind("e", &Debugger::_cmd_e);
   bind("h", &Debugger::_cmd_h);
   bind("i", &Debugger::_cmd_i);
   bind("rd", &Debugger::_cmd_rd);
   bind("v", &Debugger::_cmd_v);
   bind("w", &Debugger::_cmd_w);
   bind("w-", &Debugger::_cmd_w);

   // Help text (port of Python method docstrings).
   _help_order = {"abort", "b", "bc", "bo", "bt", "body", "c", "e", "h", "i",
                  "n", "o", "q", "rd", "s", "v", "w"};

   _cmd_help["abort"] = "abort  abort execution to top level";
   _cmd_help["b"] =
       "b [*]              list all breakpoints (numbered)\n"
       "b name ...         set breakpoint(s) on symbol name(s)\n"
       "b name (cond) ...  conditional breakpoint(s)\n"
       "b fn:call[:n]      break at nth call to call inside fn body\n"
       "b fn:call:?        list calls and choose interactively\n"
       "b fn:call:*        break on all calls to call in fn\n"
       "b! name|#          toggle enable/disable on a breakpoint\n"
       "b- name|# ...      remove breakpoint(s)\n"
       "b- fn:*            remove all inner breakpoints in fn\n"
       "b- fn:call:*       remove all breakpoints on call in fn\n"
       "b- *               clear all breakpoints";
   _cmd_help["bc"] =
       "bc name|# (cond) ...  add/change condition on breakpoint(s)\n"
       "bc- name|# ...         clear condition from breakpoint(s)";
   _cmd_help["bo"] =
       "bo [*]       show break-on list (auto-breaks during rd)\n"
       "bo name ...  add names to break-on list\n"
       "bo- name ...  remove from break-on list\n"
       "bo- *         clear break-on list\n"
       "bo+           restore default break-on list";
   _cmd_help["bt"] = "bt  show backtrace (shadow call stack)";
   _cmd_help["body"] = "body fn  show expanded body of fn";
   _cmd_help["c"] = "c  continue execution to next breakpoint";
   _cmd_help["e"] = "e expr  evaluate expr in current environment";
   _cmd_help["h"] = "h [cmd]  show help for all commands or a specific command";
   _cmd_help["i"] = "i expr  evaluate and interactively inspect the result";
   _cmd_help["n"] = "n  step over (skip deeper sub-expressions)";
   _cmd_help["o"] = "o  step out (continue until current function returns)";
   _cmd_help["q"] = "q / quit  exit the debugger";
   _cmd_help["rd"] =
       "rd expr  run expr with debugging active\n"
       "rd       re-run the current rd expression";
   _cmd_help["s"] = "s  step into the next expression";
   _cmd_help["v"] =
       "v [n]       show local variables (n limits scope depth)\n"
       "v name ...  show specific named variables";
   _cmd_help["w"] =
       "w [*]          show watch list (numbered)\n"
       "w expr ...     add watches (variables or expressions)\n"
       "w- expr|# ...  remove from watch list\n"
       "w- *           clear watch list";
   }
