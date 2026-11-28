#include <smtlib2/smtlib2.hpp>
#include <iostream>
int main() {
    smtlib2::Session session;
    for (const auto& response : session.execute(
        "(set-logic QF_UF) (declare-const x Bool) (assert (not x)) (check-sat) (get-model)"))
        std::cout << response.text << '\n';
}
