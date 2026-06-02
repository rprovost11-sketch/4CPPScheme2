// primitives/help_sys.cpp -- (help) and (apropos) primitives.
// Direct port of pyscheme/primitives/help_sys.py.
#include "help_sys.h"
#include "primitives.h"
#include "../AST.h"
#include "../Environment.h"
#include "../Context.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

static const char* CATEGORY = "help_sys";

static SourceInfo* _src(const Value* a)
   {
   return a ? src_of(*a) : nullptr;
   }

// ── Layout helper ─────────────────────────────────────────────────────────────

struct Layout
   {
   int nrows, ncols;
   std::vector<int> colwidths;
   };

static Layout* _try_layout(const std::vector<std::string>& items, int nrows, int width)
   {
   int size = static_cast<int>(items.size());
   int ncols = (size + nrows - 1) / nrows;
   std::vector<int> colwidths;
   int total = -2;
   for (int c = 0; c < ncols; ++c)
      {
      int w = 0;
      for (int r = 0; r < nrows; ++r)
         {
         int i = r + nrows * c;
         if (i >= size)
            break;
         int len = static_cast<int>(items[static_cast<size_t>(i)].size());
         if (len > w)
            w = len;
         }
      colwidths.push_back(w);
      total += w + 2;
      if (total > width)
         return nullptr;
      }
   return new Layout{nrows, ncols, std::move(colwidths)};
   }

static std::vector<std::string> _columnize(const std::vector<std::string>& items, int width = 78)
   {
   if (items.empty())
      return {};
   if (items.size() == 1)
      return {items[0]};

   Layout* best = nullptr;
   for (int nrows = 1; nrows <= static_cast<int>(items.size()); ++nrows)
      {
      Layout* layout = _try_layout(items, nrows, width);
      if (layout)
         {
         best = layout;
         break;
         }
      }
   if (!best)
      {
      int widest = 0;
      for (auto& s : items)
         if (static_cast<int>(s.size()) > widest)
            widest = static_cast<int>(s.size());
      best = new Layout{static_cast<int>(items.size()), 1, {widest}};
      }

   std::vector<std::string> lines;
   for (int r = 0; r < best->nrows; ++r)
      {
      std::string row;
      for (int c = 0; c < best->ncols; ++c)
         {
         int i = r + best->nrows * c;
         if (i >= static_cast<int>(items.size()))
            break;
         if (c > 0)
            row += "  ";
         std::string cell = items[static_cast<size_t>(i)];
         if (c < best->ncols - 1)
            cell.resize(static_cast<size_t>(best->colwidths[static_cast<size_t>(c)]), ' ');
         row += cell;
         }
      // rstrip
      while (!row.empty() && row.back() == ' ')
         row.pop_back();
      lines.push_back(row);
      }
   delete best;
   return lines;
   }

// ── Topic discovery ───────────────────────────────────────────────────────────

static std::string _help_dir()
   {
   // Compute absolute path: __FILE__ is .../src/primitives/help_sys.cpp
   // so go up three levels (primitives/ -> src/ -> project root/) then into help/.
   // This mirrors PyScheme's os.path.dirname(os.path.abspath(__file__)) approach.
   static std::string dir;
   if (dir.empty())
      {
      std::filesystem::path p(__FILE__);
      std::filesystem::path candidate =
          p.parent_path().parent_path().parent_path() / "help";
      if (std::filesystem::is_directory(candidate))
         dir = candidate.string();
      else
         dir = "help"; // CWD-relative fallback
      }
   return dir;
   }

static void _scan_dir(const std::filesystem::path& root, const std::string& section,
                      std::unordered_map<std::string, std::vector<std::string>>& topics)
   {
   std::error_code ec;
   for (auto& entry : std::filesystem::directory_iterator(root, ec))
      {
      if (entry.is_directory())
         {
         std::string sub = section.empty() ? entry.path().filename().string() : section;
         _scan_dir(entry.path(), sub, topics);
         }
      else
         {
         std::string name = entry.path().filename().string();
         if (name.size() > 4 && name.substr(name.size() - 4) == ".txt")
            {
            std::string stem = name.substr(0, name.size() - 4);
            topics[section].push_back(stem);
            }
         }
      }
   }

static std::unordered_map<std::string, std::vector<std::string>> _discover_topics()
   {
   std::unordered_map<std::string, std::vector<std::string>> topics;
   std::filesystem::path hdir(_help_dir());
   if (!std::filesystem::is_directory(hdir))
      return topics;
   _scan_dir(hdir, "", topics);
   for (auto& [sec, stems] : topics)
      std::sort(stems.begin(), stems.end());
   return topics;
   }

