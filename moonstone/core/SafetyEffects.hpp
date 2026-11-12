#pragma once
#include "Core.hpp"
namespace moonstone {
enum class Effect { Read, Write, Allocate, Free, BorrowShared, BorrowExclusive, Escape, ForeignCall, ThreadSpawn, Suspend };
struct EffectSummary {std::set<Effect> effects;bool unknown=false;
 void merge(const EffectSummary& rhs){effects.insert(rhs.effects.begin(),rhs.effects.end());unknown|=rhs.unknown;}
 bool is_pure()const{return !unknown&&effects.empty();}
};
struct OwnershipContract {std::set<std::size_t> consumed_arguments;std::set<std::size_t> borrowed_arguments;bool returned_borrow=false;bool may_escape=false;EffectSummary effects;};
}
