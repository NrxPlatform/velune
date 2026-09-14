#include <js/module/module.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <js/compiler/compiler.hpp>
#include <js/context.hpp>
#include <js/error.hpp>
#include <js/frontend/frontend.hpp>
#include <js/runtime.hpp>
#include <js/vm/vm.hpp>

#include "detail/heap.hpp"

namespace js::module {
namespace {

struct ImportEntry final {
    std::string local_name;
    std::string imported_name;
    std::string request;
    std::uint32_t module_index{0};
};

struct ExportEntry final {
    std::string export_name;
    std::string local_name;
};

[[nodiscard]] Error module_error(std::string message) {
    return Error{ErrorCode::compile_error, "module error: " + std::move(message)};
}

void collect_declared_names(const frontend::ASTNode& node, std::vector<std::string>& names) {
    if (node.type == frontend::ASTNodeType::FUNCTION_DECLARATION) {
        const auto& function = static_cast<const frontend::FunctionDeclarationNode&>(node);
        names.emplace_back(function.id->name);
        return;
    }
    if (node.type == frontend::ASTNodeType::VARIABLE_DECLARATION) {
        const auto& declaration = static_cast<const frontend::VariableDeclarationNode&>(node);
        for (const auto& declarator : declaration.declarations) {
            if (declarator->id && declarator->id->type == frontend::ASTNodeType::IDENTIFIER) {
                names.emplace_back(static_cast<const frontend::IdentifierNode&>(*declarator->id).name);
            }
        }
    }
}

} // namespace

struct ModuleSystem::Impl final {
    enum class State : std::uint8_t { loading, linking, linked, evaluating, evaluated };

    struct Record final {
        std::string name;
        std::string source;
        std::unique_ptr<frontend::ProgramNode> program;
        std::vector<ImportEntry> imports;
        std::vector<ExportEntry> exports;
        std::unordered_map<std::string, std::uint32_t> export_indices;
        detail::HeapModuleEnvironment* environment{nullptr};
        bytecode::BytecodeChunk chunk;
        BytecodeRoot bytecode_root;
        State state{State::loading};
    };

    Impl(Context& module_context, ModuleLoader module_loader)
        : context(&module_context), loader(std::move(module_loader)) {}

    ~Impl() {
        for (auto& [name, record] : records) {
            (void)name;
            if (record->environment != nullptr) context->runtime().unregister_module_environment(*record->environment);
        }
    }

    [[nodiscard]] Result<Record*> load(std::string_view specifier, std::string_view referrer) {
        const auto loaded = loader(specifier, referrer);
        if (!loaded) return loaded.error();
        if (loaded->name.empty()) return module_error("loader returned an empty canonical module name");

        const auto existing = records.find(loaded->name);
        if (existing != records.end()) return existing->second.get();

        auto record = std::make_unique<Record>();
        record->name = loaded->name;
        record->source = loaded->source;

        auto parsed = frontend::parse_program(record->source);
        if (!parsed) {
            return Error{ErrorCode::compile_error,
                         "module '" + record->name + "' parse error at " +
                         std::to_string(parsed.diagnostic().location.line) + ":" +
                         std::to_string(parsed.diagnostic().location.column) + ": " +
                         parsed.diagnostic().message};
        }
        record->program = parsed.take_program();

        std::uint32_t import_index = 0;
        for (const auto& statement : record->program->body) {
            if (statement->type == frontend::ASTNodeType::IMPORT_DECLARATION) {
                const auto& declaration = static_cast<const frontend::ImportDeclarationNode&>(*statement);
                if (declaration.source->value.size() < 2U) return module_error("invalid import source literal");
                const std::string request(declaration.source->value.substr(1U, declaration.source->value.size() - 2U));
                for (const auto& spec : declaration.specifiers) {
                    if (import_index == std::numeric_limits<std::uint32_t>::max()) return module_error("too many module imports");
                    record->imports.push_back(ImportEntry{
                        std::string(spec->local->name),
                        std::string(spec->imported->name),
                        request,
                        import_index++,
                    });
                }
            } else if (statement->type == frontend::ASTNodeType::EXPORT_NAMED_DECLARATION) {
                const auto& declaration = static_cast<const frontend::ExportNamedDeclarationNode&>(*statement);
                if (declaration.declaration) {
                    std::vector<std::string> names;
                    collect_declared_names(*declaration.declaration, names);
                    for (auto& name : names) record->exports.push_back(ExportEntry{name, name});
                } else {
                    for (const auto& spec : declaration.specifiers) {
                        record->exports.push_back(ExportEntry{std::string(spec->name), std::string(spec->name)});
                    }
                }
            }
        }

        for (std::size_t i = 0; i < record->exports.size(); ++i) {
            const auto& name = record->exports[i].export_name;
            if (record->export_indices.contains(name)) return module_error("duplicate export '" + name + "' in module '" + record->name + "'");
            const std::size_t env_index = record->imports.size() + i;
            if (env_index > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) return module_error("module binding space exhausted");
            record->export_indices.emplace(name, static_cast<std::uint32_t>(env_index));
        }

        record->environment = context->runtime().make_module_environment(record->imports.size() + record->exports.size());
        context->runtime().register_module_environment(*record->environment);

        Record* raw = record.get();
        records.emplace(record->name, std::move(record));
        return raw;
    }