static std::string _find_topic_in_dir(const std::filesystem::path& root, const std::string& lo_name)
   {
   std::error_code ec;
   for (auto& entry : std::filesystem::directory_iterator(root, ec))
      {
      if (entry.is_directory())
         {
         std::string found = _find_topic_in_dir(entry.path(), lo_name);
         if (!found.empty())
            return found;
         }
      else
         {
         std::string name = entry.path().filename().string();
         if (name.size() > 4 && name.substr(name.size() - 4) == ".txt")
            {
            std::string stem = name.substr(0, name.size() - 4);
            std::string lo_stem = stem;
            std::transform(lo_stem.begin(), lo_stem.end(), lo_stem.begin(), ::tolower);
            if (lo_stem == lo_name)
               return entry.path().string();
            }
         }
      }
   return {};
   }

static std::string _find_topic(const std::string& name)
   {
   std::filesystem::path hdir(_help_dir());
   if (!std::filesystem::is_directory(hdir))
      return {};
   std::string lo = name;
   std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
   return _find_topic_in_dir(hdir, lo);
   }

// ── Label helpers ─────────────────────────────────────────────────────────────

static std::string _type_label(const std::string& kind)
   {
   if (kind == "special")
      return "Special Form";
   if (kind == "primitive")
      return "Primitive";
   if (kind == "procedure")
      return "Procedure";
   if (kind == "macro")
      return "Macro";
   return kind;
   }

static std::string _args_label(const std::string& kind)
   {
   if (kind == "special" || kind == "macro")
      return "args: unevaluated";
   return "args: pre-evaluated";
   }

// ── Listing helpers ───────────────────────────────────────────────────────────

static std::unordered_map<std::string, std::vector<std::string>> _collect_primitives_by_category()
   {
   std::unordered_map<std::string, std::vector<std::string>> by_cat;
   for (auto& [name, info] : primitive_help())
      by_cat[info.category].push_back(name);
   for (auto& [cat, names] : by_cat)
      std::sort(names.begin(), names.end());
   return by_cat;
   }

static std::vector<std::string> _collect_closures(Environment* global_env)
   {
   std::vector<std::string> closures;
   for (auto& [sid, val] : global_env->_bindings)
      {
      if (is_closure(val))
         closures.push_back(symbol_name(sid));
      }
   std::sort(closures.begin(), closures.end());
   return closures;
   }

static std::vector<std::string> _category_print_order(
    const std::unordered_map<std::string, std::vector<std::string>>& by_cat)
   {
   const auto& order_list = category_order();
   std::vector<std::string> order;
   std::unordered_map<std::string, bool> seen;
   for (auto& cat : order_list)
      {
      if (by_cat.count(cat))
         {
         order.push_back(cat);
         seen[cat] = true;
         }
      }
   std::vector<std::string> remaining;
   for (auto& [cat, _] : by_cat)
      {
      if (!seen.count(cat))
         remaining.push_back(cat);
      }
   std::sort(remaining.begin(), remaining.end());
   for (auto& cat : remaining)
      order.push_back(cat);
   return order;
   }

static void _write_doc_block(Context* ctx, const std::string& doc)
   {
   if (doc.empty())
      {
      ctx->write("   (no documentation)\n");
      return;
      }
   std::istringstream ss(doc);
   std::string line;
   while (std::getline(ss, line))
      {
      if (!line.empty())
         ctx->write("   " + line + "\n");
      else
         ctx->write("\n");
      }
   }

static void _write_topics_section(Context* ctx,
                                  const std::unordered_map<std::string, std::vector<std::string>>& topics)
   {
   ctx->write("Topics\n");
   ctx->write("------\n");
   std::vector<std::string> section_keys;
   for (auto& [k, _] : topics)
      section_keys.push_back(k);
   std::sort(section_keys.begin(), section_keys.end());
   for (auto& section : section_keys)
      {
      const auto& stems = topics.at(section);
      std::vector<std::string> items;
      for (auto& s : stems)
         items.push_back("\"" + s + "\"");
      if (!section.empty())
         {
         std::string title = section;
         title[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(title[0])));
         ctx->write("  " + title + ": ");
         std::string joined;
         for (size_t i = 0; i < items.size(); ++i)
            {
            if (i)
               joined += "  ";
            joined += items[i];
            }
         ctx->write(joined + "\n");
         }
      else
         {
         for (auto& line : _columnize(items))
            ctx->write(line + "\n");
         }
      }
   ctx->write("\n");
   }

