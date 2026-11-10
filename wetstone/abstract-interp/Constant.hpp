#pragma once
#include "Interval.hpp"
#include <optional>
namespace wetstone::ai {
struct Constant {
    enum class Kind{Bottom,Value,Top};
    Kind kind=Kind::Top;std::int64_t value=0;
    static Constant top(){return {};}
    static Constant bottom(){return {Kind::Bottom,0};}
    static Constant point(std::int64_t v){return {Kind::Value,v};}
    bool leq(const Constant& o)const{return kind==Kind::Bottom||o.kind==Kind::Top || (kind==Kind::Value&&o.kind==Kind::Value&&value==o.value);}
    Constant join(const Constant& o)const{if(kind==Kind::Bottom)return o;if(o.kind==Kind::Bottom)return *this;if(kind==Kind::Value&&o.kind==Kind::Value&&value==o.value)return *this;return top();}
    Constant meet(const Constant& o)const{if(kind==Kind::Top)return o;if(o.kind==Kind::Top)return *this;if(kind==Kind::Value&&o.kind==Kind::Value&&value==o.value)return *this;return bottom();}
    Constant widen(const Constant& o)const{return join(o);}
    Interval interval()const{return kind==Kind::Bottom?Interval::bottom():kind==Kind::Top?Interval::top():Interval::point(value);}
    friend bool operator==(const Constant&,const Constant&)=default;
};
}
