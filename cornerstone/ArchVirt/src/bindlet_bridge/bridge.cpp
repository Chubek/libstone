#include "bridge.hpp"
#include "shared_memory.hpp"
#include "lazy_register.hpp"
#include <Python.h>
#include <cstring>
#include <string>

namespace domvirt::bindlet_bridge {

Bridge* g_bridge = nullptr;

Bridge::Bridge()
    : shared_mem_(std::make_unique<SharedMemoryView>())
    , lazy_regs_(std::make_unique<LazyRegisterProxy>())
    , buffer_mgr_(std::make_unique<GuestBufferManager>()) {}

Bridge::~Bridge() {
    shutdown();
}

VmResult Bridge::initialize() {
    if (initialized_) return {VmErrorCode::SUCCESS, "Already initialized"};

    if (!Py_IsInitialized()) {
        Py_Initialize();
        if (!Py_IsInitialized()) {
            return {VmErrorCode::BRIDGE_ERROR, "Failed to initialize Python"};
        }
    }

    PyRun_SimpleString("import sys; sys.path.insert(0, '')");

    initialized_ = true;
    g_bridge = this;
    return {VmErrorCode::SUCCESS, "Bridge initialized"};
}

void Bridge::shutdown() {
    if (!initialized_) return;
    shared_mem_->unregister_all();
    if (Py_IsInitialized()) {
        Py_FinalizeEx();
    }
    if (g_bridge == this) g_bridge = nullptr;
    initialized_ = false;
}

VmResult Bridge::execute_python(const std::string& module,
                                const std::string& function,
                                const std::string& json_args,
                                std::string& json_result) {
    if (!initialized_) {
        return {VmErrorCode::BRIDGE_ERROR, "Bridge not initialized"};
    }

    PyGILState_STATE gstate = PyGILState_Ensure();

    PyObject* pModule = PyImport_ImportModule(module.c_str());
    if (!pModule) {
        PyErr_Print();
        PyGILState_Release(gstate);
        return {VmErrorCode::BRIDGE_ERROR,
                "Failed to import module: " + module};
    }

    PyObject* pFunc = PyObject_GetAttrString(pModule, function.c_str());
    Py_DECREF(pModule);
    if (!pFunc || !PyCallable_Check(pFunc)) {
        Py_XDECREF(pFunc);
        PyErr_Print();
        PyGILState_Release(gstate);
        return {VmErrorCode::BRIDGE_ERROR,
                "Function not callable: " + function};
    }

    PyObject* pArgs = PyTuple_Pack(1, PyUnicode_FromString(json_args.c_str()));
    PyObject* pResult = PyObject_CallObject(pFunc, pArgs);
    Py_DECREF(pArgs);

    if (!pResult) {
        PyErr_Print();
        Py_DECREF(pFunc);
        PyGILState_Release(gstate);
        return {VmErrorCode::BRIDGE_ERROR, "Python call failed"};
    }

    Py_DECREF(pFunc);

    if (PyUnicode_Check(pResult)) {
        json_result = PyUnicode_AsUTF8(pResult);
    } else {
        PyObject* pStr = PyObject_Str(pResult);
        if (pStr) {
            json_result = PyUnicode_AsUTF8(pStr);
            Py_DECREF(pStr);
        }
    }
    Py_DECREF(pResult);

    PyGILState_Release(gstate);
    return {VmErrorCode::SUCCESS, ""};
}

VmResult Bridge::register_callback(BridgeEventType type, BridgeCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callbacks_.emplace_back(type, std::move(cb));
    return {VmErrorCode::SUCCESS, "Callback registered"};
}

VmResult Bridge::dispatch_event(const BridgeEvent& event) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    for (auto& [type, cb] : callbacks_) {
        if (type == event.type) {
            return cb(event);
        }
    }
    return {VmErrorCode::SUCCESS, ""};
}

void Bridge::acquire_gil() {
    PyGILState_Ensure();
}

void Bridge::release_gil() {
    if (PyGILState_Check()) {
        PyGILState_Release(PyGILState_UNLOCKED);
    }
}

bool Bridge::gil_held() const noexcept {
    return PyGILState_Check() != 0;
}

} // namespace domvirt::bindlet_bridge
