#include "common/capabilities.h"

namespace luadebug::debugger {

    namespace {

        Json make_filter(bool default_value, const char* filter, const char* label, const char* description, const char* condition_description = nullptr) {
            Json item                 = Json::object();
            item["default"]           = default_value;
            item["filter"]            = filter;
            item["label"]             = label;
            item["supportsCondition"] = true;
            item["description"]       = description;
            if (condition_description) {
                item["conditionDescription"] = condition_description;
            }
            return item;
        }

        Json build_capabilities() {
            Json caps                                 = Json::object();
            caps["supportsConfigurationDoneRequest"]  = true;
            caps["supportsFunctionBreakpoints"]       = true;
            caps["supportsConditionalBreakpoints"]    = true;
            caps["supportsHitConditionalBreakpoints"] = true;
            caps["supportsEvaluateForHovers"]         = true;
            caps["supportsSetVariable"]               = true;
            caps["supportsRestartFrame"]              = true;
            caps["supportsRestartRequest"]            = true;
            caps["supportsExceptionInfoRequest"]      = true;
            caps["supportTerminateDebuggee"]          = true;
            caps["supportSuspendDebuggee"]            = true;
            caps["supportsDelayedStackTraceLoading"]  = true;
            caps["supportsLoadedSourcesRequest"]      = true;
            caps["supportsLogPoints"]                 = true;
            caps["supportsTerminateThreadsRequest"]   = true;
            caps["supportsSetExpression"]             = true;
            caps["supportsTerminateRequest"]          = true;
            caps["supportsReadMemoryRequest"]         = true;
            caps["supportsWriteMemoryRequest"]        = true;
            caps["supportsDisassembleRequest"]        = true;
            caps["supportsInstructionBreakpoints"]    = true;
            caps["supportsClipboardContext"]          = true;
            caps["supportsExceptionFilterOptions"]    = true;
            caps["supportsANSIStyling"]               = true;

            const char* caught = "error:match \"attempt to index a nil value\"";
            Json filters       = Json::array();
            filters.push_back(make_filter(false, "runtime", "Runtime Errors", "Error raised by lua runtime."));
            filters.push_back(make_filter(false, "assert", "Assertion Errors", "Error raised by `assert`."));
            filters.push_back(make_filter(false, "error", "Common Errors", "Error raised by `error`."));
            filters.push_back(make_filter(false, "syntax", "Syntax Errors", "Syntax error."));
            filters.push_back(make_filter(false, "other", "Native Errors", "Error raised by `lua_error`."));
            filters.push_back(make_filter(false, "lua", "Caught Errors By Lua", "Breaks on errors, if it is caught by lua.", caught));
            filters.push_back(make_filter(true, "native", "Caught Errors By Native", "Breaks on errors, if it is caught by native code.", caught));
            filters.push_back(make_filter(true, "panic", "Uncaught Errors", "Breaks only on errors that are nor handled.", caught));
            caps["exceptionBreakpointFilters"] = std::move(filters);
            return caps;
        }

    }  // namespace

    const Json& capabilities() {
        static const Json caps = build_capabilities();
        return caps;
    }

}  // namespace luadebug::debugger
