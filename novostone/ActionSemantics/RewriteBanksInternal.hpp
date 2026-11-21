#pragma once
#include "RewriteBanks.hpp"
namespace novostone::action::detail {
void augmentBankContract(RewriteBank&, const BankValue&, const std::string&);
}
