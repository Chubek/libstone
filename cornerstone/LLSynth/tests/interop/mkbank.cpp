#include "sandstone.hpp"
#include <fstream>

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    using namespace domsymas;
    Builder builder(true);
    TargetRec target;
    target.arch = "x86-64";
    target.addr_size = 64;
    builder.add_target(target);
    SyntaxRec syntax;
    syntax.name = "intel";
    syntax.separator = ",";
    auto syn = builder.add_syntax(syntax);
    for (auto [name, source] : {
            std::pair{"nop", "emit8(0x90);"},
            std::pair{"ret", "emit8(0xc3);"}}) {
        auto mnemonic = builder.add_mnemonic(name, {}, 0);
        auto encoder = builder.add_encoder(name, source, 0, 16, 0);
        auto form = builder.add_form(mnemonic, 0, 0, encoder, {}, {}, {});
        builder.link_mnemonic_form(mnemonic, form);
        builder.link_syntax_form(syn, form);
    }
    Databank bank;
    if (!builder.build(bank).ok()) return 1;
    std::vector<uint8_t> bytes;
    if (!bank.to_bytes(bytes).ok()) return 1;
    std::ofstream output(argv[1], std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    return output ? 0 : 1;
}
