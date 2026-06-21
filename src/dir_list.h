// dir_list.h -- native directory listing for (directory-files ...).
//
// Kept free of Scheme/AST headers (only std types) so dir_list.cpp can include
// <windows.h> without the BOOLEAN-typedef-vs-AST.h-enum clash (the plugin_loader /
// process_exec isolation pattern).
#ifndef CPPSCHEME2_DIR_LIST_H
#define CPPSCHEME2_DIR_LIST_H

#include <string>
#include <vector>

// Append the bare entry names of directory `path` to `out`, EXCLUDING "." and
// ".." (dotfiles are kept).  Names are returned unsorted (the caller sorts).
// Returns false if the directory cannot be opened.
bool list_directory(const std::string& path, std::vector<std::string>& out);

#endif  // CPPSCHEME2_DIR_LIST_H
