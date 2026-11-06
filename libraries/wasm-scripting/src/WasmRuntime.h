//
//  WasmRuntime.h
//  libraries/wasm-scripting/src
//
//  The WebAssembly runtime (wasmtime) behind a small interface: a runtime per process compiles modules; an
//  instance is one module running in its own store, with the host functions it was given, its own linear
//  memory, a memory limit and a time limit per call. Nothing of wasmtime's C API shows in these headers.
//
//  Threads: a WasmRuntime may be shared; a WasmModule may be instantiated from any thread; a WasmInstance
//  is used from one thread only, the one that calls into it (its host functions run on that thread too).
//  requestStop() and requestStopAfter() are the exception: they may be called from any thread and make the running
//  call trap at the next check (at once, or once a deadline has passed), within one epoch tick.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_WasmRuntime_h
#define hifi_WasmRuntime_h

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <variant>
#include <vector>

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QStringList>

class WasmModule;
class WasmInstance;

/// A WebAssembly value of one of the four number types. References and vectors are not carried.
using WasmValue = std::variant<int32_t, int64_t, float, double>;

enum class WasmType { I32, I64, F32, F64 };

struct WasmSignature {
    std::vector<WasmType> params;
    std::vector<WasmType> results;
};

/// A function the host gives a module. It receives the instance it was called from (for its memory), the
/// arguments, and fills the results. A non-empty return value is an error: the call traps with that text.
using WasmHostFunction = std::function<QString(WasmInstance& instance, const std::vector<WasmValue>& args,
                                               std::vector<WasmValue>& results)>;

class WasmRuntime : public std::enable_shared_from_this<WasmRuntime> {
public:
    /// How a call into a module is interrupted, for a stop or a time limit.
    enum class Interruption {
        /// The runtime checks a deadline at every function entry and loop of the module (wasmtime's epoch
        /// interruption): any module can be stopped, at a cost in every loop. For a module from anywhere.
        Epoch,
        /// The module asks the host whether to stop (WasmInstance::interruptRequested, through an import) and
        /// unwinds by itself: no cost in its loops, and no way to stop it if it never asks. For a module known to
        /// ask often, as the JavaScript guest does through QuickJS's interrupt handler.
        Cooperative
    };

    struct Settings {
        /// How often the epoch advances; the resolution of every instance's time limit and of requestStop().
        std::chrono::milliseconds epochTick { 10 };
        /// Off only to measure what the epoch checks cost: without them no time limit holds and requestStop()
        /// does not interrupt a running call.
        bool epochInterruption { true };
    };

    /// Always owned by a shared_ptr (std::make_shared): the modules it compiles keep it alive.
    WasmRuntime();
    explicit WasmRuntime(const Settings& settings);
    ~WasmRuntime();

    /// Compiles a module from its binary form; null with the error text on failure.
    std::shared_ptr<WasmModule> compile(const QByteArray& wasm, QString& error, Interruption interruption = Interruption::Epoch);
    /// Compiles a module from the text format (for tests and small programs); null with the error on failure.
    std::shared_ptr<WasmModule> compileText(const QString& text, QString& error, Interruption interruption = Interruption::Epoch);

    const Settings& settings() const { return _settings; }

    struct Impl;
    Impl& impl() { return *_impl; }

private:
    Settings _settings;
    std::unique_ptr<Impl> _impl;
};

/// A compiled module, immutable, shared by every instance of it.
class WasmModule {
public:
    ~WasmModule();

    /// The imports the module asks for, as "module.name".
    QStringList imports() const;
    /// The names the module exports.
    QStringList exports() const;

    std::shared_ptr<WasmRuntime> runtime() const { return _runtime; }
    WasmRuntime::Interruption interruption() const;

    struct Impl;
    Impl& impl() { return *_impl; }

private:
    friend class WasmRuntime;
    WasmModule(std::shared_ptr<WasmRuntime> runtime, std::unique_ptr<Impl> impl);
    std::shared_ptr<WasmRuntime> _runtime;
    std::unique_ptr<Impl> _impl;
};

