#include "zb/library_runtime.h"

#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

#include "zb/log.h"

namespace zb {

namespace {

struct Response {
    bool ok = false;
    std::optional<GuestResult> result;
    std::string error;
};

struct Command {
    enum class Kind { Load, Symbol, Call };
    Kind kind = Kind::Call;
    std::uint32_t value = 0;
    std::uint32_t flags = 0;
    std::string text;
    GuestCall args;
    std::promise<Response> done;
};

using Invoke = std::function<std::optional<GuestResult>(std::uint32_t function, const GuestCall& args)>;

std::string exit_message(int status) {
    if (status == ZB_HOST_EXIT_PRELOAD) return "zbhost could not preload a library (status 4)";
    return "zbhost exited with status " + std::to_string(status);
}

}  // namespace

struct LibraryRuntime::Impl {
    Process process;
    Process::HostCallHandler chained;
    std::thread runner;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::shared_ptr<Command>> commands;
    zb_service_api api{};
    GuestThread* service = nullptr;
    std::thread::id service_id;
    bool started = false;
    bool ready = false;
    bool finished = false;
    std::string startup_error;

    bool validate_api(std::uint32_t address, std::string& error) {
        const std::uint8_t* source = process.memory().host_ptr(address, sizeof api, kPageRead);
        if (source == nullptr) {
            error = "zbhost passed an unreadable service API";
            return false;
        }
        zb_service_api candidate;
        std::memcpy(&candidate, source, sizeof candidate);
        if (candidate.size != sizeof candidate || candidate.version != ZB_SERVICE_PROTOCOL_VERSION) {
            error = "zbhost service protocol mismatch";
            return false;
        }
        if (candidate.dlopen_fn == 0 || candidate.dlsym_fn == 0 || candidate.dlerror_fn == 0 ||
            candidate.spawn_carrier_fn == 0 || candidate.malloc_fn == 0 || candidate.free_fn == 0 ||
            candidate.scratch_size == 0 || candidate.scratch_size > ZB_SERVICE_SCRATCH_SIZE ||
            process.memory().host_ptr(candidate.scratch, candidate.scratch_size, kPageWrite) == nullptr) {
            error = "zbhost passed an invalid service API";
            return false;
        }
        api = candidate;
        return true;
    }

    bool copy_text(std::uint32_t buffer, std::uint32_t size, const std::string& text, std::string& error) {
        if (text.find('\0') != std::string::npos || text.size() + 1 > size) {
            error = "guest loader string does not fit its scratch buffer";
            return false;
        }
        std::uint8_t* destination = process.memory().host_ptr(buffer, text.size() + 1, kPageWrite);
        if (destination == nullptr) {
            error = "guest loader scratch buffer is not writable";
            return false;
        }
        std::memcpy(destination, text.c_str(), text.size() + 1);
        return true;
    }

    std::string read_text(std::uint32_t address) {
        if (address == 0) return "guest dlerror returned null";
        std::string text;
        for (std::uint32_t i = 0; i < ZB_SERVICE_SCRATCH_SIZE; ++i) {
            const std::uint8_t* byte = process.memory().host_ptr(address + i, 1, kPageRead);
            if (byte == nullptr) return "guest dlerror returned an unreadable string";
            if (*byte == 0) return text;
            text.push_back(static_cast<char>(*byte));
        }
        return "guest dlerror string is not terminated";
    }

    // dlerror is per guest thread: read it through the same invoke as the failed call.
    std::string last_dlerror(const Invoke& invoke) {
        const auto result = invoke(api.dlerror_fn, GuestCall{});
        return result ? read_text(result->r0) : "guest dlerror call failed";
    }

    std::uint32_t load(const Invoke& invoke, std::uint32_t buffer, std::uint32_t size, const std::string& path,
                       std::uint32_t guest_flags, std::string& error) {
        if (!copy_text(buffer, size, path, error)) return 0;
        GuestCall args;
        args.regs = {buffer, guest_flags, 0, 0};
        const auto result = invoke(api.dlopen_fn, args);
        if (!result) {
            error = "guest dlopen call failed";
            return 0;
        }
        if (result->r0 == 0) error = last_dlerror(invoke);
        return result->r0;
    }

    std::uint32_t symbol(const Invoke& invoke, std::uint32_t buffer, std::uint32_t size, std::uint32_t handle,
                         const std::string& name, std::string& error) {
        if (!copy_text(buffer, size, name, error)) return 0;
        GuestCall args;
        args.regs = {handle, buffer, 0, 0};
        const auto result = invoke(api.dlsym_fn, args);
        if (!result) {
            error = "guest dlsym call failed";
            return 0;
        }
        if (result->r0 == 0) error = last_dlerror(invoke);
        return result->r0;
    }

    Response execute(GuestThread& thread, const Command& command) {
        const Invoke invoke = [&](std::uint32_t function, const GuestCall& args) {
            return process.call_guest(thread, function, args);
        };
        Response response;
        switch (command.kind) {
        case Command::Kind::Load:
            response.result = GuestResult{
                load(invoke, api.scratch, api.scratch_size, command.text, command.flags, response.error), 0};
            response.ok = response.result->r0 != 0;
            break;
        case Command::Kind::Symbol:
            response.result = GuestResult{
                symbol(invoke, api.scratch, api.scratch_size, command.value, command.text, response.error), 0};
            response.ok = response.result->r0 != 0;
            break;
        case Command::Kind::Call:
            response.result = invoke(command.value, command.args);
            response.ok = response.result.has_value();
            if (!response.ok) response.error = "guest service call failed";
            break;
        }
        return response;
    }

