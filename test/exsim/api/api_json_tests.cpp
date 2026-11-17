// JSON writer escaping and structural output.

#include "api/api_json.hpp"
#include "api/json.hpp"
#include "test/framework/test.hpp"

#include <string>

using namespace exsim::api;
using exsim::api::JsonValue;
using exsim::api::parse_json;

TEST_CASE(api_json, writer_escapes_and_nests) {
    ApiJsonWriter json;
    json.begin_object();
    json.key("text").string(std::string("quote\" backslash\\ newline\n tab\t nul") + std::string(1, '\0') + "\x1f");
    json.key("list").begin_array().integer_number(-3).unsigned_text(18446744073709551615ull).null().end_array();
    json.end_object();
    const std::string text = json.take();
    CHECK_EQ(text, std::string(R"({"text":"quote\" backslash\\ newline\n tab\t nul\u0000\u001f","list":[-3,"18446744073709551615",null]})"));
    const JsonValue parsed = parse_json(text);
    CHECK_EQ(parsed.find("list")->items.size(), 3u);
}

TEST_CASE(api_json, parser_combines_surrogate_pairs_and_rejects_unpaired_surrogates) {
    CHECK_EQ(parse_json(R"("😀")").text, std::string("\xF0\x9F\x98\x80"));  // U+1F600
    CHECK_EQ(parse_json(R"("�éA")").text, std::string("\xEF\xBF\xBD\xC3\xA9" "A"));
    for (const auto* text : {R"("\uD83D")", R"("\uDE00")", R"("\uD83Dx")", R"("\uD83DA")", R"("\uDE00\uD83D")"}) {
        bool rejected = false;
        try {
            (void)parse_json(text);
        } catch (const JsonError&) {
            rejected = true;
        }
        CHECK(rejected);
    }
}
