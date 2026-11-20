#define PY_SSIZE_T_CLEAN
#include <Python.h>

#ifdef Py_GIL_DISABLED
#error "Archon requires a CPython build with the GIL"
#endif

#include <archon/Archon-IRGen.hpp>

#include <limits>
#include <utility>

namespace archon {
namespace {

thread_local const std::string* package_prefix = nullptr;

class GIL {
public:
    GIL() {
        if (!Py_IsInitialized())
            throw Error("initialize a CPython main interpreter before using Archon");
        state_ = PyGILState_Ensure();
    }
    ~GIL() { PyGILState_Release(state_); }
    GIL(const GIL&) = delete;
    GIL& operator=(const GIL&) = delete;

private:
    PyGILState_STATE state_;
};

// Local refs are always nested inside a GIL scope.
class Ref {
public:
    explicit Ref(PyObject* object) noexcept : object_(object) {}
    ~Ref() { Py_XDECREF(object_); }
    Ref(const Ref&) = delete;
    Ref& operator=(const Ref&) = delete;
    PyObject* get() const noexcept { return object_; }

private:
    PyObject* object_;
};

[[noreturn]] void python_error() {
#if PY_VERSION_HEX >= 0x030C0000
    Ref value(PyErr_GetRaisedException());
    const char* type = value.get() ? Py_TYPE(value.get())->tp_name : "PythonError";
#else
    PyObject *raw_type = nullptr, *raw_value = nullptr, *raw_traceback = nullptr;
    PyErr_Fetch(&raw_type, &raw_value, &raw_traceback);
    PyErr_NormalizeException(&raw_type, &raw_value, &raw_traceback);
    Ref error_type(raw_type), value(raw_value), traceback(raw_traceback);
    const char* type = value.get() ? Py_TYPE(value.get())->tp_name : "PythonError";
#endif
    Ref text(value.get() ? PyObject_Str(value.get()) : nullptr);
    Py_ssize_t size = 0;
    const char* utf8 = text.get() ? PyUnicode_AsUTF8AndSize(text.get(), &size) : nullptr;
    if (!utf8)
        PyErr_Clear();
    throw Error(utf8 ? std::string(utf8, static_cast<std::size_t>(size))
                     : "CPython operation failed", type);
}

PyObject* checked(PyObject* object) {
    if (!object)
        python_error();
    return object;
}

Py_ssize_t length(std::size_t size) {
    if (size > static_cast<std::size_t>(std::numeric_limits<Py_ssize_t>::max()))
        throw Error("value is too large for CPython");
    return static_cast<Py_ssize_t>(size);
}

PyObject* unicode(std::string_view text) {
    return checked(PyUnicode_DecodeUTF8(text.empty() ? "" : text.data(), length(text.size()),
                                       "strict"));
}

std::string utf8(PyObject* object) {
    if (!PyUnicode_Check(object))
        throw Error("expected a Python str");
    Py_ssize_t size;
    const auto* data = PyUnicode_AsUTF8AndSize(object, &size);
    if (!data)
        python_error();
    return std::string(data, static_cast<std::size_t>(size));
}

std::string_view binary(PyObject* object) {
    if (PyBytes_Check(object))
        return {PyBytes_AS_STRING(object), static_cast<std::size_t>(PyBytes_GET_SIZE(object))};
    if (PyByteArray_Check(object))
        return {PyByteArray_AS_STRING(object),
                static_cast<std::size_t>(PyByteArray_GET_SIZE(object))};
    throw Error("expected Python bytes or bytearray");
}

void require_integer(PyObject* object) {
    if (!PyLong_Check(object) || PyBool_Check(object))
        throw Error("expected a Python int, not a bool or another type");
}

void release(PyObject* object) noexcept {
    if (object && Py_IsInitialized()) {
        const auto state = PyGILState_Ensure();
        Py_DECREF(object);
        PyGILState_Release(state);
    }
}

} // namespace

Error::Error(std::string message, std::string python_type)
    : std::runtime_error(std::move(message)), python_type_(std::move(python_type)) {}

Interpreter::Interpreter(std::string_view package_root) : state_(nullptr) {
    if (Py_IsInitialized())
        throw Error("CPython is already initialized; use the existing main interpreter");
    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    config.site_import = 0;  // No implicit .pth/startup code in the owned interpreter.
    const auto status = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    if (PyStatus_Exception(status))
        throw Error(status.err_msg ? status.err_msg : "CPython initialization failed");
    try {
        if (!package_root.empty())
            add_package_path(package_root);
    } catch (...) {
        Py_FinalizeEx();
        throw;
    }
    state_ = PyEval_SaveThread();
}

Interpreter::~Interpreter() noexcept {
    PyEval_RestoreThread(state_);
    Py_FinalizeEx();
}

void add_package_path(std::string_view package_root) {
    if (package_root.empty() || package_root.find('\0') != std::string_view::npos)
        throw Error("expected a nonempty Archon package directory without NUL bytes");
    GIL gil;
    Ref path(unicode(package_root));
    auto* sys_path = PySys_GetObject("path");  // borrowed
    if (!sys_path || !PyList_Check(sys_path))
        throw Error("CPython sys.path is not a list");
    if (PyList_Insert(sys_path, 0, path.get()) < 0)
        python_error();
}

PackageScope::PackageScope(std::string_view parent_package)
    : prefix_(parent_package), previous_(package_prefix) {
    const auto head = [](char c) {
        return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    };
    if (prefix_.empty() || !head(prefix_.front()))
        throw Error("expected an application Python package identifier");
    for (const char c : prefix_)
        if (!head(c) && !(c >= '0' && c <= '9'))
            throw Error("expected an application Python package identifier");
    prefix_ += '.';
    package_prefix = &prefix_;
}

PackageScope::~PackageScope() noexcept { package_prefix = previous_; }

namespace detail {

struct Access {
    static PyObject* get(const Value& value) noexcept {
        return value.object_ ? value.object_ : Py_None;
    }
    static Value take(PyObject* object) {
        Value value;
        value.object_ = checked(object);
        return value;
    }
    static Value share(PyObject* object) {
        Py_INCREF(object);
        return take(object);
    }
};

} // namespace detail

namespace {

using detail::Access;

Value dictionary(const Object& entries) {
    auto value = Access::take(PyDict_New());
    for (const auto& entry : entries) {
        Ref key(unicode(entry.first));
        if (PyDict_SetItem(Access::get(value), key.get(), Access::get(entry.second)) < 0)
            python_error();
    }
    return value;
}

Value invoke_api(const char* module_name, std::string_view operation,
                 const List& args, const Object& kwargs) {
    GIL gil;
    const auto qualified = package_prefix ? *package_prefix + module_name : std::string{};
    if (package_prefix)
        module_name = qualified.c_str();
    auto module = Access::take(PyImport_ImportModule(module_name));
    return module.attribute(operation).call(args, kwargs);
}

} // namespace

Value::Value(bool value) {
    GIL gil;
    object_ = checked(PyBool_FromLong(value));
}

Value::Value(double value) {
    GIL gil;
    object_ = checked(PyFloat_FromDouble(value));
}

Value::Value(const char* value) : Value(value ? std::string_view(value) : std::string_view()) {
    if (!value)
        throw Error("cannot convert a null C string to a Python str");
}

Value::Value(std::string_view value) {
    GIL gil;
    object_ = unicode(value);
}

Value::Value(const List& value) {
    GIL gil;
    auto array = Access::take(PyList_New(length(value.size())));
    for (std::size_t i = 0; i < value.size(); ++i) {
        auto* item = Access::get(value[i]);
        Py_INCREF(item);
        PyList_SET_ITEM(Access::get(array), static_cast<Py_ssize_t>(i), item);
    }
    swap(array);
}

Value::Value(const Object& value) {
    GIL gil;
    auto mapping = dictionary(value);
    swap(mapping);
}

Value Value::from_signed(std::int64_t value) {
    GIL gil;
    return Access::take(PyLong_FromLongLong(value));
}

Value Value::from_unsigned(std::uint64_t value) {
    GIL gil;
    return Access::take(PyLong_FromUnsignedLongLong(value));
}

Value Value::from_bytes(const Bytes& value) {
    GIL gil;
    const auto* bytes = value.empty() ? "" : reinterpret_cast<const char*>(value.data());
    return Access::take(PyBytes_FromStringAndSize(bytes, length(value.size())));
}

Value Value::integer(std::string_view decimal) {
    GIL gil;
    Ref text(unicode(decimal));
    return Access::take(PyLong_FromUnicodeObject(text.get(), 10));
}

Value::Value(const Value& other) : object_(other.object_) {
    if (object_) {
        GIL gil;
        Py_INCREF(object_);
    }
}

Value::Value(Value&& other) noexcept : object_(std::exchange(other.object_, nullptr)) {}

Value& Value::operator=(const Value& other) {
    Value copy(other);
    swap(copy);
    return *this;
}

Value& Value::operator=(Value&& other) noexcept {
    Value moved(std::move(other));
    swap(moved);
    return *this;
}

Value::~Value() noexcept { release(object_); }

void Value::swap(Value& other) noexcept { std::swap(object_, other.object_); }

bool Value::is_none() const {
    if (!object_)
        return true;
    GIL gil;
    return Access::get(*this) == Py_None;
}

bool Value::as_bool() const {
    GIL gil;
    if (!PyBool_Check(Access::get(*this)))
        throw Error("expected a Python bool");
    return Access::get(*this) == Py_True;
}

std::int64_t Value::as_int64() const {
    GIL gil;
    auto* object = Access::get(*this);
    require_integer(object);
    const auto value = PyLong_AsLongLong(object);
    if (value == -1 && PyErr_Occurred())
        python_error();
    return value;
}

std::uint64_t Value::as_uint64() const {
    GIL gil;
    auto* object = Access::get(*this);
    require_integer(object);
    const auto value = PyLong_AsUnsignedLongLong(object);
    if (value == static_cast<unsigned long long>(-1) && PyErr_Occurred())
        python_error();
    return value;
}

std::string Value::as_integer_string() const {
    GIL gil;
    auto* object = Access::get(*this);
    require_integer(object);
    Ref text(checked(PyObject_Str(object)));
    return utf8(text.get());
}

double Value::as_double() const {
    GIL gil;
    if (!PyFloat_Check(Access::get(*this)))
        throw Error("expected a Python float");
    return PyFloat_AS_DOUBLE(Access::get(*this));
}

std::string Value::as_string() const {
    GIL gil;
    return utf8(Access::get(*this));
}

std::string Value::as_path() const {
    GIL gil;
    Ref path(checked(PyOS_FSPath(Access::get(*this))));
    if (PyBytes_Check(path.get()))
        return std::string(binary(path.get()));
    Ref encoded(checked(PyUnicode_EncodeFSDefault(path.get())));
    return std::string(binary(encoded.get()));
}

Bytes Value::as_bytes() const {
    GIL gil;
    const auto bytes = binary(Access::get(*this));
    return Bytes(bytes.begin(), bytes.end());
}

List Value::as_list() const {
    GIL gil;
    auto* object = Access::get(*this);
    const bool is_list = PyList_Check(object);
    if (!is_list && !PyTuple_Check(object))
        throw Error("expected a Python list or tuple");
    const auto size = is_list ? PyList_GET_SIZE(object) : PyTuple_GET_SIZE(object);
    List result;
    result.reserve(static_cast<std::size_t>(size));
    for (Py_ssize_t i = 0; i < size; ++i)
        result.push_back(Access::share(is_list ? PyList_GET_ITEM(object, i)
                                               : PyTuple_GET_ITEM(object, i)));
    return result;
}

Object Value::as_object() const {
    GIL gil;
    auto* object = Access::get(*this);
    if (!PyDict_Check(object))
        throw Error("expected a Python dict");
    Object result;
    Py_ssize_t pos = 0;
    PyObject *key, *value;
    while (PyDict_Next(object, &pos, &key, &value))
        result.emplace(utf8(key), Access::share(value));
    return result;
}

std::size_t Value::size() const {
    GIL gil;
    const auto size = PyObject_Size(Access::get(*this));
    if (size < 0)
        python_error();
    return static_cast<std::size_t>(size);
}

Value Value::at(std::string_view key) const {
    GIL gil;
    Ref name(unicode(key));
    return Access::take(PyObject_GetItem(Access::get(*this), name.get()));
}

Value Value::at(std::size_t index) const {
    GIL gil;
    Ref key(checked(PyLong_FromSize_t(index)));
    return Access::take(PyObject_GetItem(Access::get(*this), key.get()));
}

Value Value::attribute(std::string_view name) const {
    if (name.empty() || name.front() == '_' || name.find('\0') != std::string_view::npos)
        throw Error("expected a public Python attribute name");
    GIL gil;
    Ref key(unicode(name));
    return Access::take(PyObject_GetAttr(Access::get(*this), key.get()));
}

Value Value::call(const List& args, const Object& kwargs) const {
    GIL gil;
    auto* callable = Access::get(*this);
    if (!PyCallable_Check(callable))
        throw Error("Python value is not callable");
    Ref positional(checked(PyTuple_New(length(args.size()))));
    for (std::size_t i = 0; i < args.size(); ++i) {
        auto* item = Access::get(args[i]);
        Py_INCREF(item);
        PyTuple_SET_ITEM(positional.get(), static_cast<Py_ssize_t>(i), item);
    }
    auto keywords = kwargs.empty() ? Value() : dictionary(kwargs);
    return Access::take(PyObject_Call(callable, positional.get(),
                                      kwargs.empty() ? nullptr : Access::get(keywords)));
}

Value get_export(std::string_view name) {
    return invoke_api("archon.api", "get_export", {name}, {});
}

Value dispatch(std::string_view name, const List& args, const Object& kwargs) {
    return get_export(name).call(args, kwargs);
}

std::vector<std::string> exports() {
    return detail::strings(invoke_api("archon.api", "exports", {}, {}));
}

MachineState::MachineState(Value value) : value_(std::move(value)) {}

namespace irgen {

Value get_export(std::string_view name) {
    return invoke_api("archon.irgen.api", "get_export", {name}, {});
}

Value dispatch(std::string_view name, const List& args, const Object& kwargs) {
    return irgen::get_export(name).call(args, kwargs);
}

std::vector<std::string> exports() {
    return detail::strings(invoke_api("archon.irgen.api", "exports", {}, {}));
}

} // namespace irgen

} // namespace archon