    // Serves commands until a signal must be delivered (r0 = AGAIN) or the guest is exiting.
    void serve(GuestThread& thread) {
        for (;;) {
            const std::uint32_t token = thread.park_token();
            if (thread.has_pending_signals(thread.sigmask)) {
                thread.regs()[0] = ZB_SERVICE_AGAIN;
                return;
            }
            std::shared_ptr<Command> command;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!commands.empty()) {
                    command = commands.front();
                    commands.pop_front();
                }
            }
            if (!command) {
                thread.park(token);
                continue;
            }
            command->done.set_value(execute(thread, *command));
            if (process.exiting()) {
                thread.regs()[0] = 1;
                return;
            }
        }
    }

    bool handle_ready(GuestThread& thread) {
        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!ready) {
                std::string error;
                if (!validate_api(thread.regs()[0], error)) {
                    startup_error = std::move(error);
                    lock.unlock();
                    cv.notify_all();
                    thread.regs()[0] = 1;
                    return true;
                }
                service = &thread;
                service_id = std::this_thread::get_id();
                ready = true;
                lock.unlock();
                cv.notify_all();
            }
        }
        serve(thread);
        return true;
    }

    bool handle_host_call(std::uint32_t index, GuestThread& thread) {
        if (index >= ZB_RUNTIME_HOST_CALL_FIRST && index <= ZB_RUNTIME_HOST_CALL_LAST) {
            if (index == ZB_SERVICE_READY_INDEX) return handle_ready(thread);
            return false;
        }
        return chained && chained(index, thread);
    }

    Response submit(std::shared_ptr<Command> command) {
        std::future<Response> future = command->done.get_future();
        GuestThread* target = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!ready || finished) return {false, std::nullopt, "guest library runtime is not running"};
            if (std::this_thread::get_id() == service_id) {
                return {false, std::nullopt, "guest service request made on the service thread; use call_on_current"};
            }
            commands.push_back(std::move(command));
            target = service;
        }
        target->wake();
        return future.get();
    }
};

LibraryRuntime::LibraryRuntime() : impl_(std::make_unique<Impl>()) {}

LibraryRuntime::~LibraryRuntime() {
    if (!impl_->runner.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->ready || !impl_->finished) {
            log("LibraryRuntime destroyed after start; the runtime is process-lifetime");
            std::abort();
        }
    }
    impl_->runner.join();
}

void LibraryRuntime::set_host_call_handler(Process::HostCallHandler handler) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->started) {
        log("LibraryRuntime::set_host_call_handler called after start");
        std::abort();
    }
    impl_->chained = std::move(handler);
}

bool LibraryRuntime::start(const LibraryRuntimeOptions& options, std::string& error) {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->started) {
            error = "guest library runtime was already started";
            return false;
        }
        impl_->started = true;
    }
    Impl* impl = impl_.get();
    impl->process.set_sysroot(options.sysroot);
    impl->process.set_host_call_handler(
        [impl](std::uint32_t index, GuestThread& thread) { return impl->handle_host_call(index, thread); });
    std::vector<std::string> argv = {options.zbhost, std::to_string(options.target_sdk)};
    if (!options.preload.empty()) argv.push_back(options.preload);
    impl->runner = std::thread([impl, argv, options] {
        const int status = impl->process.run(options.zbhost, argv, options.guest_environment);
        std::deque<std::shared_ptr<Command>> pending;
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->finished = true;
            if (!impl->ready && impl->startup_error.empty()) impl->startup_error = exit_message(status);
            pending.swap(impl->commands);
        }
        impl->cv.notify_all();
        for (const auto& command : pending) command->done.set_value({false, std::nullopt, exit_message(status)});
    });

    std::unique_lock<std::mutex> lock(impl->mutex);
    const bool settled = impl->cv.wait_for(lock, options.ready_timeout, [impl] {
        return impl->ready || impl->finished || !impl->startup_error.empty();
    });
    if (impl->ready) return true;
    error = settled ? impl->startup_error
                    : "zbhost did not report ready within " + std::to_string(options.ready_timeout.count()) + " ms";
    return false;
}

std::uint32_t LibraryRuntime::load_library(const std::string& path, std::uint32_t guest_flags, std::string& error) {
    auto command = std::make_shared<Command>();
    command->kind = Command::Kind::Load;
    command->text = path;
    command->flags = guest_flags;
    Response response = impl_->submit(std::move(command));
    if (!response.ok) {
        error = std::move(response.error);
        return 0;
    }
    return response.result->r0;
}

std::uint32_t LibraryRuntime::find_symbol(std::uint32_t handle, const std::string& name, std::string& error) {
    auto command = std::make_shared<Command>();
    command->kind = Command::Kind::Symbol;
    command->value = handle;
    command->text = name;
    Response response = impl_->submit(std::move(command));
    if (!response.ok) {
        error = std::move(response.error);
        return 0;
    }
    return response.result->r0;
}

std::optional<GuestResult> LibraryRuntime::call_on_service(std::uint32_t function, const GuestCall& args) {
    auto command = std::make_shared<Command>();
    command->kind = Command::Kind::Call;
    command->value = function;
    command->args = args;
    Response response = impl_->submit(std::move(command));
    return response.ok ? response.result : std::nullopt;
}

std::optional<GuestResult> LibraryRuntime::call_on_current(std::uint32_t function, const GuestCall& args) {
    GuestThread* thread = Process::current_thread();
    if (thread == nullptr) return std::nullopt;
    return impl_->process.call_guest(*thread, function, args);
}

GuestMemory& LibraryRuntime::memory() {
    return impl_->process.memory();
}

const zb_service_api& LibraryRuntime::service_api() const {
    return impl_->api;
}

std::size_t LibraryRuntime::guest_thread_count() const {
    return impl_->process.thread_count();
}

}  // namespace zb
