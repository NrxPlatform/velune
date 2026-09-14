#include <js/context.hpp>

#include <regex>
#include <string>
#include <string_view>

#include "detail/heap.hpp"

namespace js {
namespace {
Result<std::regex> compile_regex(const detail::HeapObject& object) {
    auto syntax = std::regex_constants::ECMAScript;
    if (object.regexp_ignore_case) syntax |= std::regex_constants::icase;
    try {
        return std::regex(object.regexp_pattern, syntax);
    } catch (const std::regex_error& error) {
        return Error{ErrorCode::type_error, std::string("invalid regular expression: ") + error.what()};
    }
}
}

bool Context::is_regexp(const Value& value) const noexcept {
    return value.is_object() && value.as_heap_object()->is_regexp;
}

Result<Value> Context::regexp(std::string_view pattern, std::string_view flags) {
    bool global = false;
    bool ignore_case = false;
    bool multiline = false;
    bool seen_g = false, seen_i = false, seen_m = false;
    for (char flag : flags) {
        switch (flag) {
        case 'g': if (seen_g) return Error{ErrorCode::type_error, "duplicate RegExp flag 'g'"}; seen_g = true; global = true; break;
        case 'i': if (seen_i) return Error{ErrorCode::type_error, "duplicate RegExp flag 'i'"}; seen_i = true; ignore_case = true; break;
        case 'm': if (seen_m) return Error{ErrorCode::type_error, "duplicate RegExp flag 'm'"}; seen_m = true; multiline = true; break;
        default: return Error{ErrorCode::unsupported, std::string("unsupported RegExp flag '") + flag + "'"};
        }
    }

    ensure_builtins();
    Realm& realm = active_realm();
    Value value = runtime_->make_object(realm);
    auto* object = value.as_heap_object();
    object->is_regexp = true;
    object->object_kind = ObjectKind::RegExp;
    object->regexp_pattern = std::string(pattern);
    object->regexp_flags = std::string(flags);
    object->regexp_global = global;
    object->regexp_ignore_case = ignore_case;
    object->regexp_multiline = multiline;
    object->prototype = realm.regexp_prototype_;

    const auto compiled = compile_regex(*object);
    if (!compiled) return compiled.error();
    return value;
}

Result<Value> Context::regexp_exec(Value regexp_value, Value input) {
    const auto rv = validate(regexp_value); if (!rv) return rv.error();
    const auto iv = validate(input); if (!iv) return iv.error();
    if (!is_regexp(regexp_value)) return Error{ErrorCode::type_error, "RegExp.prototype.exec receiver is not a RegExp"};
    if (!input.is_string()) return Error{ErrorCode::type_error, "RegExp input must be a string"};

    auto* object = regexp_value.as_heap_object();
    const auto compiled = compile_regex(*object); if (!compiled) return compiled.error();
    const std::string text(input.as_string());
    const std::size_t start = object->regexp_global ? object->regexp_last_index : 0U;
    if (start > text.size()) { if (object->regexp_global) object->regexp_last_index = 0; return Value::null(); }

    std::smatch match;
    const auto begin = text.cbegin() + static_cast<std::ptrdiff_t>(start);
    auto flags = std::regex_constants::match_default;
    if (object->regexp_multiline && start != 0U) flags |= std::regex_constants::match_not_bol;
    if (!std::regex_search(begin, text.cend(), match, *compiled, flags)) {
        if (object->regexp_global) object->regexp_last_index = 0;
        return Value::null();
    }

    Value result = array();
    for (std::size_t i = 0; i < match.size(); ++i) {
        const auto appended = array_push(result, string(match.str(i))); if (!appended) return appended.error();
    }
    const std::size_t match_index = start + static_cast<std::size_t>(match.position(0));
    if (object->regexp_global) object->regexp_last_index = match_index + static_cast<std::size_t>(match.length(0));
    const auto index_set = set_own_property(result, "index", number(static_cast<double>(match_index))); if (!index_set) return index_set.error();
    const auto input_set = set_own_property(result, "input", input); if (!input_set) return input_set.error();
    return result;
}

Result<Value> Context::regexp_test(Value regexp_value, Value input) {
    const auto result = regexp_exec(regexp_value, input); if (!result) return result.error();
    return boolean(!result->is_null());
}
} // namespace js
