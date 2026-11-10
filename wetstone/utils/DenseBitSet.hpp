#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>
namespace wetstone {
class DenseBitSet {
    std::size_t bits_=0;std::vector<std::uint64_t> words_;
    void check(std::size_t i)const{if(i>=bits_)throw std::out_of_range("bit index");}
public:
    explicit DenseBitSet(std::size_t n=0):bits_(n),words_((n+63)/64){}
    std::size_t size()const{return bits_;}
    void set(std::size_t i,bool v=true){check(i);auto& w=words_[i/64];auto bit=1ULL<<(i%64);if(v)w|=bit;else w&=~bit;}
    bool test(std::size_t i)const{check(i);return !!(words_[i/64]&(1ULL<<(i%64)));}
    void unite(const DenseBitSet& o){if(bits_!=o.bits_)throw std::invalid_argument("bitset width");for(std::size_t i=0;i<words_.size();++i)words_[i]|=o.words_[i];}
    void intersect(const DenseBitSet& o){if(bits_!=o.bits_)throw std::invalid_argument("bitset width");for(std::size_t i=0;i<words_.size();++i)words_[i]&=o.words_[i];}
    friend bool operator==(const DenseBitSet&,const DenseBitSet&)=default;
};
}
