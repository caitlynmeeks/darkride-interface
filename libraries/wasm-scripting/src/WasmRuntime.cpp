//
//  WasmRuntime.cpp
//  libraries/wasm-scripting/src
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "WasmRuntime.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <thread>
#include <type_traits>

#include <wasm.h>
#include <wasmtime.h>
#include <wasmtime/profiling.h>

#include <QtCore/QHash>
#include <QtCore/QLoggingCategory>

#include <ScriptCaller.h>

Q_LOGGING_CATEGORY(wasmScripting, "hifi.wasm-scripting")

namespace {

// Takes the error's text and frees it.
QString takeErrorText(wasmtime_error_t* error) {
    if (!error) {
        return QString();
    }
    wasm_name_t message;
    wasmtime_error_message(error, &message);
    QString text = QString::fromUtf8(message.data, static_cast<int>(message.size));
    wasm_byte_vec_delete(&message);
    wasmtime_error_delete(error);
    return text;
}

// Takes the trap's text and frees it.
QString takeTrapText(wasm_trap_t* trap) {
    if (!trap) {
        return QString();
    }
    wasm_message_t message;
    wasm_trap_message(trap, &message);
    QString text = QString::fromUtf8(message.data); // null terminated
    wasm_byte_vec_delete(&message);
    wasm_trap_delete(trap);
    return text;
}

wasm_valkind_t toValKind(WasmType type) {
    switch (type) {
        case WasmType::I32: return WASM_I32;
        case WasmType::I64: return WASM_I64;
        case WasmType::F32: return WASM_F32;
        case WasmType::F64: return WASM_F64;
    }
    return WASM_I32;
}

// A function type for the linker; the caller deletes it.
wasm_functype_t* makeFunctionType(const WasmSignature& signature) {
    std::vector<wasm_valtype_t*> params, results;
    for (auto type : signature.params) {
        params.push_back(wasm_valtype_new(toValKind(type)));
    }
    for (auto type : signature.results) {
        results.push_back(wasm_valtype_new(toValKind(type)));
    }
    wasm_valtype_vec_t paramVec, resultVec;
    if (params.empty()) {
        wasm_valtype_vec_new_empty(&paramVec);
    } else {
        wasm_valtype_vec_new(&paramVec, params.size(), params.data());
    }
    if (results.empty()) {
        wasm_valtype_vec_new_empty(&resultVec);
    } else {
        wasm_valtype_vec_new(&resultVec, results.size(), results.data());
    }
    return wasm_functype_new(&paramVec, &resultVec); // takes the vectors
}

WasmValue fromVal(const wasmtime_val_t& value) {
    switch (value.kind) {
        case WASMTIME_I32: return value.of.i32;
        case WASMTIME_I64: return value.of.i64;
        case WASMTIME_F32: return value.of.f32;
        case WASMTIME_F64: return value.of.f64;
        default: return int32_t(0);
    }
}

wasmtime_val_t toVal(const WasmValue& value) {
    wasmtime_val_t out {};
    std::visit([&out](auto v) {
        using T = decltype(v);
        if constexpr (std::is_same_v<T, int32_t>) {
            out.kind = WASMTIME_I32;
            out.of.i32 = v;
        } else if constexpr (std::is_same_v<T, int64_t>) {
            out.kind = WASMTIME_I64;
            out.of.i64 = v;
        } else if constexpr (std::is_same_v<T, float>) {
            out.kind = WASMTIME_F32;
            out.of.f32 = v;
        } else {
            out.kind = WASMTIME_F64;
            out.of.f64 = v;
        }
    }, value);
    return out;
}

QString nameToString(const wasm_name_t* name) {
    return QString::fromUtf8(name->data, static_cast<int>(name->size));
}

} // namespace

// ---------------------------------------------------------------------------------------------------------
// WasmRuntime

struct WasmRuntime::Impl {
    wasm_engine_t* engine { nullptr };              // epoch interruption: a module from anywhere
    wasm_engine_t* cooperativeEngine { nullptr };   // no epoch checks: a module that asks whether to stop
    std::thread ticker;
    std::atomic<bool> stopTicker { false };
};

