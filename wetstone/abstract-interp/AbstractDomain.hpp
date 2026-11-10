#pragma once
#include <concepts>
namespace wetstone::ai {
// A finite or widening-equipped lattice: join must overapproximate union,
// widen must overapproximate join, and leq is the precision ordering.
template<class T> concept AbstractDomain = requires(const T& a,const T& b) {
    {T::top()} -> std::same_as<T>;
    {T::bottom()} -> std::same_as<T>;
    {a.join(b)} -> std::same_as<T>;
    {a.meet(b)} -> std::same_as<T>;
    {a.widen(b)} -> std::same_as<T>;
    {a.leq(b)} -> std::convertible_to<bool>;
    {a==b} -> std::convertible_to<bool>;
};
template<AbstractDomain T> T narrow(const T& a,const T& b){return a.meet(b);}
}
