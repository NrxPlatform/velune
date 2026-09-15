#include "test.hpp"
#include <string_view>
#include <js/js.hpp>
namespace {
js::Result<js::Value> eval10(js::Context& context, std::string_view source) {
    auto parsed=js::frontend::parse_program(source); if(!parsed) return js::Error{js::ErrorCode::internal,"test parse failed"};
    auto chunk=js::compiler::compile_program(context,parsed.program()); 
    
    if(!chunk) return chunk.error();
    
    js::VM vm(context); 
    return vm.run(*chunk);
}
}
TEST_CASE("array literal supports indexed reads and length"){ js::Runtime r; js::Context c(r); auto v=eval10(c,"let a=[10,20,30]; a[1]+a.length"); REQUIRE(v); REQUIRE(v->as_number()==23.0); }
TEST_CASE("array indexed write extends length") { js::Runtime r; js::Context c(r); auto v=eval10(c,"let a=[1]; a[3]=7; a.length+a[3]"); REQUIRE(v); REQUIRE(v->as_number()==11.0); }
TEST_CASE("array prototype push and pop are ordinary native methods") { js::Runtime r; js::Context c(r); auto v=eval10(c,"let a=[1,2]; a.push(9); a.pop()+a.length"); REQUIRE(v); REQUIRE(v->as_number()==11.0); }
TEST_CASE("Array isArray builtin recognizes array identity") { js::Runtime r; js::Context c(r); auto v=eval10(c,"Array.isArray([1])"); REQUIRE(v); REQUIRE(v->is_boolean()); REQUIRE(v->as_boolean()); }
TEST_CASE("Number isFinite builtin is installed modularly") { 
    js::Runtime r; 
    js::Context c(r); 
    auto v=eval10(c,"Number.isFinite(42)"); 
    REQUIRE(v); 
    REQUIRE(v->as_boolean()); 
}
TEST_CASE("Object getPrototypeOf exposes the real Array prototype object") { 
    js::Runtime r; 
    js::Context c(r); 
    auto v=eval10(c,"let a=[]; Object.getPrototypeOf(a) === Array.prototype && Array.isArray(Array.prototype)"); 
    REQUIRE(v); 
    REQUIRE(v->is_boolean());
    REQUIRE(v->as_boolean()); 
}

TEST_CASE("string length basic works through property access") { 
    js::Runtime r; 
    js::Context c(r); 
    auto v=eval10(c,"\"hello\".length"); 
    REQUIRE(v); 
    REQUIRE(v->as_number()==5.0); 
}

TEST_CASE("computed string key accesses ordinary object") { 
    js::Runtime r; 
    js::Context c(r); 
    auto v=eval10(c,"let o={x:8}; o[\"x\"]"); 
    REQUIRE(v); 
    REQUIRE(v->as_number()==8.0); 
}

TEST_CASE("array holes are distinct from explicit undefined") {
    js::Runtime r; js::Context c(r);
    const auto a = c.array();
    REQUIRE(c.define_own_property(a, "0", js::PropertyDescriptor::data(c.undefined(), true, true, true)));
    const auto present = c.has_property(a, "0"); REQUIRE(present); REQUIRE(*present);
    const auto deleted = c.delete_property(a, "0"); REQUIRE(deleted); REQUIRE(*deleted);
    const auto missing = c.has_property(a, "0"); REQUIRE(missing); REQUIRE(!*missing);
    const auto length = c.get_own_property(a, "length"); REQUIRE(length); REQUIRE(length->as_number() == 1.0);
}

TEST_CASE("array length growth creates holes rather than undefined properties") {
    js::Runtime r; js::Context c(r);
    const auto a = c.array();
    js::PropertyDescriptor length; length.value = c.number(4);
    const auto grown = c.define_own_property(a, "length", length); REQUIRE(grown); REQUIRE(*grown);
    const auto present = c.has_property(a, "3"); REQUIRE(present); REQUIRE(!*present);
    const auto value = c.get_property(a, "3"); REQUIRE(value); REQUIRE(value->is_undefined());
}

