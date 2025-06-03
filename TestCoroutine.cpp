#include "tokoro.h"
#include <cassert>
#include <iostream>
#include <vector>

using namespace tokoro;

Async<int> DelayedValue(int value, double delaySeconds)
{
    co_await Wait(delaySeconds);
    co_return value;
}

Async<void> Delayed(double delaySeconds)
{
    co_await Wait(delaySeconds);
    co_return;
}

// Test awaiting a single coroutine with a return value
void TestSingleAwaitValue()
{
    Scheduler sched;
    bool      completed = false;
    int       result    = 0;

    auto h = sched.Start([&]() -> Async<void> {
        result    = co_await DelayedValue(42, 0.0);
        completed = true;
    });

    // Drive scheduler until completed or timeout
    for (int iter = 0; iter < 1000000 && !completed; ++iter)
    {
        sched.Update();
    }
    assert(completed && "Scheduler did not finish in time");
    assert(result == 42);
    assert(h.IsDown());
    std::cout << "TestSingleAwaitValue passed\n";
}

// Test awaiting a single void coroutine
void TestSingleAwaitVoid()
{
    Scheduler sched;
    bool      completed = false;

    auto h = sched.Start([&]() -> Async<void> {
        co_await Delayed(0.0);
        completed = true;
    });

    // Drive scheduler until completed or timeout
    for (int iter = 0; iter < 1000000 && !completed; ++iter)
    {
        sched.Update();
    }
    assert(completed && "Scheduler did not finish in time");
    assert(h.IsDown());
    std::cout << "TestSingleAwaitVoid passed\n";
}

// Test All combinator
void TestAllCombinator()
{
    Scheduler      sched;
    bool           completed = false;
    int            a = 0, b = 0, c = 0;
    std::monostate d;

    auto h = sched.Start([&]() -> Async<void> {
        std::tie(a, b, c, d) = co_await All(
            DelayedValue(1, 0.0),
            DelayedValue(2, 0.001),
            DelayedValue(3, 0.0),
            Delayed(0.0));
        completed = true;
    });

    // Drive scheduler until completed or timeout
    for (int iter = 0; iter < 1000000 && !completed; ++iter)
    {
        sched.Update();
    }
    assert(completed && "Scheduler did not finish in time");
    assert(a == 1 && b == 2 && c == 3);
    assert(h.IsDown());
    std::cout << "TestAllCombinator passed\n";
}

// Test Any combinator
void TestAnyCombinator()
{
    Scheduler          sched;
    bool               completed = false;
    std::optional<int> a, b;

    auto h = sched.Start([&]() -> Async<void> {
        auto tup = co_await Any(DelayedValue(10, 10),
                                DelayedValue(20, 0.0));

        a = std::get<0>(tup);
        b = std::get<1>(tup);

        co_await Any(DelayedValue(10, 10), Delayed(0.000000000000000001));

        completed = true;
    });

    // Drive scheduler until completed or timeout
    for (int iter = 0; iter < 1000000 && !completed; ++iter)
    {
        sched.Update();
    }
    assert(completed && "Scheduler did not finish in time");
    assert(!a.has_value() && b.has_value() && b.value() == 20);
    assert(h.IsDown());
    std::cout << "TestAnyCombinator passed\n";
}

// Recursive Fibonacci coroutine
Async<int> Fib(int n)
{
    if (n < 2)
        co_return n;

    co_await Wait();

    auto a  = Fib(n - 1);
    auto b  = Fib(n - 2);
    int  ra = co_await a;
    int  rb = co_await b;
    co_return ra + rb;
}

// Test NextFrame ordering
void TestNextFrame()
{
    Scheduler sched;
    int       count = 0;

    auto h = sched.Start([&]() -> Async<void> {
        co_await Wait(); // resume 1
        count += 1;
        co_await Wait(); // resume 2
        count += 2;
    });

    // Before any update, count==0
    assert(count == 0);
    sched.Update(); // first resume
    assert(count == 1);
    sched.Update(); // second resume
    assert(count == 3);
    assert(h.IsDown());
    std::cout << "TestNextFrame passed\n";
}

// Test Stop and cancellation
void TestStop()
{
    Scheduler sched;
    int       loops = 0;

    auto h = sched.Start([&]() -> Async<void> {
        while (true)
        {
            co_await Wait();
            loops++;
        }
    });

    // run a few frames
    for (int i = 0; i < 5; ++i)
        sched.Update();
    assert(loops == 5);

    assert(!h.IsDown());
    h.Stop();
    assert(h.IsDown());
    sched.Update();
    assert(loops == 5);
    std::cout << "TestStop passed\n";
}

