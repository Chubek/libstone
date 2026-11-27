#include <TTyUtils/TTyUtils-TreeView.hpp>
#include <cassert>
using namespace ttyutils::ui;

struct Model : TreeModel {
    mutable int fetched{};
    std::vector<TreeNodeRef> roots() const override { return {{"/src", "src"}, {"/other", "other"}}; }
    std::vector<TreeNodeRef> children(const std::string& path) const override {
        ++fetched;
        if (path == "/src") return {{"/src/main", "main"}};
        if (path == "/src/main") return {{"/src", "cycle"}};
        return {};
    }
};

int main() {
    auto model = std::make_shared<Model>();
    TreeView tree{model};
    tree.set_rect({0, 0, 12, 2});
    assert(tree.length() == 2 && model->fetched == 0);
    tree.expand("/src");
    assert(tree.length() == 3 && model->fetched == 1);
    Canvas canvas(tree.rect());
    tree.render(canvas);
    assert(canvas.texts().size() == 2 && canvas.texts()[1].value == "│ ▸ main");
    tree.select(1);
    std::string path;
    tree.on_choose([&](const TreeNodeRef& node) { path = node.path; });
    tree.choose();
    assert(path == "/src/main");
    tree.collapse_all();
    assert(tree.length() == 2);
    tree.expand_all();
    assert(tree.length() == 3 && model->fetched == 3);
    tree.set_model(std::make_shared<Model>());
    path.clear();
    tree.choose();
    assert(path == "/src/main");
}