WasmRuntime::WasmRuntime() : WasmRuntime(Settings()) {
}

WasmRuntime::WasmRuntime(const Settings& settings) : _settings(settings), _impl(new Impl) {
    wasm_config_t* config = wasm_config_new();
    wasmtime_config_epoch_interruption_set(config, settings.epochInterruption);
    wasmtime_config_cranelift_opt_level_set(config, WASMTIME_OPT_LEVEL_SPEED);
    _impl->engine = wasm_engine_new_with_config(config); // takes the config
    // A module compiled by one engine runs only on that engine's stores, so the modules that stop themselves get an
    // engine of their own, without the checks.
    wasm_config_t* cooperativeConfig = wasm_config_new();
    wasmtime_config_epoch_interruption_set(cooperativeConfig, false);
    wasmtime_config_cranelift_opt_level_set(cooperativeConfig, WASMTIME_OPT_LEVEL_SPEED);
    _impl->cooperativeEngine = wasm_engine_new_with_config(cooperativeConfig);
    // The epoch is the clock every instance's time limit is measured in; one thread advances it for all.
    wasm_engine_t* engine = _impl->engine;
    auto tick = settings.epochTick;
    std::atomic<bool>* stop = &_impl->stopTicker;
    _impl->ticker = std::thread([engine, tick, stop] {
        while (!stop->load()) {
            std::this_thread::sleep_for(tick);
            wasmtime_engine_increment_epoch(engine);
        }
    });
}

WasmRuntime::~WasmRuntime() {
    _impl->stopTicker = true;
    _impl->ticker.join();
    wasm_engine_delete(_impl->engine);
    wasm_engine_delete(_impl->cooperativeEngine);
}

struct WasmModule::Impl {
    wasmtime_module_t* module { nullptr };
    wasm_engine_t* engine { nullptr };
    WasmRuntime::Interruption interruption { WasmRuntime::Interruption::Epoch };
};

std::shared_ptr<WasmModule> WasmRuntime::compile(const QByteArray& wasm, QString& error, Interruption interruption) {
    wasm_engine_t* engine = interruption == Interruption::Cooperative ? _impl->cooperativeEngine : _impl->engine;
    wasmtime_module_t* module = nullptr;
    auto result = wasmtime_module_new(engine, reinterpret_cast<const uint8_t*>(wasm.constData()),
                                      static_cast<size_t>(wasm.size()), &module);
    if (result) {
        error = takeErrorText(result);
        return nullptr;
    }
    auto impl = std::make_unique<WasmModule::Impl>();
    impl->module = module;
    impl->engine = engine;
    impl->interruption = interruption;
    return std::shared_ptr<WasmModule>(new WasmModule(shared_from_this(), std::move(impl)));
}

std::shared_ptr<WasmModule> WasmRuntime::compileText(const QString& text, QString& error, Interruption interruption) {
    QByteArray utf8 = text.toUtf8();
    wasm_byte_vec_t binary;
    auto result = wasmtime_wat2wasm(utf8.constData(), static_cast<size_t>(utf8.size()), &binary);
    if (result) {
        error = takeErrorText(result);
        return nullptr;
    }
    QByteArray bytes(binary.data, static_cast<int>(binary.size));
    wasm_byte_vec_delete(&binary);
    return compile(bytes, error, interruption);
}

// ---------------------------------------------------------------------------------------------------------
// WasmModule

WasmModule::WasmModule(std::shared_ptr<WasmRuntime> runtime, std::unique_ptr<Impl> impl) :
    _runtime(std::move(runtime)), _impl(std::move(impl)) {
}

WasmModule::~WasmModule() {
    wasmtime_module_delete(_impl->module);
}

WasmRuntime::Interruption WasmModule::interruption() const {
    return _impl->interruption;
}