class WasmInstance {
public:
    struct Limits {
        /// The most linear memory the instance may have; growth past it fails inside the module.
        int64_t memoryBytes { 256 * 1024 * 1024 };
        /// A call into the instance that runs longer than this is interrupted (a trap, or for a cooperative module
        /// the module's own unwinding); zero is no limit (a requested stop still interrupts the call). The time is
        /// the module's own: what it spends in host functions does not count, so a slow download or a long host
        /// call is not a runaway module.
        std::chrono::milliseconds callTimeout { 5000 };
        /// A call into the instance that runs longer than this, of the module's own time on the same clock, is
        /// reported once through the call warning handler and goes on; zero is no warning. For a module whose
        /// runaway hurts only itself, in place of callTimeout.
        std::chrono::milliseconds callWarning { 0 };
    };

    explicit WasmInstance(std::shared_ptr<WasmModule> module);
    WasmInstance(std::shared_ptr<WasmModule> module, const Limits& limits);
    ~WasmInstance();

    /// Before instantiate(): a function the module may import as `module`.`name`.
    void defineHostFunction(const QString& module, const QString& name, const WasmSignature& signature,
                            WasmHostFunction function);
    /// Before instantiate(): give the module WASI preview 1 with no files, no sockets, empty arguments and
    /// environment, and no standard streams.
    void enableWasi();

    /// Links the imports and runs the module's start function. False with the error text on failure.
    bool instantiate(QString& error);
    bool isInstantiated() const;

    bool hasExport(const QString& name) const;
    /// Calls an exported function. False with the error text when the export is missing, the arguments do
    /// not match, or the call traps (including the time limit and a requested stop).
    bool call(const QString& name, const std::vector<WasmValue>& args, std::vector<WasmValue>& results,
              QString& error);

    /// The instance's exported linear memory ("memory"). Size 0 when there is none.
    size_t memorySize() const;
    QByteArray readMemory(uint64_t offset, size_t length);
    bool writeMemory(uint64_t offset, const QByteArray& bytes);

    /// From any thread: the running call stops (with epoch interruption, it traps at the next epoch tick; with
    /// cooperative interruption, the module stops when it next asks), and every later call is refused.
    void requestStop();
    /// From any thread: the running call may finish, until `deadline` or until the world `world` is left
    /// (ScriptCaller::retireWorld; 0 for none), whichever comes first; then it stops as with requestStop(). A call
    /// it makes into the module meanwhile (a value returned to it, a callback) goes on as usual; a new outermost
    /// call is refused at once. A second call keeps the earlier deadline.
    void requestStopAfter(std::chrono::steady_clock::time_point deadline, int world);
    /// Whether the module must stop now: requestStop() was called, or requestStopAfter()'s deadline or world came.
    bool isStopRequested() const;
    /// Takes requestStop() back once the call it ended is over, so the module can be called again. Only a module that
    /// stops by itself (cooperative interruption) comes out of that call whole: one stopped by a trap stays stopped.
    /// Outermost calls are let in again after requestStopAfter(), under the same deadline.
    void resumeAfterStop();
    /// Whether the running call should stop now: a stop was requested, or the call has run past its time limit.
    /// A module compiled for cooperative interruption asks this often (the host interface's "interrupted" import);
    /// it is also where such a module's profile is sampled.
    bool interruptRequested();
    /// Why a cooperative module was told to stop since this was last asked (it unwinds and returns normally, so
    /// the host reports the reason); empty when it was not.
    QString takeInterruptionReason();
    /// Called once in an outermost call that runs longer than Limits::callWarning, with the module's own time so
    /// far, on the thread running the call and while it runs: it must not call into this instance.
    void setCallWarningHandler(std::function<void(std::chrono::milliseconds elapsed)> handler);

    /// Samples the module's stack at every epoch tick while a call runs (wasmtime's guest profiler) until
    /// finishProfiling(), which returns the profile in the Firefox Profiler's format. For measurements.
    bool startProfiling(const QString& name, QString& error);
    QByteArray finishProfiling(QString& error);

    const Limits& limits() const { return _limits; }
    std::shared_ptr<WasmModule> module() const { return _module; }

    struct Impl;
    Impl& impl() { return *_impl; }

private:
    std::shared_ptr<WasmModule> _module;
    Limits _limits;
    std::unique_ptr<Impl> _impl;
};

#endif // hifi_WasmRuntime_h
