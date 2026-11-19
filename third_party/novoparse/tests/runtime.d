module runtime_tests;

import Arithmetic : createParser;
import NovoParse : ParseError;
import std.json : JSONValue;

void main()
{
    auto parser = createParser();
    scope(exit) parser.close();
    auto tree = parser.parse(" 1 + 2 * 3\n");
    assert(tree["name"].str == "Add");
    assert(tree["children"][2]["name"].str == "Multiply");
    assert(!parser.ambiguous());
    assert(parser.conflicts() == 0);
    try { parser.parse("1+"); assert(false); }
    catch (ParseError error) { assert(error.status == 1); assert(error.position == 2); }
    assert(parser.parse("2^3^4")["children"][2]["name"].str == "Power");
    parser.close();
    try { parser.parse("1"); assert(false); }
    catch (Exception error) { assert(error.msg == "parser is closed"); }
}
