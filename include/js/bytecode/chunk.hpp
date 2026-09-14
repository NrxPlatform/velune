#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include <js/environment.hpp>
#include <js/value.hpp>

namespace js::bytecode {

class BytecodeBuilder;

enum class UpvalueSource : std::uint8_t {
    local,
    upvalue,
};

struct UpvalueDescriptor final {
    UpvalueSource source;
    std::uint32_t index;
};

inline constexpr std::uint32_t no_handler_target = std::numeric_limits<std::uint32_t>::max();

struct ModuleExportBinding final {
    std::uint32_t local_index;
    std::uint32_t module_index;
};

struct ExceptionHandler final {
    std::uint32_t try_start;
    std::uint32_t try_end;
    std::uint32_t catch_start{no_handler_target};
    std::uint32_t catch_end{no_handler_target};
    std::uint32_t finally_start{no_handler_target};
    std::uint32_t finally_end{no_handler_target};
};

class BytecodeChunk final {
public:
    BytecodeChunk() = default;

    [[nodiscard]] const std::vector<std::uint8_t>& code() const noexcept { return code_; }
    [[nodiscard]] const std::vector<Value>& constants() const noexcept { return constants_; }
    [[nodiscard]] const std::vector<UpvalueDescriptor>& upvalues() const noexcept { return upvalues_; }
    [[nodiscard]] const std::vector<ExceptionHandler>& exception_handlers() const noexcept { return exception_handlers_; }
    [[nodiscard]] const std::vector<ModuleExportBinding>& module_exports() const noexcept { return module_exports_; }
    [[nodiscard]] const std::vector<BindingState>& local_binding_states() const noexcept { return local_binding_states_; }
    [[nodiscard]] const std::vector<bool>& local_binding_immutable() const noexcept { return local_binding_immutable_; }
    [[nodiscard]] std::size_t code_size() const noexcept { return code_.size(); }
    [[nodiscard]] std::size_t constant_count() const noexcept { return constants_.size(); }
    [[nodiscard]] std::uint32_t local_count() const noexcept { return local_count_; }
    [[nodiscard]] std::uint32_t upvalue_count() const noexcept { return static_cast<std::uint32_t>(upvalues_.size()); }
    [[nodiscard]] std::uint32_t module_binding_count() const noexcept { return module_binding_count_; }

private:
    friend class BytecodeBuilder;

    std::vector<std::uint8_t> code_;
    std::vector<Value> constants_;
    std::vector<UpvalueDescriptor> upvalues_;
    std::vector<ExceptionHandler> exception_handlers_;
    std::vector<ModuleExportBinding> module_exports_;
    std::vector<BindingState> local_binding_states_;
    std::vector<bool> local_binding_immutable_;
    std::uint32_t local_count_{0};
    std::uint32_t module_binding_count_{0};
};

} // namespace js::bytecode