TEST_CASE("array length shrink deletes configurable indexed properties") {
    js::Runtime r; js::Context c(r);
    const auto a = c.array();
    REQUIRE(c.set_own_property(a, "0", c.number(1)));
    REQUIRE(c.set_own_property(a, "3", c.number(4)));
    js::PropertyDescriptor length; length.value = c.number(1);
    const auto shrunk = c.define_own_property(a, "length", length); REQUIRE(shrunk); REQUIRE(*shrunk);
    const auto present = c.has_property(a, "3"); REQUIRE(present); REQUIRE(!*present);
    const auto actual_length = c.get_own_property(a, "length"); REQUIRE(actual_length); REQUIRE(actual_length->as_number() == 1.0);
}

TEST_CASE("non configurable indexed property blocks array length shrink") {
    js::Runtime r; js::Context c(r);
    const auto a = c.array();
    REQUIRE(c.define_own_property(a, "3", js::PropertyDescriptor::data(c.number(9), true, true, false)));
    js::PropertyDescriptor length; length.value = c.number(1);
    const auto shrunk = c.define_own_property(a, "length", length); REQUIRE(shrunk); REQUIRE(!*shrunk);
    const auto actual_length = c.get_own_property(a, "length"); REQUIRE(actual_length); REQUIRE(actual_length->as_number() == 4.0);
    const auto present = c.has_property(a, "3"); REQUIRE(present); REQUIRE(*present);
}

TEST_CASE("non writable array length rejects indexes beyond length") {
    js::Runtime r; js::Context c(r);
    const auto a = c.array();
    REQUIRE(c.set_own_property(a, "0", c.number(1)));
    js::PropertyDescriptor lock; lock.writable = false;
    const auto locked = c.define_own_property(a, "length", lock); REQUIRE(locked); REQUIRE(*locked);
    const auto write = c.define_own_property(a, "1", js::PropertyDescriptor::data(c.number(2), true, true, true));
    REQUIRE(write); REQUIRE(!*write);
}

TEST_CASE("sparse array index grows length without dense allocation semantics") {
    js::Runtime r; js::Context c(r);
    const auto a = c.array();
    REQUIRE(c.set_own_property(a, "100000", c.number(7)));
    const auto length = c.get_own_property(a, "length"); REQUIRE(length); REQUIRE(length->as_number() == 100001.0);
    const auto value = c.get_property(a, "100000"); REQUIRE(value); REQUIRE(value->as_number() == 7.0);
    const auto hole = c.has_property(a, "99999"); REQUIRE(hole); REQUIRE(!*hole);
}

TEST_CASE("array index 2^32 minus 1 is an ordinary property") {
    js::Runtime r; js::Context c(r);
    const auto a = c.array();
    REQUIRE(c.set_own_property(a, "4294967295", c.number(5)));
    const auto length = c.get_own_property(a, "length"); REQUIRE(length); REQUIRE(length->as_number() == 0.0);
    const auto value = c.get_property(a, "4294967295"); REQUIRE(value); REQUIRE(value->as_number() == 5.0);
}

TEST_CASE("array defineProperty defaults do not become dense writable properties") {
    js::Runtime r; js::Context c(r);
    const auto a = c.array();
    js::PropertyDescriptor descriptor; descriptor.value = c.number(3);
    const auto defined = c.define_own_property(a, "0", descriptor); REQUIRE(defined); REQUIRE(*defined);
    const auto own = c.get_own_property_descriptor(a, "0"); REQUIRE(own); REQUIRE(own->has_value());
    REQUIRE(!(*own)->writable.value_or(true));
    REQUIRE(!(*own)->enumerable.value_or(true));
    REQUIRE(!(*own)->configurable.value_or(true));
}

