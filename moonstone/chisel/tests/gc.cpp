#include <chisel/GC.hpp>
#include <cassert>
using namespace moonstone::chisel;
int main(){TracingHeap h;auto a=h.allocate(12),b=h.allocate(4),c=h.allocate(8);h.set_references(a,{b});auto r=h.collect({a});assert(r.retained==2&&r.reclaimed==1&&h.size()==2);h.pin(a);assert(h.collect().retained==2);h.unpin(a);assert(h.collect().reclaimed==2);try{h.get(c);assert(false);}catch(const std::out_of_range&) {}}
