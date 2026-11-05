#include <frida-gum.h>
#include <stdio.h>

#ifdef _WIN32
#define EXPORT_HIDDEN 
#define EXPORT __declspec( dllexport )
#else
#define EXPORT_HIDDEN __attribute__((visibility("hidden")))
#define EXPORT __attribute__((visibility("default")))
#endif

EXPORT_HIDDEN extern void hidden_extern_function(
    const char*,
    ...);
EXPORT extern void extern_function(const char*,
                                                                  ...);
extern const char* extern_data;

const char* GumType_to_string(GumSymbolType t){
    switch (t)
    {
    /* Common */
    case GUM_SYMBOL_SECTION:
        return"section";

    /* Mach-O */
    case GUM_SYMBOL_UNDEFINED:
        return"undefined";
    case GUM_SYMBOL_ABSOLUTE:
        return"absulute";
    case GUM_SYMBOL_PREBOUND_UNDEFINED:
        return"prebound_undefined";
    case GUM_SYMBOL_INDIRECT:
        return"indirect";

    /* ELF */
    case GUM_SYMBOL_OBJECT:
        return"object";
    case GUM_SYMBOL_FUNCTION:
        return"function";
    case GUM_SYMBOL_FILE:
        return"file";
    case GUM_SYMBOL_COMMON:
        return"common";
    case GUM_SYMBOL_TLS:
        return"tls";
    case GUM_SYMBOL_UNKNOWN:
    default:
        return "unknown";
    }
}

gboolean enumerate_exports(const GumExportDetails* details,
                           gpointer user_data) {
  fprintf(stdout, "exports %llu,%s,%d\n", details->address, details->name,
          details->type);
  return true;
}
gboolean FoundSymbolFunc (const GumSymbolDetails * details,
    gpointer user_data){
  fprintf(stdout, "FoundSymbolFunc %s,\t%llu,\t%s,\t%s\n", details->is_global?"global":"no global", details->address, details->name,
          GumType_to_string(details->type));
  return true;
    }
int main() {
  extern_function(extern_data);
  gum_init_embedded();

  // frida 17: `gum_module_find_symbol_by_name` is a GumModule method (it used to
  // take the module *name*); passing a string here is read as a module object and
  // crashes inside the GType interface lookup.
  GumModule* module = gum_process_find_module_by_name("test_symbol");
  g_assert(module != NULL);

  g_assert(gum_module_find_symbol_by_name(module, "extern_function") ==
           GPOINTER_TO_SIZE(&extern_function));
  g_assert(gum_module_find_symbol_by_name(module, "hidden_extern_function") ==
           GPOINTER_TO_SIZE(&hidden_extern_function));
  g_assert((void*)gum_module_find_symbol_by_name(module, "extern_data") ==
           (void*)&extern_data);

  g_assert(gum_module_find_symbol_by_name(module, "static_function") != 0);
  g_assert(gum_module_find_symbol_by_name(module, "static_data") != 0);

  g_object_unref(module);
  return 0;
}