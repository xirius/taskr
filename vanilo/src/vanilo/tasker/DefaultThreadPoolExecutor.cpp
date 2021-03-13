#include <vanilo/tasker/DefaultThreadPoolExecutor.h>

using namespace vanilo::concurrent;
using namespace vanilo::tasker;

/// ThreadPool system tasks
/// ================================================================================================

class StopThreadException final: public std::exception
{
  public:
    [[nodiscard]] const char* what() const noexcept override
    {
        return "Stop the thread execution exception!";
    }
};

class StopThreadTask: public Task
{
  public:
    explicit StopThreadTask(
        TaskExecutor* executor, std::thread thread, std::shared_ptr<std::promise<void>> promise, std::shared_ptr<std::atomic_int> counter)
        : _executor{executor}, _thread{std::move(thread)}, _promise{std::move(promise)}, _counter{std::move(counter)}
    {
    }

    void cancel() noexcept override
    {
    }

    void run() override
    {
        // Detach the thread if the right thread is found
        if (std::this_thread::get_id() == _thread.get_id()) {
            _thread.detach();

            if (--(*_counter) == 0) {
                // All the scheduled threads that have to be stopped are found
                _promise->set_value();
            }

            throw StopThreadException{};
        }

        _executor->submit(std::make_unique<StopThreadTask>(_executor, std::move(_thread), std::move(_promise), std::move(_counter)));
    }

  private:
    TaskExecutor* _executor;
    std::thread _thread;
    std::shared_ptr<std::promise<void>> _promise;
    std::shared_ptr<std::atomic_int> _counter;
};

/// DefaultThreadPoolExecutor implementation
/// ================================================================================================

DefaultThreadPoolExecutor::DefaultThreadPoolExecutor(size_t numThreads)
{
    init(numThreads);
}

DefaultThreadPoolExecutor::~DefaultThreadPoolExecutor()
{
    invalidate();
}

size_t DefaultThreadPoolExecutor::count() const
{
    return _queue.size();
}

std::future<void> DefaultThreadPoolExecutor::resize(size_t numThreads)
{
    std::lock_guard<std::mutex> _lock{_mutex};
    auto promise = std::make_shared<std::promise<void>>();
    auto future  = promise->get_future();

    if (_threads.size() < numThreads) {
        for (auto i = _threads.size(); i < numThreads; i++) {
            _threads.enqueue(std::thread{&DefaultThreadPoolExecutor::worker, this});
        }

        promise->set_value();
        return future;
    }

    if (_threads.size() > numThreads) {
        auto counter = std::make_shared<std::atomic_int>(_threads.size() - numThreads);
        std::thread thread;

        for (auto i = _threads.size(); i > numThreads; i--) {
            if (_threads.tryDequeue(thread)) {
                submit(std::make_unique<StopThreadTask>(this, std::move(thread), promise, std::move(counter)));
            }
        }

        return future;
    }

    promise->set_value();
    return future;
}

void DefaultThreadPoolExecutor::submit(std::unique_ptr<Task> task)
{
    _queue.enqueue(std::move(task));
}

size_t DefaultThreadPoolExecutor::threadCount() const noexcept
{
    return _threads.size();
}

//! Private members
void DefaultThreadPoolExecutor::init(size_t numThreads)
{
    try {
        resize(numThreads);
    }
    catch (...) {
        invalidate();
        TRACE("An unhandled exception occurred during initialisation of the DefaultThreadPoolExecutor!");
        throw;
    }
}

void DefaultThreadPoolExecutor::invalidate()
{
    auto tasks = _queue.invalidate();

    for (auto& task : tasks) {
        task->cancel();
        // Not enough !!!
    }

    std::thread thread;
    while (_threads.tryDequeue(thread)) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

void DefaultThreadPoolExecutor::worker()
{
    std::unique_ptr<Task> task;

    try {
        while (true) {
            if (!_queue.waitDequeue(task)) {
                return;
            }

            try {
                task->run();
            }
            catch (const StopThreadException&) {
                throw; // This thread was asked to exit so propagate the exception
            }
            catch (const std::exception& ex) {
                TRACE("An unhandled exception occurred during execution of the task. Message: %s", ex.what());
            }
            catch (...) {
                TRACE("An unhandled exception occurred during execution of the task!");
            }
        }
    }
    catch (const StopThreadException&) {
        return; // Gracefully stop the thread execution
    }
}