TEST_CASE("Object defineProperty uses ECMAScript descriptor defaults") {
    js::Runtime r; js::Context c(r);
    auto v = eval10(c,
        "let o={}; Object.defineProperty(o,'x',{value:7}); "
        "o.x===7 && Object.keys(o).length===0");
    REQUIRE(v); REQUIRE(v->is_boolean()); REQUIRE(v->as_boolean());
}

TEST_CASE("Object defineProperty supports enumerable data and accessor descriptors") {
    js::Runtime r; js::Context c(r);
    auto v = eval10(c,
        "let o={}; "
        "Object.defineProperty(o,'a',{value:3,writable:true,enumerable:true,configurable:true}); "
        "Object.defineProperty(o,'b',{get:function(){return 9;},enumerable:true,configurable:true}); "
        "Object.keys(o).length===2 && Object.keys(o)[0]==='a' && Object.keys(o)[1]==='b' && o.b===9");
    REQUIRE(v); REQUIRE(v->is_boolean()); REQUIRE(v->as_boolean());
}

TEST_CASE("Object keys returns own enumerable string keys and excludes symbols") {
    js::Runtime r; js::Context c(r);
    auto v = eval10(c,
        "let s=Symbol('s'); let o={}; o.b=2; o['1']=1; o.a=3; "
        "Object.defineProperty(o,'hidden',{value:4,enumerable:false}); o[s]=5; "
        "let k=Object.keys(o); k.length===3 && k[0]==='1' && k[1]==='b' && k[2]==='a'");
    REQUIRE(v); REQUIRE(v->is_boolean()); REQUIRE(v->as_boolean());
}


TEST_CASE("Object prototype hasOwnProperty tests own properties only") {
    js::Runtime r; js::Context c(r);
    auto v = eval10(c,
        "let proto={inherited:1}; let o={own:2}; "
        "Object.setPrototypeOf ? 0 : 0; "
        "o.hasOwnProperty('own') && !o.hasOwnProperty('missing') && "
        "Array.prototype.hasOwnProperty.call([7], '0')");
    REQUIRE(v); REQUIRE(v->is_boolean()); REQUIRE(v->as_boolean());
}

TEST_CASE("Object prototype hasOwnProperty accepts symbols and distinguishes inherited properties") {
    js::Runtime r; js::Context c(r);
    auto v = eval10(c,
        "let s=Symbol('s'); let o={}; o[s]=3; "
        "o.hasOwnProperty(s) && !o.hasOwnProperty('toString')");
    REQUIRE(v); REQUIRE(v->is_boolean()); REQUIRE(v->as_boolean());
}

TEST_CASE("Object is implements SameValue semantics") {
    js::Runtime r; js::Context c(r);
    auto v = eval10(c,
        "Object.is(NaN,NaN) && !Object.is(0,-0) && Object.is(-0,-0) && "
        "Object.is(3,3) && !Object.is(3,'3')");
    REQUIRE(v); REQUIRE(v->is_boolean()); REQUIRE(v->as_boolean());
}

TEST_CASE("Object getOwnPropertyDescriptor exposes complete data descriptors") {
    js::Runtime r; js::Context c(r);
    auto v = eval10(c,
        "let o={}; Object.defineProperty(o,'x',{value:7,writable:false,enumerable:true,configurable:false}); "
        "let d=Object.getOwnPropertyDescriptor(o,'x'); "
        "d.value===7 && d.writable===false && d.enumerable===true && d.configurable===false");
    REQUIRE(v); REQUIRE(v->is_boolean()); REQUIRE(v->as_boolean());
}

