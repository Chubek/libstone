#pragma once
#include "ExoML.hpp"
#include <cmath>
#include <limits>
#include <tuple>
#include <type_traits>

namespace moonstone::exoml {
struct NativeOptions {
    bool privileged = true;
    bool thread_affine = false;
    // Explicit promise: C strings are NUL-terminated, borrowed only for the call;
    // returned const char* is readable until the wrapper finishes copying it.
    bool c_strings = false;
    std::size_t max_string_bytes = 1024 * 1024;
    OwnershipContract ownership = [] {
        OwnershipContract contract;
        contract.effects.effects.insert(Effect::ForeignCall);
        contract.effects.unknown = true;
        return contract;
    }();
    std::string documentation;
};

namespace detail {
template <class> inline constexpr bool unsupported_type = false;
template <class T> struct optional_type : std::false_type {};
template <class T> struct optional_type<std::optional<T>> : std::true_type {
    using type = T;
};
template <class T> using bare = std::remove_cvref_t<T>;

template <class T> std::string native_type_name() {
    using U = bare<T>;
    if constexpr (std::is_same_v<U, void>)
        return "void";
    else if constexpr (std::is_same_v<U, bool>)
        return "bool";
    else if constexpr (std::is_same_v<U, char>)
        return "char";
    else if constexpr (std::is_same_v<U, signed char>)
        return "signed char";
    else if constexpr (std::is_same_v<U, unsigned char>)
        return "unsigned char";
    else if constexpr (std::is_same_v<U, short>)
        return "short";
    else if constexpr (std::is_same_v<U, unsigned short>)
        return "unsigned short";
    else if constexpr (std::is_same_v<U, int>)
        return "int";
    else if constexpr (std::is_same_v<U, unsigned int>)
        return "unsigned int";
    else if constexpr (std::is_same_v<U, long>)
        return "long";
    else if constexpr (std::is_same_v<U, unsigned long>)
        return "unsigned long";
    else if constexpr (std::is_same_v<U, long long>)
        return "long long";
    else if constexpr (std::is_same_v<U, unsigned long long>)
        return "unsigned long long";
    else if constexpr (std::is_same_v<U, float>)
        return "float";
    else if constexpr (std::is_same_v<U, double>)
        return "double";
    else if constexpr (std::is_same_v<U, std::string>)
        return "std::string";
    else if constexpr (std::is_same_v<U, std::string_view>)
        return "std::string_view";
    else if constexpr (std::is_same_v<U, const char *>)
        return "const char*";
    else if constexpr (std::is_same_v<U, std::vector<std::string>>)
        return "strings";
    else if constexpr (std::is_same_v<U, Value>)
        return "value";
    else if constexpr (optional_type<U>::value)
        return "optional<" + native_type_name<typename optional_type<U>::type>() + ">";
    else if constexpr (std::is_enum_v<U>)
        return "enum<" + native_type_name<std::underlying_type_t<U>>() + ">";
    else
        static_assert(unsupported_type<U>,
                      "ExoML: unsupported native type; write an explicit host adapter");
}

template <class T> bare<T> from_value(const Value &value) {
    using U = bare<T>;
    static_assert(!std::is_reference_v<T> || (std::is_lvalue_reference_v<T> &&
                                              std::is_const_v<std::remove_reference_t<T>>),
                  "ExoML: mutable/rvalue reference arguments require an explicit adapter");
    if constexpr (std::is_same_v<U, Value>)
        return value;
    else if constexpr (optional_type<U>::value) {
        if (std::holds_alternative<std::monostate>(value))
            return std::nullopt;
        return U{from_value<typename optional_type<U>::type>(value)};
    } else if constexpr (std::is_same_v<U, bool>) {
        if (auto p = std::get_if<bool>(&value))
            return *p;
    } else if constexpr (std::is_enum_v<U>) {
        return static_cast<U>(from_value<std::underlying_type_t<U>>(value));
    } else if constexpr (std::is_integral_v<U> && sizeof(U) <= sizeof(std::uint64_t)) {
        if (auto p = std::get_if<std::int64_t>(&value)) {
            if constexpr (std::is_signed_v<U>) {
                if (*p >= std::numeric_limits<U>::min() && *p <= std::numeric_limits<U>::max())
                    return static_cast<U>(*p);
            } else if (*p >= 0 && static_cast<std::uint64_t>(*p) <= std::numeric_limits<U>::max())
                return static_cast<U>(*p);
            throw std::out_of_range("integer argument outside native range");
        }
        if (auto p = std::get_if<std::uint64_t>(&value)) {
            if (*p <= static_cast<std::uint64_t>(std::numeric_limits<U>::max()))
                return static_cast<U>(*p);
            throw std::out_of_range("unsigned argument outside native range");
        }
    } else if constexpr (std::is_same_v<U, float> || std::is_same_v<U, double>) {
        double number;
        if (auto p = std::get_if<double>(&value))
            number = *p;
        else if (auto p = std::get_if<std::int64_t>(&value)) {
            if (*p < -9007199254740992LL || *p > 9007199254740992LL)
                throw std::out_of_range("integer exceeds exact real conversion range");
            number = static_cast<double>(*p);
        } else if (auto p = std::get_if<std::uint64_t>(&value)) {
            if (*p > 9007199254740992ULL)
                throw std::out_of_range("integer exceeds exact real conversion range");
            number = static_cast<double>(*p);
        } else
            throw std::invalid_argument("expected real argument");
        if (!std::isfinite(number) || number < -std::numeric_limits<U>::max() ||
            number > std::numeric_limits<U>::max())
            throw std::out_of_range("real argument outside native range");
        return static_cast<U>(number);
    } else if constexpr (std::is_same_v<U, std::string>) {
        if (auto p = std::get_if<std::string>(&value))
            return *p;
    } else if constexpr (std::is_same_v<U, std::string_view>) {
        if (auto p = std::get_if<std::string>(&value))
            return *p;
    } else if constexpr (std::is_same_v<U, const char *>) {
        if (auto p = std::get_if<std::string>(&value)) {
            if (p->find('\0') != std::string::npos)
                throw std::invalid_argument("embedded NUL in C string argument");
            return p->c_str();
        }
    } else if constexpr (std::is_same_v<U, std::vector<std::string>>) {
        if (auto p = std::get_if<U>(&value))
            return *p;
    } else
        static_assert(unsupported_type<U>, "ExoML: unsupported native argument");
    throw std::invalid_argument("expected " + native_type_name<U>() + ", got " +
                                std::string(value_type(value)));
}

template <class T> Value to_value(T &&value, const NativeOptions &options) {
    using U = bare<T>;
    if constexpr (std::is_same_v<U, Value>)
        return std::forward<T>(value);
    else if constexpr (optional_type<U>::value) {
        if (!value)
            return std::monostate{};
        return to_value(*value, options);
    } else if constexpr (std::is_same_v<U, bool>)
        return value;
    else if constexpr (std::is_enum_v<U>)
        return to_value(static_cast<std::underlying_type_t<U>>(value), options);
    else if constexpr (std::is_integral_v<U> && sizeof(U) <= sizeof(std::uint64_t)) {
        if constexpr (std::is_signed_v<U>)
            return static_cast<std::int64_t>(value);
        else
            return static_cast<std::uint64_t>(value);
    } else if constexpr (std::is_same_v<U, double> || std::is_same_v<U, float>) {
        if (!std::isfinite(value))
            throw std::out_of_range("non-finite native result");
        return static_cast<double>(value);
    } else if constexpr (std::is_same_v<U, std::string> || std::is_same_v<U, std::string_view>)
        return std::string(value);
    else if constexpr (std::is_same_v<U, std::vector<std::string>>)
        return value;
    else if constexpr (std::is_same_v<U, const char *>) {
        if (!value)
            return std::monostate{};
        for (std::size_t length = 0; length < options.max_string_bytes; ++length)
            if (value[length] == '\0')
                return std::string(value, length);
        throw std::length_error("native C string result exceeds copy budget");
    } else
        static_assert(unsupported_type<U>, "ExoML: unsupported native result");
}

template <class T> struct traits : traits<decltype(&T::operator())> {};
template <class R, class... A> struct traits<R(A...)> {
    using result = R;
    using args = std::tuple<A...>;
};
template <class R, class... A> struct traits<R (*)(A...)> : traits<R(A...)> {};
template <class R, class... A> struct traits<R (*)(A...) noexcept> : traits<R(A...)> {};
template <class R, class... A> struct traits<std::function<R(A...)>> : traits<R(A...)> {};
template <class C, class R, class... A> struct traits<R (C::*)(A...) const> : traits<R(A...)> {};
template <class C, class R, class... A> struct traits<R (C::*)(A...)> : traits<R(A...)> {};
template <class C, class R, class... A>
struct traits<R (C::*)(A...) const noexcept> : traits<R(A...)> {};
template <class C, class R, class... A> struct traits<R (C::*)(A...) noexcept> : traits<R(A...)> {};

template <class T> constexpr bool has_c_string() {
    if constexpr (optional_type<bare<T>>::value)
        return has_c_string<typename optional_type<bare<T>>::type>();
    else
        return std::is_same_v<bare<T>, const char *>;
}
template <class F, std::size_t... I>
Interpreter::Registered make_native(F function, NativeOptions options, std::index_sequence<I...>) {
    using Signature = traits<F>;
    using Args = typename Signature::args;
    using Result = typename Signature::result;
    if constexpr (std::is_pointer_v<F>)
        if (!function)
            throw std::invalid_argument("null native function");
    if ((has_c_string<Result>() || (has_c_string<std::tuple_element_t<I, Args>>() || ...)) &&
        !options.c_strings)
        throw std::invalid_argument("C string bindings require the explicit c_strings contract");
    if (!options.ownership.consumed_arguments.empty() || options.ownership.returned_borrow ||
        options.ownership.may_escape)
        throw std::invalid_argument(
            "consumed, escaping or borrowed-result ownership requires an explicit adapter");
    for (auto index : options.ownership.borrowed_arguments)
        if (index >= sizeof...(I))
            throw std::invalid_argument("borrowed argument index out of range");
    for (auto effect : options.ownership.effects.effects)
        if (effect < Effect::Read || effect > Effect::Suspend)
            throw std::invalid_argument("unknown native effect");
    Interpreter::Registered service;
    service.privileged = options.privileged;
    service.info.arity = sizeof...(I);
    service.info.parameter_types = {native_type_name<std::tuple_element_t<I, Args>>()...};
    service.info.return_type = native_type_name<Result>();
    service.info.ownership = options.ownership;
    service.info.ownership.effects.effects.insert(Effect::ForeignCall);
    service.info.thread_affine = options.thread_affine;
    service.info.documentation = options.documentation;
    service.callback = [function = std::move(function), options = std::move(options)](
                           const std::vector<Value> &values) mutable -> Value {
        if (values.size() != sizeof...(I))
            throw std::invalid_argument("wrong native argument count");
        // Braced tuple construction makes validation/conversion left-to-right;
        // every argument is converted before native code is entered.
        std::tuple<bare<std::tuple_element_t<I, Args>>...> converted{
            from_value<std::tuple_element_t<I, Args>>(values[I])...};
        if constexpr (std::is_void_v<Result>) {
            std::apply(function, converted);
            return std::monostate{};
        } else
            return to_value(std::apply(function, converted), options);
    };
    return service;
}
} // namespace detail

template <class F> Interpreter::Registered native_service(F function, NativeOptions options = {}) {
    using Args = typename detail::traits<F>::args;
    return detail::make_native(std::move(function), std::move(options),
                               std::make_index_sequence<std::tuple_size_v<Args>>{});
}
template <class F>
void bind_native(Interpreter &interpreter, std::string name, F function,
                 NativeOptions options = {}) {
    Interpreter::Services services;
    services.emplace(std::move(name), native_service(std::move(function), std::move(options)));
    interpreter.register_services(std::move(services));
}

struct AutoBindOptions {
    std::string name_space;
    std::set<std::string> include, exclude;
    std::map<std::string, std::string> rename;
};
class StaticLibrary {
    std::string name_;
    Interpreter::Services services_;