QStringList WasmModule::imports() const {
    QStringList names;
    wasm_importtype_vec_t imports;
    wasmtime_module_imports(_impl->module, &imports);
    for (size_t i = 0; i < imports.size; ++i) {
        names << nameToString(wasm_importtype_module(imports.data[i])) + "." + nameToString(wasm_importtype_name(imports.data[i]));
    }
    wasm_importtype_vec_delete(&imports);
    return names;
}

QStringList WasmModule::exports() const {
    QStringList names;
    wasm_exporttype_vec_t exports;
    wasmtime_module_exports(_impl->module, &exports);
    for (size_t i = 0; i < exports.size; ++i) {
        names << nameToString(wasm_exporttype_name(exports.data[i]));
    }
    wasm_exporttype_vec_delete(&exports);
    return names;
}

// ---------------------------------------------------------------------------------------------------------
// WasmInstance

namespace {

struct HostFunctionEntry {
    WasmInstance* instance;
    WasmHostFunction function;
    WasmSignature signature;
};

} // namespace

struct WasmInstance::Impl {
    wasmtime_store_t* store { nullptr };
    wasmtime_context_t* context { nullptr };
    wasmtime_linker_t* linker { nullptr };
    wasmtime_instance_t instance {};
    bool instantiated { false };
    bool wasi { false };
    bool hasMemory { false };
    wasmtime_memory_t memory {};
    std::vector<std::unique_ptr<HostFunctionEntry>> hostFunctions;
    std::atomic<bool> stopRequested { false };
    // requestStopAfter: the running call may finish until the deadline (steady clock, ns; 0: none) or until the world
    // is left; no new outermost call begins meanwhile (closed)
    std::atomic<int64_t> stopDeadline { 0 };
    std::atomic<int> stopWorld { 0 };
    std::atomic<bool> closed { false };
    std::chrono::steady_clock::time_point callStarted;
    std::chrono::milliseconds callTimeout { 0 };
    int callDepth { 0 };
    // The module's own time in the outermost call: the clock runs while the module runs and pauses while a host
    // function does (the trampoline), so a slow host call does not count against the limit.
    bool inGuest { false };
    std::chrono::steady_clock::time_point guestSince;
    std::chrono::nanoseconds guestTime { 0 };
    bool limitReported { false };  // the time limit is reported once per outermost call
    std::chrono::milliseconds callWarning { 0 };
    bool warningReported { false };  // and the warning once per outermost call
    std::function<void(std::chrono::milliseconds)> warningHandler;
    wasmtime_guestprofiler_t* profiler { nullptr };
    std::chrono::steady_clock::time_point lastSample;
    QString interruptionReason; // why a cooperative module was last told to stop, until taken

    // An exported function, found by name once: the host calls a few of them (alloc, the entry points) many times
    // a frame, and looking one up builds its type each time.
    struct ExportedFunction {
        wasmtime_func_t function;
        size_t paramCount;
        size_t resultCount;
    };
    QHash<QString, ExportedFunction> exportedFunctions;

    // The outermost call sets the deadline one tick out and starts the clock; at each tick the runtime asks
    // whether to go on. A call made from inside a host function (reentrant) runs under the outer call's limit.
    void beginCall() {
        if (callDepth == 0) {
            callStarted = std::chrono::steady_clock::now();
            lastSample = callStarted;
            guestTime = std::chrono::nanoseconds(0);
            inGuest = false;
            limitReported = false;
            warningReported = false;
            wasmtime_context_set_epoch_deadline(context, 1);
        }
    }

    // The module starts or resumes running (a call into it, a host function returning to it).
    void enterGuest() {
        if (!inGuest) {
            inGuest = true;
            guestSince = std::chrono::steady_clock::now();
        }
    }
    // The module stops running for now (a host function entered, a call into it returned).
    void leaveGuest() {
        if (inGuest) {
            guestTime += std::chrono::steady_clock::now() - guestSince;
            inGuest = false;
        }
    }
    std::chrono::milliseconds guestElapsed() const {
        std::chrono::nanoseconds total = guestTime;
        if (inGuest) {
            total += std::chrono::steady_clock::now() - guestSince;
        }
        return std::chrono::duration_cast<std::chrono::milliseconds>(total);
    }