TEST_CASE("Object getOwnPropertyNames preserves own string key order and excludes symbols") {
    js::Runtime r; js::Context c(r);
    auto v = eval10(c,
        "let s=Symbol('s'); let o={b:1}; o[2]=2; o[1]=1; o.a=3; o[s]=4; "
        "let n=Object.getOwnPropertyNames(o); "
        "n.length===4 && n[0]==='1' && n[1]==='2' && n[2]==='b' && n[3]==='a'");
    REQUIRE(v); REQUIRE(v->is_boolean()); REQUIRE(v->as_boolean());
}

TEST_CASE("Object propertyIsEnumerable uses own descriptor enumerable attribute") {
    js::Runtime r; js::Context c(r);
    auto v = eval10(c,
        "let o={visible:1}; Object.defineProperty(o,'hidden',{value:2,enumerable:false}); "
        "o.propertyIsEnumerable('visible') && !o.propertyIsEnumerable('hidden') && !o.propertyIsEnumerable('missing')");
    REQUIRE(v); REQUIRE(v->is_boolean()); REQUIRE(v->as_boolean());
}

TEST_CASE("property helper primordial dependencies are callable") {
    js::Runtime r; js::Context c(r);
    auto v = eval10(c,
        "let push=Function.prototype.call.bind(Array.prototype.push); "
        "let has=Function.prototype.call.bind(Object.prototype.hasOwnProperty); "
        "let enumerable=Function.prototype.call.bind(Object.prototype.propertyIsEnumerable); "
        "let a=[]; push(a,String(Math.pow(2,5))); let o={x:1}; "
        "a[0]==='32' && has(o,'x') && enumerable(o,'x') && typeof Object.getOwnPropertyDescriptor==='function' && typeof Object.getOwnPropertyNames==='function'");
    REQUIRE(v); REQUIRE(v->is_boolean()); REQUIRE(v->as_boolean());
}

TEST_CASE("object literal getter is lazy and observes receiver access") {
    js::Runtime r; js::Context c(r);
    auto v = eval10(c,
        "let calls=0; let o={get x(){calls=calls+1; return 41+1;}}; "
        "calls===0 && o.x===42 && calls===1");
    REQUIRE(v); REQUIRE(v->is_boolean()); REQUIRE(v->as_boolean());
}

TEST_CASE("object literal getter creates an enumerable configurable accessor descriptor") {
    js::Runtime r; js::Context c(r);
    auto v = eval10(c,
        "let o={get x(){return 7;}}; let d=Object.getOwnPropertyDescriptor(o,'x'); "
        "typeof d.get==='function' && d.set===undefined && d.enumerable===true && d.configurable===true");
    REQUIRE(v); REQUIRE(v->is_boolean()); REQUIRE(v->as_boolean());
}

TEST_CASE("object spread evaluates source getter and creates a data property") {
    js::Runtime r; js::Context c(r);
    auto v = eval10(c,
        "let calls=0; let src={get x(){calls=calls+1; return 9;}}; let dst={...src}; "
        "let d=Object.getOwnPropertyDescriptor(dst,'x'); "
        "calls===1 && dst.x===9 && d.value===9 && d.get===undefined && d.writable===true && d.enumerable===true && d.configurable===true");
    REQUIRE(v); REQUIRE(v->is_boolean()); REQUIRE(v->as_boolean());
}

TEST_CASE("Symbol primitive property access boxes through Symbol prototype") {
    js::Runtime r; js::Context c(r);
    auto v = eval10(c, "let s=Symbol('foo'); s.toString()==='Symbol(foo)' && Symbol.prototype.toString.call(s)==='Symbol(foo)'");
    REQUIRE(v); REQUIRE(v->is_boolean()); REQUIRE(v->as_boolean());
}

TEST_CASE("object spread preserves Symbol primitive values that remain method-callable") {
    js::Runtime r; js::Context c(r);
    auto v = eval10(c, "let s=Symbol('foo'); let o={i:s}; let x={...o}; x.i===s && x.i.toString()==='Symbol(foo)'");
    REQUIRE(v); REQUIRE(v->is_boolean()); REQUIRE(v->as_boolean());
}
