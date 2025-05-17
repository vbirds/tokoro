#include "Coro.h"
#include <iostream>
#include <thread>

// 一个简单的协程，延迟打印并返回一个整数
Coro<int> DelayedValue(int value, double delaySeconds)
{
    co_await Scheduler::Wait(delaySeconds);
    co_return value;
}

Coro<void> Delayed(double delaySeconds)
{
    co_await Scheduler::Wait(delaySeconds);
    co_return;
}

// 演示 All combinator：等待所有协程完成
Coro<void> TestAll()
{
    std::cout << "TestAll start" << std::endl;
    auto results = co_await All(
        DelayedValue(1, 0.1),
        DelayedValue(2, 0.05),
        DelayedValue(3, 0.2));
    std::cout << "Finished TestAll() values: ";
    for (auto v : results)
        std::cout << v << " ";
    std::cout << std::endl;
}

// 演示 Any combinator：等待最先完成的协程
Coro<void> TestAny()
{
    std::cout << "TestAny start" << std::endl;
    int winner = co_await Any(
        DelayedValue(10, 0.15),
        DelayedValue(20, 0.1),
        DelayedValue(30, 0.25));
    std::cout << "Finished TestAny() value: " << winner << std::endl;
}

Coro<void> TestWaitCoro()
{
    std::cout << "TestWaitCoro start" << std::endl;
    const int value = co_await DelayedValue(2, 0.05);
    co_await Delayed(0.05);
    std::cout << "TestWaitCoro Finished" << value << std::endl;
}

// 演示 stop(): 取消并销毁协程
Coro<void> LongRunning()
{
    int i = 0;
    while (true)
    {
        std::cout << "LongRunning iteration " << i++ << std::endl;
        co_await Scheduler::NextFrame();
    }
}

int main()
{
    using namespace std::chrono_literals;

    // 1) All
    auto h1 = Scheduler::Start(TestAll());
    // 2) Any
    auto h2 = Scheduler::Start(TestAny());
    // 3) Long running + stop
    auto h3 = Scheduler::Start(LongRunning());

    auto h4 = Scheduler::Start(TestWaitCoro());

    // 模拟帧更新
    int frame = 0;
    while (true)
    {
        // std::cout << "-- Frame " << ++frame << " --" << std::endl;

        if (frame == 50)
        {
            std::cout << "Stopping LongRunning at frame " << frame << std::endl;
            h3.Stop(); // 取消协程
        }

        Scheduler::Instance().Update();
        std::this_thread::sleep_for(33.3ms);

        frame++;
    }

    return 0;
}
