#ifdef _WIN32
#define EXPORT_HIDDEN 
#define EXPORT __declspec( dllexport )
#else
#define EXPORT_HIDDEN __attribute__((visibility("hidden")))
#define EXPORT __attribute__((visibility("default")))
#endif
#if defined(_MSC_VER)
#define KEEP_SYMBOL
#else
/* This fixture exists to be looked up by name: the optimizer would drop the
 * unreferenced statics and the symbol table queries would then find nothing. */
#define KEEP_SYMBOL __attribute__((used))
#endif

KEEP_SYMBOL static const char* static_data = "static_data";
const char* extern_data = "data2";


KEEP_SYMBOL static void static_function(const char* fmt,...) {}
EXPORT_HIDDEN extern void hidden_extern_function(const char* fmt,...) {static_function(static_data);}
EXPORT extern void extern_function(const char* fmt,...) {static_function(static_data);}