#include "Coro.h"
#include <iostream>
#include <thread>

std::ostream& operator<<(std::ostream& os, const std::monostate&)
{
    return os << "void";
}

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
    auto [a, b, c] = co_await All(
        DelayedValue(1, 0.1),
        Delayed(0.05),
        DelayedValue(3, 0.2));
    std::cout << "Finished TestAll() values: " << a << b << c << std::endl;
}

// 演示 Any combinator：等待最先完成的协程
Coro<void> TestAny()
{
    std::cout << "TestAny start" << std::endl;
    auto [a, b, c] = co_await Any(
        DelayedValue(10, 0.15),
        Delayed(0.1),
        DelayedValue(30, 0.25));
    std::cout << "Finished TestAny() value: ";
    if (a.has_value())
        std::cout << a.value();
    else
        std::cout << "none";
    if (b.has_value())
        std::cout << b.value();
    else
        std::cout << "none";
    if (c.has_value())
        std::cout << c.value();
    else
        std::cout << "none";
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
    auto h1 = Scheduler::Instance().Start(TestAll);
    // 2) Any
    auto h2 = Scheduler::Instance().Start(TestAny);
    // 3) Long running + stop
    auto h3 = Scheduler::Instance().Start(LongRunning);

    auto h4 = Scheduler::Instance().Start(TestWaitCoro);

    // 模拟帧更新
    int frame = 0;
    while (true)
    {
        // std::cout << "-- Frame " << ++frame << " --" << std::endl;

        if (frame == 20)
        {
            std::cout << "Stopping LongRunning at frame " << frame << std::endl;
            h3.Stop(); // 取消协程
        }

        if (frame == 26)
        {
            break;
        }

        Scheduler::Instance().Update();
        std::this_thread::sleep_for(33.3ms);

        frame++;
    }

    return 0;
}
