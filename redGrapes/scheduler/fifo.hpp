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
    std::atomic_flag active = ATOMIC_FLAG_INIT;

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
            auto task_id = (*task_vertex)->task->task_id;
            //spdlog::trace("FIFO: run task {}", task_id);

            mgr.get_scheduling_graph()->task_start( task_id );

            running.enqueue( *task_vertex );

            mgr.current_task() = task_vertex;
            bool finished = (*(*task_vertex)->task->impl)();
            mgr.current_task() = std::nullopt;

            if(auto children = (*task_vertex)->children)
                while(auto new_task = (*children)->next())
                    mgr.activate_task(*new_task);
            while( mgr.activate_next() );

            if( finished )
                mgr.get_scheduling_graph()->task_end( task_id );

            return true;
        }
        else
            return false;
    }

    // precedence graph must be locked
    bool activate_task( TaskVertexPtr task_vertex )
    {
        auto task_id = task_vertex->task->task_id;
        if(mgr.get_scheduling_graph()->is_task_ready(task_id))
        {
            if(!task_vertex->task->active.test_and_set())
            {
                spdlog::trace("FIFO: task {} is ready", task_id);
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
        //spdlog::trace("FIFO::get_job()");
        
        TaskVertexPtr task_vertex;

        while( mgr.activate_next() );
        if(ready.try_dequeue(task_vertex))
            return task_vertex;

        update_running_spaces();

        while( mgr.activate_next() );
        if(ready.try_dequeue(task_vertex))
            return task_vertex;

        update_main_space();

        while( mgr.activate_next() );
        if(ready.try_dequeue(task_vertex))
            return task_vertex;

        //spdlog::trace("FIFO::get_job(): no job available");
        return std::nullopt;
    }

    void update_running_spaces()
    {
        //spdlog::trace("FIFO::update_running_spaces()");

        std::vector< TaskVertexPtr > buf;

        TaskVertexPtr task_vertex;
        while(running.try_dequeue(task_vertex))
        {
            TaskID task_id = task_vertex->task->task_id;

            if(auto children = task_vertex->children)
            {
                while(auto new_task = (*children)->next())
                {
                    mgr.activate_task(*new_task);
                    /* this optimization needs to be implemented differently
                    if( ready.size_approx() > 0 )
                        break;
                    */
                }

                while( mgr.activate_next() );
            }

            if(mgr.get_scheduling_graph()->is_task_finished(task_id))
                mgr.remove_task(task_vertex);
            else
                if(mgr.get_scheduling_graph()->is_task_running(task_id))
                    buf.push_back(task_vertex);
            //  else the task is paused
            // and will be enqueued again through activate_task()
            // from scheduling graph when the event is reached
        }

        for( auto task_vertex : buf )
            running.enqueue(task_vertex);
    }

    void update_main_space()
    {
        //spdlog::trace("FIFO::update_main_space()");
        while(auto task_vertex = mgr.get_main_space()->next())
        {
            mgr.activate_task(*task_vertex);
            /* same here
            if(ready.size_approx() > 0)
                break;
            */
        }
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


