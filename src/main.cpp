#include "gc.h"
#include "environment.h"
#include "symbol.h"
#include <iostream>


int main()
   {
   gc_init();
   std::cout << "CEKScheme 0.1.0\n";
   gc_shutdown();
   return 0;
   }
