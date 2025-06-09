# tokoro

**tokoro** is a lightweight, header-only coroutine library designed for modern C++20. Built for game, GUI apps, and any update-driven application. It provides efficient and powerful coroutine scheduling in a single thread—ideal for real-time environments.

### ✨ Highlights

* 🔄 **Update-based scheduling** – perfect for frame-driven systems like games or UI rendering.
* ⚡ **Lightweight & efficient** – low memory footprint and minimal scheduling overhead.

* 🧩 **Plug-and-play** – just include the header, setup update call, and you’re ready to go.

* 🔧 **Highly customizable** – plug in your own update phases or timers.

* 🧵 **Single-threaded** – less power comes with less responsbility. Sometimes we just want to be safe.

### 🎯 Project Goal

Designed to be lightweight and modular, yet expressive enough to handle complex coroutine flows.

### 🚫 Non-Goals

Not a coroutine library for maximizing multi-core CPU throughput. ( There are quite some coroutine libraries like that. ) However, you can still delegate computation to threads and return results into tokoro coroutines.

## 📚 Tutorial

### Hello tokoro

Here's a short example of how tokoro is used.

```C++
// This example is compilable, make sure you enable the C++ 20 flag.
#include "tokoro.h"
#include <iostream>
#include <thread>
#include <chrono>

using namespace tokoro;
Scheduler schedular;

Async<void> ColleagueHello(std::string somebody, double holdSeconds)
{
    std::cout << "Hello, ";
    
    // Just like greeting a coworker—sometimes you need a moment to recall their name.
    co_await Wait(holdSeconds);
    
    std::cout << somebody << "!" << std::endl;
}

int main()
{    
    schedular.Start(ColleagueHello, "tokoro", 1);
    
    // Simulate a game update loop
    while(true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
        
        schedular.Update();
    }

    return 0;
}
```

### Decide Scheduler Scope

tokoro::Scheduler can be used locally or globally.

#### Use Scheduler Locally

```c++
// The concept of an Entity is present in nearly every game engine. In Unreal Engine, 
// it is referred to as an Actor, whereas in Unity, it is known as a GameObject. 
// They all get simular update calls from the engine.
class Entity
{
public:
    ...
    virtual void Update()
    {
        schedular.Update();
    }
private:
    ...
    tokoro::Scheduler schedular;
}
```

#### Use Global Singleton Scheduler

```c++
static tokoro::Scheduler& GlobalScheduler() 
{
    static tokoro::Scheduler s;
    return s;
}
...
void Engine::Update()
{
    ...
    GlobalScheduler().Update();
}
```

### Writing a Coroutine

A tokoro coroutine contains at leat 2 elements:

1. **Returns Async<T>**. T could be anything support copy or move.
2. Have **at leat one** co_await/co_return.

Below is a bare bone example.

```C++
Async<int> Sqaure(int value) 
{
    co_await tokoro::Wait();
    co_return value * value;
}
```

Lambda could be coroutine too. To avoid the famous [pitfall](https://quuxplusone.github.io/blog/2019/07/10/ways-to-get-dangling-references-with-coroutines/) of using lambda as C++ coroutines, tokoro::Scheduler caches the start lambda with the coroutine object. So feel free to launch lambda coroutines with Scheduler.

### Launch a Coroutine

 `tokoro::Scheduler::Start()` is the only place to launch a **root coroutine**. Root coroutine is a concept we will use throughout this document, which means the coroutine directly started by Scheduler. The coroutines started in side root coroutines is called **nested coroutines**. 

```C++
Handle<T> Scheduler::Start(CoroutineFunc, Args ...)
```

`Scheduler::Start()` is a template function, which takes input a function (or functor) to create coroutine. The CoroutineFunc must returns Async<T>. The rest arguments is the CoroutineFunc's input parameters. The CoroutineFunc and arguments must be matched.

`Start()` returns a `Handle<T>` that allows you to monitor, stop, or extract the coroutine result.

### Coroutine Lifetimes