    template <typename Function>
    QString withCallerContext(wasmtime_caller_t* caller, Function function) {
        wasmtime_context_t* outer = context;
        context = wasmtime_caller_context(caller);
        QString result = function();
        context = outer;
        return result;
    }

    // A profile sample when one is due: at every epoch tick, or as often as a cooperative module asks to stop.
    void sample(std::chrono::nanoseconds interval) {
        auto now = std::chrono::steady_clock::now();
        auto spent = std::chrono::duration_cast<std::chrono::nanoseconds>(now - lastSample);
        if (spent >= interval) {
            lastSample = now;
            wasmtime_guestprofiler_sample(profiler, store, uint64_t(spent.count()));
        }
    }

    // The warning of a call that has run past callWarning, once per outermost call; the call goes on.
    void checkWarning() {
        if (callWarning.count() > 0 && !warningReported) {
            const std::chrono::milliseconds elapsed = guestElapsed();
            if (elapsed > callWarning) {
                warningReported = true;
                if (warningHandler) {
                    warningHandler(elapsed);
                }
            }
        }
    }

    static int64_t steadyNow() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Whether the module must stop now: requestStop, or requestStopAfter's deadline or world. Only a stop that waits
    // costs more than a load.
    bool stopDue() const {
        if (stopRequested.load()) {
            return true;
        }
        const int64_t deadline = stopDeadline.load();
        return deadline != 0 && (steadyNow() >= deadline || ScriptCaller::isRetired(stopWorld.load()));
    }

    // Why the running call must stop now, or nothing.
    QString interruption() const {
        if (stopDue()) {
            return "the script was stopped";
        }
        if (callTimeout.count() > 0 && guestElapsed() > callTimeout) {
            return QString("a call into the script ran longer than its limit of %1 ms").arg(callTimeout.count());
        }
        return QString();
    }

    static wasmtime_error_t* onDeadline(wasmtime_context_t* context, void* data, uint64_t* delta,
                                        wasmtime_update_deadline_kind_t* kind) {
        Q_UNUSED(context);
        auto self = static_cast<Impl*>(data);
        if (self->profiler) {
            self->sample(std::chrono::nanoseconds(0));
        }
        self->checkWarning();
        QString reason = self->interruption();
        if (!reason.isEmpty()) {
            QByteArray text = reason.toUtf8();
            return wasmtime_error_new(text.constData());
        }
        *delta = 1;
        *kind = WASMTIME_UPDATE_DEADLINE_CONTINUE;
        return nullptr;
    }

    bool findExport(const QString& name, wasmtime_extern_t& item) const {
        QByteArray utf8 = name.toUtf8();
        return instantiated && wasmtime_instance_export_get(context, &instance, utf8.constData(), static_cast<size_t>(utf8.size()), &item);
    }

    const ExportedFunction* findFunction(const QString& name) {
        auto found = exportedFunctions.constFind(name);
        if (found != exportedFunctions.constEnd()) {
            return &found.value();
        }
        wasmtime_extern_t item;
        if (!findExport(name, item) || item.kind != WASMTIME_EXTERN_FUNC) {
            return nullptr;
        }
        ExportedFunction exported;
        exported.function = item.of.func;
        wasm_functype_t* type = wasmtime_func_type(context, &exported.function);
        exported.paramCount = wasm_functype_params(type)->size;
        exported.resultCount = wasm_functype_results(type)->size;
        wasm_functype_delete(type);
        return &exportedFunctions.insert(name, exported).value();
    }
};

