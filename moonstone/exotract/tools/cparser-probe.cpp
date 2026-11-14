// Isolated compatibility probe for vendored cparser-iso11. It is intentionally
// not the ABI authority: ExoML's CDecl graph is the only trusted typed output.
#include <CParser.h>
#include <CLexer.h>
#include <AbstractSyntaxTree.h>
#include <exception>
#include <iostream>
int main(int argc,char **argv){
    if(argc!=3) {std::cerr<<"usage: cparser-probe INPUT OUTPUT\n";return 2;}
    try {
        cparser::CLexer lexer(argv[1]);
        cparser::CParser parser(&lexer);
        parser.parse(argv[2]);
        auto *ast=parser.getAST();
        if(!ast || ast->getErrors()!=0){std::cerr<<"cparser errors: "<<(ast?ast->getErrors():-1)<<"\n";return 3;}
        return 0;
    }catch(const std::exception &e){std::cerr<<e.what()<<"\n";return 4;}
}