void TestUseHandleAfterSchedulerDestroyed()
{
    Scheduler* sched = new Scheduler();

    auto handle = sched->Start([&]() -> Async<int> {
        co_await Wait(0.00000000001);
        co_return 123;
    });

    for (int iter = 0; iter < 1000000000 && !handle.IsDown(); ++iter)
    {
        sched->Update();
    }

    delete sched;
    assert(!handle.GetReturn().has_value());
    std::cout << "TestUseHandleAfterSchedulerDestroyed passed\n";
}

void TestStartInCoroutine()
{
    Scheduler sched;
    int       frame = 0;

    sched.Start([&]() -> Async<void> {
        co_await Wait();

        auto innerCoro = [&]() -> Async<void> {
            co_await Wait();
            // Inner coroutine should always resume in next frame
            assert(frame == 1);
        };

        // Star more than 2 to make sure some inner coros insert after outer coros
        sched.Start(innerCoro);
        sched.Start(innerCoro);
        sched.Start(innerCoro);
        sched.Start(innerCoro);

        assert(frame == 0);
    });

    // One more outer coro to make sure the first update won't exceed immediately.
    sched.Start([&]() -> Async<void> {
        co_await Wait();
        assert(frame == 0);
    });

    for (; frame < 5; ++frame)
    {
        sched.Update();
    }

    std::cout << "TestStartInCoroutine passed\n";
}

// Test global scheduler and GetReturn
void TestGlobalScheduler()
{
    auto handle = GlobalScheduler().Start([&]() -> Async<int> {
        co_await Wait(0.0);
        co_return 123;
    });

    // Drive global scheduler until done or timeout
    for (int iter = 0; iter < 10 && !handle.IsDown(); ++iter)
    {
        GlobalScheduler().Update();
    }
    assert(handle.IsDown());
    auto ret = handle.GetReturn();
    assert(ret.has_value() && ret.value() == 123);
    std::cout << "TestGlobalScheduler passed\n";
}

template <typename T>
struct TmplTester
{
    void Check()
    {
        if constexpr (!std::is_same<T, int>::value)
        {
            assert(false);
        }
    }
};

// Test only to make sure the move/copy constructor works.
void TestTmplAnyMove()
{
    internal::TmplAny<TmplTester> A = TmplTester<int>();
    internal::TmplAny<TmplTester> B = TmplTester<int>();

    internal::TmplAny<TmplTester> test(std::move(A));
    test.WithTmplArg<int>().Check();

    test = std::move(B);
    test.WithTmplArg<int>().Check();

    internal::TmplAny<TmplTester> test2(test);
    test2.WithTmplArg<int>().Check();

    std::cout << "TestTmplAnyMove passed\n";
}

// TestCustomUpdateAndTimers
//
enum class UpdateType
{
    Update = 0,
    PreUpdate,
    PostUpdate,
    Count,
};

enum class TimeType
{
    EmuRealTime = 0,
    GameTime,
    Count,
};

// Give alias names for ease of life.
// Note: You can still use Scheduler, Wait and Handle if you really like them.
// Just don't introduce 'using namespace tokoro' to your code.
using MyScheduler = SchedulerBP<UpdateType, TimeType>;
using MyWait      = WaitBP<UpdateType, TimeType>;
template <typename T>
using MyHandle = HandleBP<T, UpdateType, TimeType>;