static void _list_all(Context* ctx, Environment* env)
   {
   const auto& cat_titles = category_titles();
   auto by_cat = _collect_primitives_by_category();
   auto closures = _collect_closures(env->getGlobalEnv());

   ctx->write("Scheme Documentation\n");
   ctx->write("====================\n\n");

   for (auto& cat : _category_print_order(by_cat))
      {
      std::string title;
      auto it = cat_titles.find(cat);
      if (it != cat_titles.end())
         title = it->second;
      else
         {
         title = cat;
         if (!title.empty())
            title[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(title[0])));
         }
      ctx->write(title + "\n");
      ctx->write(std::string(title.size(), '-') + "\n");
      for (auto& line : _columnize(by_cat[cat]))
         ctx->write(line + "\n");
      ctx->write("\n");
      }

   if (!closures.empty())
      {
      ctx->write("Procedures\n");
      ctx->write("----------\n");
      for (auto& line : _columnize(closures))
         ctx->write(line + "\n");
      ctx->write("\n");
      }

   auto topics = _discover_topics();
   if (!topics.empty())
      _write_topics_section(ctx, topics);

   ctx->write("Type (help <name>) for documentation on a name.\n");
   ctx->write("Type (help \"topic\") to view a topic or search by substring.\n");
   ctx->write("Type (apropos \"substring\") to list every matching name.\n");
   }

// ── Per-entry helpers ─────────────────────────────────────────────────────────

static bool _show_primitive_entry(Context* ctx, const std::string& name)
   {
   const auto& ph = primitive_help();
   auto it = ph.find(name);
   if (it == ph.end())
      return false;
   const auto& info = it->second;
   ctx->write(_type_label(info.kind) + "  |  " + _args_label(info.kind) + "\n");
   ctx->write("\n");
   ctx->write("   Usage: " + info.usage + "\n");
   ctx->write("\n");
   _write_doc_block(ctx, info.doc);
   return true;
   }

static std::string _closure_usage(const std::string& name, const Value& closure)
   {
   const auto& param_ids = as_closure_params(closure);
   uint32_t rest_id = as_closure_rest_name(closure);
   std::vector<std::string> parts;
   for (uint32_t id : param_ids)
      parts.push_back(symbol_name(id));
   std::string rest_str = (rest_id != UINT32_MAX) ? symbol_name(rest_id) : std::string();

   if (rest_id != UINT32_MAX)
      {
      if (parts.empty())
         return "(" + name + " . " + rest_str + ")";
      std::string joined;
      for (auto& p : parts)
         joined += " " + p;
      return "(" + name + joined + " . " + rest_str + ")";
      }
   if (parts.empty())
      return "(" + name + ")";
   std::string joined;
   for (auto& p : parts)
      joined += " " + p;
   return "(" + name + joined + ")";
   }

static void _show_closure_entry(Context* ctx, const std::string& name, const Value& closure)
   {
   ctx->write("Procedure  |  args: pre-evaluated\n");
   ctx->write("\n");
   ctx->write("   Usage: " + _closure_usage(name, closure) + "\n");
   ctx->write("\n");
   _write_doc_block(ctx, as_closure_docstring(closure));
   }

static bool _show_topic(Context* ctx, const std::string& name)
   {
   std::string path = _find_topic(name);
   if (path.empty())
      return false;
   std::ifstream f(path);
   if (!f.is_open())
      return false;
   std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
   ctx->write(text);
   if (text.empty() || text.back() != '\n')
      ctx->write("\n");
   return true;
   }

// ── Apropos ───────────────────────────────────────────────────────────────────

static std::vector<std::string> _collect_matching_names(Environment* global_env, const std::string& lo)
   {
   std::vector<std::string> result;
   for (auto& [sid, val_] : global_env->_bindings)
      {
      std::string name = symbol_name(sid);
      std::string lo_name = name;
      std::transform(lo_name.begin(), lo_name.end(), lo_name.begin(), ::tolower);
      if (lo_name.find(lo) != std::string::npos)
         result.push_back(name);
      }
   std::sort(result.begin(), result.end());
   return result;
   }

static std::vector<std::string> _collect_matching_topics(
    const std::unordered_map<std::string, std::vector<std::string>>& topics, const std::string& lo)
   {
   std::vector<std::string> result;
   std::vector<std::string> keys;
   for (auto& [k, _] : topics)
      keys.push_back(k);
   std::sort(keys.begin(), keys.end());
   for (auto& section : keys)
      {
      for (auto& stem : topics.at(section))
         {
         std::string lo_stem = stem;
         std::transform(lo_stem.begin(), lo_stem.end(), lo_stem.begin(), ::tolower);
         if (lo_stem.find(lo) != std::string::npos)
            result.push_back("\"" + stem + "\"");
         }
      }
   std::sort(result.begin(), result.end());
   return result;
   }

