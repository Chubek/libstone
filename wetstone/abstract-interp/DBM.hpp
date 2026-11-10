#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>
namespace wetstone::ai {
// x_i-x_j <= c; caller may reserve x_0 as constant zero.
class DBM {
public:
    static constexpr std::int64_t inf=INT64_MAX/4;
private:
    std::size_t n_=0;std::vector<std::int64_t> d_;bool empty_=false;
    void check(std::size_t i)const{if(i>=n_)throw std::out_of_range("DBM index");}
    std::int64_t& at(std::size_t i,std::size_t j){return d_[i*n_+j];}
    std::int64_t at(std::size_t i,std::size_t j)const{return d_[i*n_+j];}
public:
    explicit DBM(std::size_t n=0):n_(n),d_(n*n,inf){for(std::size_t i=0;i<n;++i)at(i,i)=0;}
    static DBM top(std::size_t n){return DBM(n);}
    static DBM bottom(std::size_t n){auto x=DBM(n);x.empty_=true;return x;}
    std::size_t dimensions()const{return n_;}
    bool is_bottom()const{return empty_;}
    std::int64_t bound(std::size_t i,std::size_t j)const{check(i);check(j);return at(i,j);}
    void constrain(std::size_t i,std::size_t j,std::int64_t c){check(i);check(j);if(empty_)return;at(i,j)=std::min(at(i,j),std::clamp(c,-inf,inf));close();}
    void close(){
        if(empty_)return;
        for(std::size_t k=0;k<n_;++k)for(std::size_t i=0;i<n_;++i)if(at(i,k)!=inf)
            for(std::size_t j=0;j<n_;++j)if(at(k,j)!=inf){
                auto sum=static_cast<__int128>(at(i,k))+at(k,j);
                auto c=static_cast<std::int64_t>(std::clamp(sum,static_cast<__int128>(-inf),static_cast<__int128>(inf)));
                if(c<at(i,j))at(i,j)=c;
            }
        for(std::size_t i=0;i<n_;++i)if(at(i,i)<0){empty_=true;break;}
    }
    bool leq(const DBM& b)const{compatible(b);if(empty_)return true;if(b.empty_)return false;
        for(std::size_t i=0;i<d_.size();++i)if(d_[i]>b.d_[i])return false;return true;}
    DBM join(const DBM& b)const{compatible(b);if(empty_)return b;if(b.empty_)return *this;DBM r=*this;
        for(std::size_t i=0;i<d_.size();++i)r.d_[i]=std::max(d_[i],b.d_[i]);r.close();return r;}
    DBM meet(const DBM& b)const{compatible(b);if(empty_||b.empty_)return bottom(n_);DBM r=*this;
        for(std::size_t i=0;i<d_.size();++i)r.d_[i]=std::min(d_[i],b.d_[i]);r.close();return r;}
    DBM widen(const DBM& b)const{compatible(b);if(empty_)return b;if(b.empty_)return *this;DBM r=*this;
        for(std::size_t i=0;i<d_.size();++i)if(b.d_[i]>d_[i])r.d_[i]=inf;return r;}
    void compatible(const DBM& b)const{if(n_!=b.n_)throw std::invalid_argument("DBM dimensional mismatch");}
    friend bool operator==(const DBM& a,const DBM& b){return a.n_==b.n_ && (a.empty_&&b.empty_ || (a.empty_==b.empty_&&a.d_==b.d_));}
};
}
