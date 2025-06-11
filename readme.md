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
Not a coroutine library for maximizing multi-core CPU throughput. ( There are quite some coroutine libraries doing that. ) However, you can still delegate computation to threads and return results into tokoro coroutines.

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

Async<void> awkwardHello(std::string somebody, double holdSeconds)
{
    std::cout << "Hello, ";
    
    // Just like greeting a coworker—sometimes you need a moment to recall their name.
    co_await Wait(holdSeconds);
    
    std::cout << somebody << "!" << std::endl;
}

int main()
{    
    schedular.Start(awkwardHello, "tokoro", 1);
    
    // Simulate a game update loop
    while(true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
        
        schedular.Update();
    }

    return 0;
}
```

## 📚 Tutorial
### Integrate tokoro
**tokoro** is a lightweight, header-only library with zero dependencies. To integrate tokoro into your project, simply copy the header files and `#include "tokoro.h"`. Don't forget to add the library directory to your compiler's include search path.

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

**Lambda could be coroutines** too. To avoid the famous [pitfall](https://quuxplusone.github.io/blog/2019/07/10/ways-to-get-dangling-references-with-coroutines/) of using lambda as C++ coroutines, tokoro::Scheduler caches the start lambda with the coroutine object. However regular lambda reference capturing limitation still applies: make sure the references you captured in lambda can last long enough for the lifetime of the coroutine.

**Coroutines can be nested** with co_await. So you can reuse some generic coroutines.
`co_await awkwardHello("you", 1);`

tokoro also have two helper coroutins: `WaitUntil` and `WaitWhile`. They are two contravers coroutines that will keep checking until input lambda turns true. They are very useful when you want wait for some external signals.
```cpp
co_await WaitUntil([&]()->bool{return launchComplete;});
co_await WaitWhile([&]()->bool{return playingStartCutscene;});
```

In later sections, we will introduce **combination awaiters** `All`&`Any`, with them you can consist even more complex coroutine structures.

### Launch a Coroutine

 `tokoro::Scheduler::Start()` is the only place to launch a **root coroutine**. Root coroutine is a concept we will use throughout this document, which means the coroutine directly started by Scheduler. The coroutines started in side root coroutines is called **nested coroutines**. 

```C++
Handle<T> Scheduler::Start(CoroutineFunc, Args ...)
```

`Scheduler::Start()` is a template function, which takes input a function (or functor) to create coroutine. The CoroutineFunc must returns Async<T>. The rest arguments is the CoroutineFunc's input parameters. The CoroutineFunc and arguments must be matched.

`Start()` returns a `Handle<T>` that allows you to monitor, stop, or extract the coroutine result.

### Coroutine Lifetimes
The Scheduler manages all root coroutine objects it starts. It keeps them alive until,

1. Coroutines running to their end **and** their handle has been destroid.
2. Coroutines running to their end **and** their result has been takend by Handle.TakeResult().
3. Coroutines has been stopped from its handle.
   
Just like normal C++, when a root coroutine destroied, the nested coroutine objects as long as any other objects under the scope of the root coroutine will be destroied recursively. RAII is a recommend way to manage resources since a coroutine can be interrupted by outside manual stop or internal exceptions.

### The Way to Handle It
tokoro::Handle is a simple yet powerful tool to manage coroutines from outside. This section, we will go through each methods of it.

#### Construction&Destruction
`Handle<T>` is a template class, T is the return value of the associated coroutine. Normally, you get a handle by starting a coroutine. The T can get from automatic deduction.
```cpp
auto handle = schedular.Start(DelayAction);
```
You can also declare and initialize a invalid handle by default constructor. Handle dose not have copy constructor, but have a move constructor and a '=' operator overload for move. So there's alway only one handle related to one root coroutine. You can move another handle into a invalid handle.
```cpp
Handle<void> handle; // handle.IsInvalid() == false
handle = schedular.Start(DelayAction); // handle.IsInvalid() == true
```
When Handle destroid, its destructor will release associated coroutine. But scheduler will keep running them until they reach the end. So you can run a coroutine but discard the handle if you don't care how it runs nor its returns.
```cpp
schedular.Start(DelayAction); // Handle is discard, but the coroutine will keep running to its end.
```
However, it's not a recommend practice to headlessly discard most handles. Make sure you know your coroutine's lifetime and status is important to write robust coroutine logic. We will talk about this later in Best Practice section.

#### bool Handle::IsValid()
`IsValid()` tells you whether the handle is get from Scheduler::Start(). No coroutine or Scheduler status related.

#### void Handle::Stop()
`Stop()` is the way to stop suspended coroutine from outside. This only function is the whole tokoro's cancelation system. tokoro do not take the current popular cancel token philosophy, relys on RAII and suspend point to safely stop coroutines. So you don't need to manage these annoying cancel tokens, and passing them down in your nested coroutines recursively. Stop mechanic removed a lot of pain of cancel token system, in most coroutines you don't need to do anything. ( Focus on single thread helps us to achieve this.) However for some coroutines, you still need to take care your resource releases and state rollback with RAII. We will talk in detail in the Best Practice section.

#### std::optional<AsyncState> Handle::GetState()
`GetState()` actually consist two layer of information. When it returns std::nullopt, it means either the handle is invalid or the associated scheduler is destroyed. To distinguish between this two state, you only need to get confirm from `IsInvilad()`. On the other hand, AsyncState tells you the state of the coroutine,

* Running: every coroutine starts with Running state. But you shouldn't a assume every new started Handle will returns Running immediatly. Because the coroutine might already reached its end in the Scheduler::Start().
* Succeed: means a coroutine finished execution without exceptions.
* Failed: a exception throw out in executing this coroutine and it's not catch internally. Call `Hanle::TakeResult()` will rethrow the exception to outside.
* Stopped: the coroutine is stopped manually by Handle::Stop().

**Note**: You can get a AsyncState from the handle, does not mean the underlying coroutine object is still there. It might be already destroyed, refer to Coroutine Lifetimes.

#### bool Handle::IsRunning()
Sometimes keep writing
```cpp
auto state = handle.GetState();
if (state.has_value() && *state == AsyncState::Running)
    ...
```
is just too annoying. So `IsRunning()` is the short-term of above. 

#### std::optional<T> Handle::TakeResult()
`TakeResult()` is a one time call. As the name says, it will take out the return of the coroutine return it to caller. So if your coroutine do produced a return, the first call will give you the result, the second call will returns std::nullopt.
However, when the coroutine is still running, TakeResult will also returns nullopt. To tell whether it's already taken, you can call `IsRunning()` to make sure.
If a coroutine is ended with a unhandled exception, TakeResult() will throw it out. The throw is one time too.

### Waiters
Currently there are only 3 kind of awaiters you can explicitly used in tokoro (there are some implicity awaiters, but you don't need to care as a library user.)

#### Wait
`Wait` is the most important awaiter for tokoro. There's two way to use `Wait`:

1. `co_await Wait();` will suspend the current coroutine until next Scheduler::Update().
2.  `co_await Wait(double sec);` will suspend the current coroutine and add it to scheduler's time queue. In which the coroutine will get resumed `sec` seconds later in Scheduler::Update().

Note internally the time queue only check and resume coroutines who need to get resumed in current time point. So performance wise, a `Wait(max_double)` only cost the queue insert time and a few bytes of queue node memory, no other impact in regular update.
You can also specify which custom update type and time type for the `Wait(UpdateType, TimeType)`.  Please check Custom Update Types if you want use your own update types and timers.

#### All
All will wait for all coroutines it holds to finish. The return is a tuple which holds all returns of each sub-coroutines.
```cpp
auto [terrain, texture] = co_await All(GenerateTerrain(), LoadTexture(path));
```

#### Any
Any wait until any of its sub-coroutine finish. The return is a tuple with std::optional<T>, T is each coroutines' return type. You can check tuple's element one by one to find out which coroutine returned first.
```cpp
auto [texture, timeout] = co_await All(LoadMesh(meshPath), Delay(10));
if(timeout.has_value())
{
    LOG_Error("Timeout after 10 second when try to load %s", meshPath);
}
```
Note that, **when any of the coroutine returned, all the other coroutines are immdediatly stopped** by this awaiter.
If you do need other coroutines to keep running after Any awaiter, you have to start them as root coroutines, and wait for their handle instead.
```cpp
Handle<Mesh> handle1 = GlobalScheduler().Start(LoadMesh, mesh1);
Handle<Mesh> handle2 = GlobalScheduler().Start(LoadMesh, mesh2);
// You can probably co_await WaitWhile([&](){return handle1.IsRunning() || handle2.IsRunning()});
// But this is just a example.
auto [handle1Finished, handle2Finished] = co_await All(WaitWhile([&](){return handle1.IsRunning()}), WaitWhile([&](){return handle2.IsRunning()}));
if(handle1Finished.has_value())
{
    LOG("%s loaded first", mesh1);
}
else
{
    LOG("%s loaded first", mesh2);
}
```