namespace {

// The argument and result vectors of the host functions running on this thread, one pair per nesting depth (a host
// function can call into the module, which can call a host function again), kept so that a call allocates nothing.
// A deque, so that a deeper call adding a pair moves none of the outer calls' pairs.
struct TrampolineBuffers {
    std::vector<WasmValue> in;
    std::vector<WasmValue> out;
};
thread_local std::deque<TrampolineBuffers> trampolineBuffers;
thread_local size_t trampolineDepth = 0;

struct TrampolineScope {
    TrampolineBuffers& buffers;
    TrampolineScope() : buffers(take()) {}
    ~TrampolineScope() { --trampolineDepth; }
    static TrampolineBuffers& take() {
        if (trampolineBuffers.size() <= trampolineDepth) {
            trampolineBuffers.emplace_back();
        }
        TrampolineBuffers& buffers = trampolineBuffers[trampolineDepth++];
        buffers.in.clear();
        buffers.out.clear();
        return buffers;
    }
};

// What the module calls when it calls a host function.
wasm_trap_t* hostFunctionTrampoline(void* env, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t argCount,
                                    wasmtime_val_t* results, size_t resultCount) {
    auto entry = static_cast<HostFunctionEntry*>(env);
    TrampolineScope scope;
    std::vector<WasmValue>& in = scope.buffers.in;
    for (size_t i = 0; i < argCount; ++i) {
        in.push_back(fromVal(args[i]));
    }
    std::vector<WasmValue>& out = scope.buffers.out;
    // While the host function runs, the instance works through the caller's context: a call back into the
    // module (the guest's alloc, a callback) and its memory accesses go through it, as wasmtime requires.
    WasmInstance::Impl& impl = entry->instance->impl();
    impl.leaveGuest();
    QString error = impl.withCallerContext(caller, [&] { return entry->function(*entry->instance, in, out); });
    impl.enterGuest();
    if (!error.isEmpty()) {
        QByteArray utf8 = error.toUtf8();
        return wasmtime_trap_new(utf8.constData(), static_cast<size_t>(utf8.size()));
    }
    if (out.size() != resultCount) {
        QByteArray utf8 = QString("host function returned %1 results, %2 expected").arg(out.size()).arg(resultCount).toUtf8();
        return wasmtime_trap_new(utf8.constData(), static_cast<size_t>(utf8.size()));
    }
    for (size_t i = 0; i < resultCount; ++i) {
        results[i] = toVal(out[i]);
    }
    return nullptr;
}

} // namespace

WasmInstance::WasmInstance(std::shared_ptr<WasmModule> module) : WasmInstance(std::move(module), Limits()) {
}

WasmInstance::WasmInstance(std::shared_ptr<WasmModule> module, const Limits& limits) :
    _module(std::move(module)), _limits(limits), _impl(new Impl) {
    wasm_engine_t* engine = _module->impl().engine;
    _impl->store = wasmtime_store_new(engine, nullptr, nullptr);
    _impl->context = wasmtime_store_context(_impl->store);
    _impl->callTimeout = limits.callTimeout;
    _impl->callWarning = limits.callWarning;
    wasmtime_store_limiter(_impl->store, limits.memoryBytes, -1, -1, -1, -1);
    wasmtime_store_epoch_deadline_callback(_impl->store, &Impl::onDeadline, _impl.get(), nullptr);
    _impl->linker = wasmtime_linker_new(engine);
}

WasmInstance::~WasmInstance() {
    if (_impl->profiler) {
        wasmtime_guestprofiler_delete(_impl->profiler);
    }
    wasmtime_linker_delete(_impl->linker);
    wasmtime_store_delete(_impl->store);
}

void WasmInstance::defineHostFunction(const QString& module, const QString& name, const WasmSignature& signature,
                                      WasmHostFunction function) {
    auto entry = std::make_unique<HostFunctionEntry>();
    entry->instance = this;
    entry->function = std::move(function);
    entry->signature = signature;
    QByteArray moduleUtf8 = module.toUtf8();
    QByteArray nameUtf8 = name.toUtf8();
    wasm_functype_t* type = makeFunctionType(signature);
    auto result = wasmtime_linker_define_func(_impl->linker, moduleUtf8.constData(), static_cast<size_t>(moduleUtf8.size()),
                                              nameUtf8.constData(), static_cast<size_t>(nameUtf8.size()), type,
                                              &hostFunctionTrampoline, entry.get(), nullptr);
    wasm_functype_delete(type);
    if (result) {
        qCWarning(wasmScripting) << "cannot define host function" << module + "." + name << ":" << takeErrorText(result);
        return;
    }
    _impl->hostFunctions.push_back(std::move(entry));
}

