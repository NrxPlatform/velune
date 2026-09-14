#include <iostream>
#include <string_view>
#include <js/js.hpp>
int main() {
    js::Runtime runtime; js::Context context(runtime);
    constexpr std::string_view source="let a=[10,20]; a.push(30); a[1]=a[1]+2; a.length + a[0] + a[1] + a[2]";
    auto parsed=js::frontend::parse_program(source); if(!parsed){ std::cerr<<parsed.diagnostic().message<<'\n'; return 1; }
    auto chunk=js::compiler::compile_program(context,parsed.program()); if(!chunk){ std::cerr<<chunk.error()<<'\n'; return 1; }
    js::VM vm(context); auto result=vm.run(*chunk); if(!result){ std::cerr<<result.error()<<'\n'; return 1; }
    std::cout<<"result: "<<result->to_debug_string()<<'\n';
    std::cout<<"runtime heap cells retained: "<<runtime.heap_cell_count()<<'\n';
}
