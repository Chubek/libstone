#include "Backends.hpp"

namespace sandstone::archml {
namespace {
class Data final : public Target {
    bool json_;
public:
    explicit Data(bool json) : json_(json) {}
    std::string_view name() const override { return json_ ? "json" : "archml"; }
    void generate(const Document &doc, std::ostream &out, const TargetOptions &options) const override {
        if (!options.namespace_name.empty()) throw std::invalid_argument("--namespace requires the cpp target");
        if (json_) out << json(doc.root()) << '\n';
        else {
            out << "(archml 1 " << doc.name();
            for (std::size_t i = 3; i < doc.root().children.size(); ++i)
                out << "\n  " << format(doc.root().children[i]);
            out << ")\n";
        }
    }
};
}
std::unique_ptr<Target> data_target(bool json) { return std::make_unique<Data>(json); }
}