void WasmInstance::enableWasi() {
    _impl->wasi = true;
}

bool WasmInstance::instantiate(QString& error) {
    if (_impl->instantiated) {
        error = "already instantiated";
        return false;
    }
    if (_impl->wasi) {
        // no arguments, no environment, no files, no standard streams
        wasi_config_t* wasi = wasi_config_new();
        auto result = wasmtime_context_set_wasi(_impl->context, wasi); // takes the config
        if (result) {
            error = takeErrorText(result);
            return false;
        }
        result = wasmtime_linker_define_wasi(_impl->linker);
        if (result) {
            error = takeErrorText(result);
            return false;
        }
    }
    _impl->beginCall(); // the module's start function runs now
    _impl->enterGuest();
    wasm_trap_t* trap = nullptr;
    auto result = wasmtime_linker_instantiate(_impl->linker, _impl->context, _module->impl().module, &_impl->instance, &trap);
    _impl->leaveGuest();
    if (result) {
        error = takeErrorText(result);
        return false;
    }
    if (trap) {
        error = takeTrapText(trap);
        return false;
    }
    _impl->instantiated = true;
    wasmtime_extern_t item;
    if (_impl->findExport("memory", item) && item.kind == WASMTIME_EXTERN_MEMORY) {
        _impl->memory = item.of.memory;
        _impl->hasMemory = true;
    }
    return true;
}

bool WasmInstance::isInstantiated() const {
    return _impl->instantiated;
}

bool WasmInstance::hasExport(const QString& name) const {
    wasmtime_extern_t item;
    return _impl->findExport(name, item);
}

bool WasmInstance::call(const QString& name, const std::vector<WasmValue>& args, std::vector<WasmValue>& results,
                        QString& error) {
    results.clear();
    if (_impl->stopDue() || (_impl->closed.load() && _impl->callDepth == 0)) {
        error = "the script was stopped";
        return false;
    }
    const Impl::ExportedFunction* exported = _impl->instantiated ? _impl->findFunction(name) : nullptr;
    if (!exported) {
        error = QString("no exported function \"%1\"").arg(name);
        return false;
    }
    wasmtime_func_t function = exported->function;
    size_t resultCount = exported->resultCount;
    if (exported->paramCount != args.size()) {
        error = QString("\"%1\" takes %2 arguments, %3 given").arg(name).arg(exported->paramCount).arg(args.size());
        return false;
    }
    // the values on the stack: the host calls into the module many times a frame
    const size_t INLINE_VALUES = 8;
    wasmtime_val_t inlineIn[INLINE_VALUES];
    wasmtime_val_t inlineOut[INLINE_VALUES];
    std::vector<wasmtime_val_t> heapIn, heapOut;
    wasmtime_val_t* in = inlineIn;
    wasmtime_val_t* out = inlineOut;
    if (args.size() > INLINE_VALUES) {
        heapIn.resize(args.size());
        in = heapIn.data();
    }
    if (resultCount > INLINE_VALUES) {
        heapOut.resize(resultCount);
        out = heapOut.data();
    }
    for (size_t i = 0; i < args.size(); ++i) {
        in[i] = toVal(args[i]);
    }
    _impl->beginCall();
    ++_impl->callDepth;
    _impl->enterGuest();
    wasm_trap_t* trap = nullptr;
    auto result = wasmtime_func_call(_impl->context, &function, in, args.size(), out, resultCount, &trap);
    _impl->leaveGuest();
    --_impl->callDepth;
    if (result) {
        error = takeErrorText(result);
        return false;
    }
    if (trap) {
        error = takeTrapText(trap);
        return false;
    }
    for (size_t i = 0; i < resultCount; ++i) {
        results.push_back(fromVal(out[i]));
    }
    return true;
}