void TestCustomUpdateAndTimers()
{
    MyScheduler sched;

    double emuRealTime = 0;
    // Note: this timer is only for test,
    // in most applications the default timer is good enough for 'real time'
    sched.SetCustomTimer(TimeType::EmuRealTime, [&]() -> double { return emuRealTime; });

    double gameTime = 0;
    sched.SetCustomTimer(TimeType::GameTime, [&]() -> double { return gameTime; });

    bool gamePaused = false;

    // Help variable for reality checking
    UpdateType curUpdateType = UpdateType::PreUpdate;

    // Define the test coroutine
    MyHandle handle = sched.Start([&]() -> Async<void> {
        // Check wait in realtime
        co_await MyWait(1);
        assert(emuRealTime >= 1);

        // Check the ability to switch between updates.
        co_await MyWait(0, UpdateType::PreUpdate);
        assert(curUpdateType == UpdateType::PreUpdate);

        co_await MyWait(0, UpdateType::Update);
        assert(curUpdateType == UpdateType::Update);

        co_await MyWait(0, UpdateType::PostUpdate);
        assert(curUpdateType == UpdateType::PostUpdate);

        // Start another coro to stop and start the game time.
        sched.Start([&]() -> Async<void> {
            gamePaused = true;
            co_await MyWait(2);
            gamePaused = false;
        });

        const double saveGameTime = gameTime;
        co_await MyWait(0, UpdateType::Update, TimeType::GameTime); // Wait one game frame
        // Game time should be still paused.
        // Please note this compare assumes EmuRealTime update is before GameTime. Or there will be one frame time diff.
        assert(saveGameTime == gameTime);

        // Wait until the game time start to move again.
        co_await MyWait(0.1, UpdateType::Update, TimeType::GameTime);
        assert(saveGameTime < gameTime);
        assert(gameTime < emuRealTime);

        // Check the ability to switch between updates ub game time.
        co_await MyWait(0, UpdateType::PreUpdate, TimeType::GameTime);
        assert(curUpdateType == UpdateType::PreUpdate);

        co_await MyWait(0, UpdateType::Update, TimeType::GameTime);
        assert(curUpdateType == UpdateType::Update);

        co_await MyWait(0, UpdateType::PostUpdate, TimeType::GameTime);
        assert(curUpdateType == UpdateType::PostUpdate);
    });

    // Game Loop
    //
    constexpr double frameTime = 0.166666;
    for (int i = 0; i < 100 && !handle.IsDown(); ++i)
    {
        emuRealTime += frameTime;
        if (!gamePaused)
            gameTime += frameTime;

        curUpdateType = UpdateType::PreUpdate;
        sched.Update(UpdateType::PreUpdate, TimeType::EmuRealTime);
        sched.Update(UpdateType::PreUpdate, TimeType::GameTime);

        curUpdateType = UpdateType::Update;
        sched.Update(UpdateType::Update, TimeType::EmuRealTime);
        sched.Update(UpdateType::Update, TimeType::GameTime);

        curUpdateType = UpdateType::PostUpdate;
        sched.Update(UpdateType::PostUpdate, TimeType::EmuRealTime);
        sched.Update(UpdateType::PostUpdate, TimeType::GameTime);
    }

    // task should finish in time
    assert(handle.IsDown());
    std::cout << "TestCustomUpdateAndTimers passed\n";
}

void TestWaitUntilAndWhile()
{
    Scheduler sched;
    int       frame = 0;

    sched.Start([&]() -> Async<void> {
        co_await WaitUntil([&]() { return frame == 10; });
        assert(frame == 10);

        co_await WaitWhile([&]() { return frame < 20; });
        assert(frame == 20);
    });

    for (; frame < 100; ++frame)
    {
        sched.Update();
    }

    std::cout << "TestWaitUntilAndWhile passed\n";
}

// Stress test: spawn many coroutines computing Fibonacci and cancel some
void TestStress(size_t count, int fibN)
{
    Scheduler                sched;
    std::vector<Handle<int>> handles;
    handles.reserve(count);

    // Start coroutines
    for (size_t i = 0; i < count; ++i)
    {
        auto h = sched.Start([fibN]() -> Async<int> {
            co_return co_await Fib(fibN);
        });
        handles.push_back(std::move(h));
    }

    // Cancel half
    for (size_t i = 0; i < count; i += 2)
    {
        handles[i].Stop();
    }

    // Drive scheduler until remaining complete or timeout
    auto done = [&]() {
        for (size_t i = 1; i < count; i += 2)
        {
            if (!handles[i].IsDown())
                return false;
        }
        return true;
    };
    for (int iter = 0; iter < 10000000 && !done(); ++iter)
    {
        sched.Update();
    }
    assert(done() && "Scheduler did not finish in time");

    // Verify results
    for (size_t i = 1; i < count; i += 2)
    {
        auto r = handles[i].GetReturn();
        assert(r.has_value());
        // Fibonacci correctness for small fibN
    }
    std::cout << "TestStress(" << count << ", " << fibN << ") passed\n";
}

int main()
{
    TestSingleAwaitValue();
    TestSingleAwaitVoid();
    TestAllCombinator();
    TestAnyCombinator();
    TestNextFrame();
    TestStop();
    TestUseHandleAfterSchedulerDestroyed();
    TestStartInCoroutine();
    TestGlobalScheduler();
    TestTmplAnyMove();
    TestCustomUpdateAndTimers();
    TestWaitUntilAndWhile();

    TestStress(10000, 10);

    std::cout << "All tests passed successfully." << std::endl;
    return 0;
}
