// JNI export names: "Java_" + mangled class and method, optional "__" + mangled argument types.
// Escapes: _1 = '_', _2 = ';', _3 = '[', _0xxxx = UTF-16 code unit; a plain '_' is '/'.
#include <cstdio>

#include "check.h"
#include "zb/jni_mangle.h"

int main() {
    auto simple = zb::decode_jni_export("Java_org_haxe_lime_Lime_onTouch");
    CHECK(simple && simple->class_name == "org/haxe/lime/Lime" && simple->method == "onTouch");
    CHECK(!simple->arguments);

    auto underscore = zb::decode_jni_export("Java_com_example_My_1Class_do_1it");
    CHECK(underscore && underscore->class_name == "com/example/My_Class" && underscore->method == "do_it");

    auto overloaded = zb::decode_jni_export("Java_pkg_Foo_bar__ILjava_lang_String_2_3I");
    CHECK(overloaded && overloaded->class_name == "pkg/Foo" && overloaded->method == "bar");
    CHECK(overloaded->arguments && *overloaded->arguments == "(ILjava/lang/String;[I)");

    auto no_args = zb::decode_jni_export("Java_pkg_Foo_bar__");
    CHECK(no_args && no_args->arguments && *no_args->arguments == "()");

    auto unicode = zb::decode_jni_export("Java_pkg_Caf_000e9_run");
    CHECK(unicode && unicode->class_name == "pkg/Caf\xC3\xA9" && unicode->method == "run");

    auto default_package = zb::decode_jni_export("Java_Main_start");
    CHECK(default_package && default_package->class_name == "Main" && default_package->method == "start");

    CHECK(!zb::decode_jni_export("JNI_OnLoad"));
    CHECK(!zb::decode_jni_export("Java_nomethod"));
    CHECK(!zb::decode_jni_export("Java_pkg_Foo_bar_"));
    CHECK(!zb::decode_jni_export("Java_pkg_Foo_bar_0zz12"));
    CHECK(!zb::decode_jni_export("Java_pkg_Foo_bar__I__J"));

    std::printf("jni_mangle_test ok\n");
    return 0;
}