static void _apropos(Context* ctx, Environment* env, const std::string& pattern)
   {
   std::string lo = pattern;
   std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
   auto global_env = env->getGlobalEnv();
   auto names = _collect_matching_names(global_env, lo);
   auto topics = _discover_topics();
   auto topic_matches = _collect_matching_topics(topics, lo);

   if (names.empty() && topic_matches.empty())
      {
      ctx->write("No matches for '" + pattern + "'.\n");
      return;
      }
   if (!names.empty())
      {
      ctx->write("Names\n");
      ctx->write("-----\n");
      for (auto& line : _columnize(names))
         ctx->write(line + "\n");
      ctx->write("\n");
      }
   if (!topic_matches.empty())
      {
      ctx->write("Topics\n");
      ctx->write("------\n");
      for (auto& line : _columnize(topic_matches))
         ctx->write(line + "\n");
      ctx->write("\n");
      }
   }

// ── Primitives ────────────────────────────────────────────────────────────────

static bool _val_equal(const Value& a, const Value& b)
   {
   if (is_closure(a) && is_closure(b))
      return std::get<SchemeClosure*>(a.repr) == std::get<SchemeClosure*>(b.repr);
   if (is_case_closure(a) && is_case_closure(b))
      return std::get<CaseClosure*>(a.repr) == std::get<CaseClosure*>(b.repr);
   return false;
   }

static Value _prim_help(Context* ctx, Environment* env, std::vector<Value>& args, const Value* app)
   {
   if (args.empty())
      {
      _list_all(ctx, env);
      return VOID_VALUE;
      }
   const Value& target = args[0];

   if (is_string(target))
      {
      const std::string& name = as_string(target);
      if (!_show_topic(ctx, name))
         _apropos(ctx, env, name);
      return VOID_VALUE;
      }

   if (is_primitive(target))
      {
      const std::string& name = as_primitive_name(target);
      if (!_show_primitive_entry(ctx, name))
         ctx->write("No help for primitive '" + name + "'.\n");
      return VOID_VALUE;
      }

   if (is_closure(target))
      {
      Environment* global_env = env->getGlobalEnv();
      std::string matched_name = "<anonymous>";
      for (auto& [sid, val] : global_env->_bindings)
         {
         if (_val_equal(val, target))
            {
            matched_name = symbol_name(sid);
            break;
            }
         }
      _show_closure_entry(ctx, matched_name, target);
      return VOID_VALUE;
      }

   if (is_symbol(target))
      {
      const std::string& name = as_symbol(target);
      Environment* global_env = env->getGlobalEnv();
      auto val_opt = global_env->lookup_optional(name);
      if (!val_opt)
         {
         ctx->write("No binding for '" + name + "'.\n");
         return VOID_VALUE;
         }
      if (is_primitive(*val_opt))
         _show_primitive_entry(ctx, as_primitive_name(*val_opt));
      else if (is_closure(*val_opt))
         _show_closure_entry(ctx, name, *val_opt);
      else
         ctx->write(name + ": non-callable binding.\n");
      return VOID_VALUE;
      }

   throw SchemeTypeError(
       "help: argument must be a procedure, primitive, symbol, string, or omitted",
       _src(app));
   }

static Value _prim_apropos(Context* ctx, Environment* env, std::vector<Value>& args, const Value* app)
   {
   if (!is_string(args[0]))
      throw SchemeTypeError("apropos: expected a string argument", _src(app));
   _apropos(ctx, env, as_string(args[0]));
   return VOID_VALUE;
   }

// ── Registration ──────────────────────────────────────────────────────────────

void register_help_sys()
   {
   register_primitive("help", 0, 1, _prim_help,
                      "",
                      "Interactive documentation.\n"
                      "\n"
                      "With no argument, list everything available in the global environment.\n"
                      "With a callable argument, print usage and documentation for that procedure.\n"
                      "With a string argument, look up a help topic; if not found, do apropos search.",
                      CATEGORY);
   register_primitive("apropos", 1, 1, _prim_apropos,
                      "",
                      "Print every global binding whose name contains the given substring "
                      "(case-insensitive), along with any help topic whose name matches.",
                      CATEGORY);
   }
