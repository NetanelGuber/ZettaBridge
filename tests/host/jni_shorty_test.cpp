// Shorty: return type first, then one letter per parameter; references and arrays are 'L'.
#include <cstdio>
#include <string>

#include "check.h"
#include "zb/jni_shorty.h"

int main() {
    CHECK(zb::shorty_from_signature("(IFFIFF)I") == std::string("IIFFIFF"));
    CHECK(zb::shorty_from_signature("(J)V") == std::string("VJ"));
    CHECK(zb::shorty_from_signature("(JLjava/lang/String;[Ljava/lang/Object;)D") == std::string("DJLL"));
    CHECK(zb::shorty_from_signature("()V") == std::string("V"));
    CHECK(zb::shorty_from_signature("([[I[Z)Lorg/haxe/lime/HaxeObject;") == std::string("LLL"));
    CHECK(zb::shorty_from_signature("(ZBCS)C") == std::string("CZBCS"));

    CHECK(!zb::shorty_from_signature("(V)V"));
    CHECK(!zb::shorty_from_signature("(I"));
    CHECK(!zb::shorty_from_signature("(Ljava/lang/String)V"));
    CHECK(!zb::shorty_from_signature("(L;)V"));
    CHECK(!zb::shorty_from_signature("(I)"));
    CHECK(!zb::shorty_from_signature("(I)VX"));
    CHECK(!zb::shorty_from_signature("I)V"));
    CHECK(!zb::shorty_from_signature("([)V"));

    std::printf("jni_shorty_test ok\n");
    return 0;
}
