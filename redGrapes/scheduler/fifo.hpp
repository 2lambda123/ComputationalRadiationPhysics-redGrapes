/* Copyright 2019-2021 Michael Sippel
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <mutex>
#include <unordered_set>
#include <optional>
#include <atomic>

#include <moodycamel/concurrentqueue.h>

#include <redGrapes/scheduler/scheduler.hpp>
#include <redGrapes/thread_local.hpp>
#include <redGrapes/imanager.hpp>

namespace redGrapes
{
namespace scheduler
{

struct FIFOSchedulerProp
{
    std::atomic_flag in_activation_queue = ATOMIC_FLAG_INIT;
    std::atomic_flag in_ready_list = ATOMIC_FLAG_INIT;

    FIFOSchedulerProp()
    {
    }
    FIFOSchedulerProp(FIFOSchedulerProp&& other)
    {
    }
    FIFOSchedulerProp(FIFOSchedulerProp const& other)
    {
    }
    FIFOSchedulerProp& operator=(FIFOSchedulerProp const& other)
    {
        return *this;
    }

    template<typename PropertiesBuilder>
    struct Builder
    {
        PropertiesBuilder& builder;

        Builder(PropertiesBuilder& b) : builder(b)
        {
        }
    };

    struct Patch
    {
        template <typename PatchBuilder>
        struct Builder
        {
            Builder( PatchBuilder & ) {}
        };
    };

    void apply_patch( Patch const & ) {};
};

template < typename Task >
struct FIFO : public IScheduler< Task >
{
    using TaskVertexPtr = std::shared_ptr<PrecedenceGraphVertex<Task>>;

    IManager<Task>& mgr;

    moodycamel::ConcurrentQueue< TaskVertexPtr > ready;
    moodycamel::ConcurrentQueue< TaskVertexPtr > running;

    FIFO(IManager<Task>& mgr) : mgr(mgr)
    {
    }

    //! returns true if a job was consumed, false if queue is empty
    bool consume()
    {
        if( auto task_vertex = get_job() )
        {
            auto & task = *(*task_vertex)->task;
            auto task_id = task.task_id;

            task.pre_event->reach();

            mgr.current_task() = task_vertex;
            bool finished = (*(*task_vertex)->task->impl)();
            mgr.current_task() = std::nullopt;

            if(finished)
            {
                auto pe = task.post_event;
                pe->reach();

                mgr.get_scheduler()->notify();
            }
            else
            {
                task.in_activation_queue.clear();
                task.in_ready_list.clear();

                task.pause( *(task.impl->event) );
            }

            return true;
        }
        else
            return false;
    }

    // precedence graph must be locked
    bool activate_task( TaskVertexPtr task_vertex )
    {
        auto & task = *task_vertex->task;
        auto task_id = task.task_id;

        if( task.is_ready() )
        {
            if(!task.in_ready_list.test_and_set())
            {
                ready.enqueue(task_vertex);
                mgr.get_scheduler()->notify();

                return true;
            }
        }

        return false;
    }

private:
    std::optional<TaskVertexPtr> get_job()
    {
        if( auto task_vertex = try_next_task() )
            return task_vertex;
        else
        {
            mgr.update_active_task_spaces();
            return try_next_task();
        }
    }

    std::optional<TaskVertexPtr> try_next_task()
    {
        do
        {
            TaskVertexPtr task_vertex;
            if(ready.try_dequeue(task_vertex))
                return task_vertex;
        }
        while( mgr.activate_next() );

        return std::nullopt;
    }
};

/*! Factory function to easily create a fifo-scheduler object
 */
template <
    typename Task
>
auto make_fifo_scheduler(
    IManager< Task > & m
)
{
    return std::make_shared<
               FIFO< Task >
           >(m);
}

} // namespace scheduler

} // namespace redGrapes


template<>
struct fmt::formatter<redGrapes::scheduler::FIFOSchedulerProp>
{
    constexpr auto parse(format_parse_context& ctx)
    {
        return ctx.begin();
    }

    template<typename FormatContext>
    auto format(redGrapes::scheduler::FIFOSchedulerProp const& prop, FormatContext& ctx)
    {
        auto out = ctx.out();
        format_to(out, "\"active\": 0");
        return out;
    }
};