  public:
    explicit StaticLibrary(std::string name) : name_(std::move(name)) {
        if (!valid_identifier(name_))
            throw std::invalid_argument("invalid static library name");
    }
    template <class F>
    StaticLibrary &bind(std::string name, F function, NativeOptions options = {}) {
        if (!valid_identifier(name))
            throw std::invalid_argument("invalid export name");
        if (!services_.emplace(name, native_service(std::move(function), std::move(options)))
                 .second)
            throw std::invalid_argument("duplicate export: " + name);
        return *this;
    }
    StaticLibrary &constant(std::string name, Value value) {
        NativeOptions options;
        options.privileged = false;
        return bind(std::move(name), [value = std::move(value)] { return value; }, options);
    }
    StaticLibrary &alias(std::string name, const std::string &original) {
        if (!valid_identifier(name))
            throw std::invalid_argument("invalid alias name");
        auto found = services_.find(original);
        if (found == services_.end())
            throw std::invalid_argument("unknown alias target: " + original);
        if (!services_.emplace(std::move(name), found->second).second)
            throw std::invalid_argument("duplicate alias");
        return *this;
    }
    const Interpreter::Services &exports() const noexcept { return services_; }
    void install(Interpreter &interpreter, AutoBindOptions options = {}) const {
        auto check = [&](const std::string &name) {
            if (!services_.count(name))
                throw std::invalid_argument("unknown export selection: " + name);
        };
        for (const auto &name : options.include)
            check(name);
        for (const auto &name : options.exclude)
            check(name);
        for (const auto &[name, renamed] : options.rename) {
            check(name);
            if (!valid_identifier(renamed))
                throw std::invalid_argument("invalid renamed export");
            if (options.exclude.count(name) ||
                (!options.include.empty() && !options.include.count(name)))
                throw std::invalid_argument("rename selects an excluded export");
        }
        auto prefix = options.name_space.empty() ? name_ : options.name_space;
        if (!valid_service_name(prefix))
            throw std::invalid_argument("invalid binding namespace");
        Interpreter::Services batch;
        for (const auto &[name, service] : services_) {
            if ((!options.include.empty() && !options.include.count(name)) ||
                options.exclude.count(name))
                continue;
            const auto &exported = options.rename.count(name) ? options.rename.at(name) : name;
            if (!batch.emplace(prefix + ':' + exported, service).second)
                throw std::invalid_argument("duplicate renamed export");
        }
        interpreter.register_services(std::move(batch));
    }
};
} // namespace moonstone::exoml
