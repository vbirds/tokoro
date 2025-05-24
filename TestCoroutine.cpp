#include "Coro.h"
#include <iostream>
#include <thread>

std::ostream& operator<<(std::ostream& os, const std::monostate&)
{
    return os << "void";
}

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

Coro<void> TestAll()
{
    std::cout << "TestAll start" << std::endl;
    auto [a, b, c] = co_await All(
        DelayedValue(1, 0.1),
        Delayed(0.05),
        DelayedValue(3, 0.2));
    std::cout << "Finished TestAll() values: " << a << b << c << std::endl;
}

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

    std::cout << std::endl;
}

Coro<void> TestWaitCoro()
{
    std::cout << "TestWaitCoro start" << std::endl;
    const int value = co_await DelayedValue(2, 0.05);
    co_await Delayed(0.05);
    std::cout << "TestWaitCoro Finished" << value << std::endl;
}

Coro<void> LongRunning()
{
    int i = 0;
    while (true)
    {
        std::cout << "LongRunning iteration " << i++ << std::endl;
        co_await Scheduler::NextFrame();
    }
}

class ValuePrinter
{
public:
    ValuePrinter(int value)
        : mValue(value)
    {
        std::cout << "ValuePrinter " << mValue << std::endl;
    }

    ValuePrinter(ValuePrinter&& other)
        : mValue(other.mValue)
    {
        std::cout << "ValuePrinter move " << mValue << std::endl;
        other.mValue = 0;
    }

    ValuePrinter(const ValuePrinter& other)
    {
        mValue = other.mValue;
        std::cout << "ValuePrinter Copy " << mValue << std::endl;
    }

    ~ValuePrinter()
    {
        std::cout << "~ValuePrinter " << mValue << std::endl;
        mValue = 0;
    }

    void Print() const
    {
        std::cout << "ValuePrinter Print " << mValue << std::endl;
    }

private:
    int mValue = 0;
};

int main()
{
    using namespace std::chrono_literals;

    // 1) All
    auto h1 = Scheduler::Instance().Start(TestAll);
    // 2) Any
    Scheduler::Instance().Start(TestAny);
    // 3) Long running + stop
    auto h3 = Scheduler::Instance().Start(LongRunning);
    // 4) Wait single coro
    Scheduler::Instance().Start(TestWaitCoro);
    // 5) Get return
    auto h5 = Scheduler::Instance().Start(DelayedValue, 99, 0.2);

    ValuePrinter printer(101);
    Scheduler::Instance().Start([=]() -> Coro<void> {
        co_await Scheduler::Instance().Wait(0.5);
        printer.Print();
    });

    int frame = 0;
    while (true)
    {
        if (frame == 20)
        {
            std::cout << "Stopping LongRunning at frame " << frame << std::endl;
            h3.Stop();
        }

        if (frame == 26)
        {
            break;
        }

        Scheduler::Instance().Update();
        std::this_thread::sleep_for(33.3ms);

        frame++;
    }

    if (h1.IsDown())
    {
        // This will compile error since TaskHandle<void> does not have GetReturn()
        // std::cout << "Handle return value:" << h1.GetReturn() << std::endl;
        std::cout << "h1 finished" << std::endl;
    }

    if (h5.IsDown())
    {
        std::cout << "h5 return value:" << h5.GetReturn().value() << std::endl;
    }

    return 0;
}
