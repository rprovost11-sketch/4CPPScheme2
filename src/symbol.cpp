#include "symbol.h"
#include <vector>
#include <unordered_map>

static std::vector<std::string>             g_symbol_names;
static std::unordered_map<std::string, uint32_t> g_symbol_pool;


uint32_t intern_symbol(std::string_view name)
   {
   std::string key(name);
   auto it = g_symbol_pool.find(key);
   if (it != g_symbol_pool.end())
      return it->second;
   uint32_t id = static_cast<uint32_t>(g_symbol_names.size());
   g_symbol_names.push_back(key);
   g_symbol_pool.emplace(std::move(key), id);
   return id;
   }


const std::string& symbol_name(uint32_t sid)
   {
   return g_symbol_names[sid];
   }


void intern_table_clear()
   {
   g_symbol_names.clear();
   g_symbol_pool.clear();
   }