    [[nodiscard]] Result<void> link_record(Record& record) {
        if (record.state == State::linked || record.state == State::evaluating || record.state == State::evaluated) return {};
        if (record.state == State::linking) return {};
        if (record.state != State::loading) return module_error("invalid module state while linking '" + record.name + "'");
        record.state = State::linking;

        std::vector<compiler::ModuleImportBinding> compiler_imports;
        compiler_imports.reserve(record.imports.size());

        for (const auto& import : record.imports) {
            const auto dependency = load(import.request, record.name);
            if (!dependency) return dependency.error();
            const auto linked = link_record(**dependency);
            if (!linked) return linked.error();

            const auto exported = (*dependency)->export_indices.find(import.imported_name);
            if (exported == (*dependency)->export_indices.end()) {
                return module_error("module '" + (*dependency)->name + "' has no export named '" + import.imported_name + "'");
            }
            record.environment->bindings[import.module_index].target_environment = (*dependency)->environment;
            record.environment->bindings[import.module_index].target_index = exported->second;
            compiler_imports.push_back(compiler::ModuleImportBinding{import.local_name, import.module_index});
        }

        std::vector<compiler::ModuleExportRequest> export_requests;
        export_requests.reserve(record.exports.size());
        for (const auto& entry : record.exports) {
            export_requests.push_back(compiler::ModuleExportRequest{entry.export_name, entry.local_name, record.export_indices.at(entry.export_name)});
        }

        const auto compiled = compiler::compile_module(*context, *record.program, compiler_imports, export_requests);
        if (!compiled) return compiled.error();
        record.chunk = std::move(*compiled);
        auto rooted = context->runtime().root_bytecode(record.chunk);
        if (!rooted) return rooted.error();
        record.bytecode_root = std::move(rooted).value();
        record.state = State::linked;
        return {};
    }

    [[nodiscard]] Result<void> evaluate_record(Record& record) {
        if (record.state == State::evaluated) return {};
        if (record.state == State::evaluating) return {}; // cycle: linking is complete; evaluation continues from the active branch.
        if (record.state != State::linked) return module_error("module '" + record.name + "' is not linked");

        record.state = State::evaluating;
        for (const auto& import : record.imports) {
            const auto target = record.environment->bindings[import.module_index].target_environment;
            if (target == nullptr) return module_error("unlinked import in module '" + record.name + "'");
            Record* dependency = nullptr;
            for (auto& [name, candidate] : records) {
                (void)name;
                if (candidate->environment == target) { dependency = candidate.get(); break; }
            }
            if (dependency == nullptr) return module_error("internal dependency record missing");
            const auto evaluated = evaluate_record(*dependency);
            if (!evaluated) return evaluated.error();
        }

        VM vm(*context);
        const auto result = vm.run_module(record.chunk, *record.environment);
        if (!result) return result.error().legacy_error();
        if (result.completion().is_throw()) {
            return Error{ErrorCode::uncaught_exception, "uncaught JavaScript exception during module evaluation: " + result.completion().value().to_debug_string()};
        }
        if (!result.completion().is_normal()) return Error{ErrorCode::internal, "module evaluation escaped with non-script completion"};
        record.state = State::evaluated;
        return {};
    }

    Context* context;
    ModuleLoader loader;
    std::unordered_map<std::string, std::unique_ptr<Record>> records;
};

ModuleSystem::ModuleSystem(Context& context, ModuleLoader loader)
    : impl_(std::make_unique<Impl>(context, std::move(loader))) {}
ModuleSystem::~ModuleSystem() = default;

Result<void> ModuleSystem::link(std::string_view entry_specifier) {
    const auto entry = impl_->load(entry_specifier, {});
    if (!entry) return entry.error();
    return impl_->link_record(**entry);
}

Result<Value> ModuleSystem::evaluate(std::string_view entry_specifier) {
    const auto entry = impl_->load(entry_specifier, {});
    if (!entry) return entry.error();
    const auto linked = impl_->link_record(**entry);
    if (!linked) return linked.error();
    const auto evaluated = impl_->evaluate_record(**entry);
    if (!evaluated) return evaluated.error();
    return Value::undefined();
}

Result<Value> ModuleSystem::get_export(std::string_view module_name, std::string_view export_name) const {
    const auto found = impl_->records.find(std::string(module_name));
    if (found == impl_->records.end()) return Error{ErrorCode::reference_error, "module '" + std::string(module_name) + "' is not loaded"};
    const auto binding = found->second->export_indices.find(std::string(export_name));
    if (binding == found->second->export_indices.end()) return Error{ErrorCode::reference_error, "module '" + std::string(module_name) + "' has no export named '" + std::string(export_name) + "'"};
    return found->second->environment->get(binding->second);
}

std::size_t ModuleSystem::module_count() const noexcept {
    return impl_->records.size();
}

} // namespace js::module
