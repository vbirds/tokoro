#pragma once

#include "defines.h"

#include <cassert>
#include <set>

namespace tokoro::internal
{

template <typename T>
class TimeQueue
{
private:
    struct Node
    {
        double   time;
        uint32_t seq;
        uint32_t frame;
        T        value;
    };

    struct Comp
    {
        bool operator()(const Node& a, const Node& b) const noexcept
        {
            if (a.time != b.time)
                return a.time < b.time;
            else
                return a.seq < b.seq;
        }
    };

    using SetType = std::multiset<Node, Comp>;

public:
    using Iterator = typename SetType::const_iterator;

    TimeQueue()
    {
        mUpdatePtr = mSet.end();
    }

    void Clear()
    {
        mSet.clear();
        mAddOrder   = 0;
        mAddFrame   = 0;
        mUpdatePtr  = mSet.end();
        mCurExeTime = 0;
    }

    Iterator AddTimed(const double time, const T& e)
    {
        return AddImpl(time, e);
    }

    void Remove(Iterator iter)
    {
        if (iter == mUpdatePtr)
        {
            mUpdatePtr = mSet.erase(mUpdatePtr);
        }
        else
        {
            mSet.erase(iter);
        }
    }

    T Pop()
    {
        // User should CheckUpdate() before Pop()
        assert(mUpdatePtr != mSet.end());

        T ret = std::move(mUpdatePtr->value);

        mUpdatePtr = mSet.erase(mUpdatePtr);

        return ret;
    }

    bool CheckUpdate() noexcept
    {
        MoveToNext();
        return !mSet.empty() && mSet.end() != mUpdatePtr;
    }

    void SetupUpdate(double exeTime)
    {
        mAddFrame++;
        mAddOrder   = 0;
        mUpdatePtr  = mSet.begin();
        mCurExeTime = exeTime;
    }

private:
    void MoveToNext()
    {
        while (mUpdatePtr != mSet.end())
        {
            const Node& node = *mUpdatePtr;

            if (node.time > mCurExeTime)
            {
                // Next item is for future to update. Stop.
                mUpdatePtr = mSet.end();
                break;
            }

            if (node.frame == mAddFrame)
            {
                ++mUpdatePtr;
            }
            else
            {
                // Found update
                break;
            }
        }
    }

    Iterator AddImpl(double time, const T& e)
    {
        Node node{time, mAddOrder++, mAddFrame, e};
        return mSet.insert(std::move(node));
    }

    SetType  mSet;
    uint32_t mAddOrder = 0;
    uint32_t mAddFrame = 0;
    Iterator mUpdatePtr;
    double   mCurExeTime;
};

} // namespace tokoro::internal