size_t WasmInstance::memorySize() const {
    return _impl->hasMemory ? wasmtime_memory_data_size(_impl->context, &_impl->memory) : 0;
}

QByteArray WasmInstance::readMemory(uint64_t offset, size_t length) {
    size_t size = memorySize();
    if (offset > size || length > size - offset) {
        return QByteArray();
    }
    const uint8_t* data = wasmtime_memory_data(_impl->context, &_impl->memory);
    return QByteArray(reinterpret_cast<const char*>(data) + offset, static_cast<int>(length));
}

bool WasmInstance::writeMemory(uint64_t offset, const QByteArray& bytes) {
    size_t size = memorySize();
    size_t length = static_cast<size_t>(bytes.size());
    if (offset > size || length > size - offset) {
        return false;
    }
    uint8_t* data = wasmtime_memory_data(_impl->context, &_impl->memory);
    memcpy(data + offset, bytes.constData(), length);
    return true;
}

void WasmInstance::requestStop() {
    _impl->stopRequested = true;
}

void WasmInstance::requestStopAfter(std::chrono::steady_clock::time_point deadline, int world) {
    _impl->closed = true;
    _impl->stopWorld = world;
    const int64_t at = std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count());
    int64_t none = 0;
    _impl->stopDeadline.compare_exchange_strong(none, at);
}

bool WasmInstance::isStopRequested() const {
    return _impl->stopDue();
}

void WasmInstance::resumeAfterStop() {
    _impl->closed = false;
    if (_module->interruption() == WasmRuntime::Interruption::Cooperative) {
        _impl->stopRequested = false;
    }
}

bool WasmInstance::interruptRequested() {
    if (_impl->profiler) {
        _impl->sample(_module->runtime()->settings().epochTick);
    }
    _impl->checkWarning();
    QString reason = _impl->interruption();
    if (reason.isEmpty()) {
        return false;
    }
    // the time limit is recorded once per outermost call: a nested call (one made from a host function) unwinds
    // first and is the one reported; the outer frames, told to stop too, end without a second report
    const bool limit = !_impl->stopDue();
    if (_impl->interruptionReason.isEmpty() && !(limit && _impl->limitReported)) {
        _impl->interruptionReason = reason;
    }
    if (limit) {
        _impl->limitReported = true;
    }
    return true;
}

void WasmInstance::setCallWarningHandler(std::function<void(std::chrono::milliseconds elapsed)> handler) {
    _impl->warningHandler = std::move(handler);
}

QString WasmInstance::takeInterruptionReason() {
    QString reason = _impl->interruptionReason;
    _impl->interruptionReason.clear();
    return reason;
}

bool WasmInstance::startProfiling(const QString& name, QString& error) {
    if (_impl->profiler) {
        error = "already profiling";
        return false;
    }
    QByteArray utf8 = name.toUtf8();
    wasm_name_t moduleName;
    wasm_name_new(&moduleName, static_cast<size_t>(utf8.size()), utf8.constData());
    wasmtime_guestprofiler_modules_t modules { &moduleName, _module->impl().module };
    auto interval = std::chrono::duration_cast<std::chrono::nanoseconds>(_module->runtime()->settings().epochTick).count();
    _impl->profiler = wasmtime_guestprofiler_new(_module->impl().engine, &moduleName, uint64_t(interval), &modules, 1);
    wasm_name_delete(&moduleName); // the profiler keeps its own copies
    if (!_impl->profiler) {
        error = "the guest profiler could not be made";
        return false;
    }
    return true;
}

QByteArray WasmInstance::finishProfiling(QString& error) {
    if (!_impl->profiler) {
        error = "not profiling";
        return QByteArray();
    }
    wasm_byte_vec_t profile;
    auto result = wasmtime_guestprofiler_finish(_impl->profiler, &profile); // takes the profiler
    _impl->profiler = nullptr;
    if (result) {
        error = takeErrorText(result);
        return QByteArray();
    }
    QByteArray bytes(profile.data, static_cast<int>(profile.size));
    wasm_byte_vec_delete(&profile);
    return bytes;
}
