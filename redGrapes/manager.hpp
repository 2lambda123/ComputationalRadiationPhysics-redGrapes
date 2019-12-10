/* Copyright 2019 Michael Sippel
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#pragma once

#include <shared_mutex>
#include <unordered_map>
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/graph_traits.hpp>

#include <redGrapes/thread/thread_dispatcher.hpp>
#include <redGrapes/thread/thread_local.hpp>
#include <redGrapes/graph/scheduling_graph.hpp>
#include <redGrapes/graph/recursive_graph.hpp>
#include <redGrapes/graph/precedence_graph.hpp>
#include <redGrapes/graph/util.hpp>
#include <redGrapes/task/delayed_functor.hpp>
#include <redGrapes/task/working_future.hpp>
#include <redGrapes/task/task.hpp>

#include <redGrapes/property/inherit.hpp>

#include <redGrapes/scheduler/fifo.hpp>

#include <redGrapes/property/trait.hpp>

namespace redGrapes
{

template <typename T>
struct DefaultEnqueuePolicy
{
    static bool is_serial(T const & a, T const & b) { return true; }
    static void assert_superset(T const & super, T const & sub) {}
};

template <
    typename T_TaskProperties = TaskProperties<>,
    typename EnqueuePolicy = DefaultEnqueuePolicy< T_TaskProperties >,
    template <typename, typename, typename> class Scheduler = FIFOScheduler
>
class Manager
{
public:
    using TaskID = unsigned int;
    static TaskID gen_task_id()
    {
        static TaskID id = 0;
        static std::mutex m;
        std::unique_lock<std::mutex> l(m);
        return id++;
    }
    
    struct TaskPtr;
    struct WeakTaskPtr;

    struct TaskProperties : T_TaskProperties
    {
        TaskID task_id;
        std::experimental::optional< WeakTaskPtr > parent;

        TaskProperties( T_TaskProperties const & p )
            : T_TaskProperties( p )
            , task_id(gen_task_id())
        {}
    };

    struct Task : TaskProperties
    {
        std::shared_ptr< TaskImplBase > impl;

        template <typename F>
        Task( F && f, T_TaskProperties prop )
            : TaskProperties(prop)
            , impl( new FunctorTask<F>(std::move(f)) )
        {}

        void hook_before( std::function<void()> hook )
        {
            impl->before_hooks.push_back( hook );
        }

        void hook_after( std::function<void()> hook )
        {
            impl->after_hooks.push_back( hook );
        }
    };

    using PrecedenceGraph = QueuedPrecedenceGraph<Task, EnqueuePolicy>;

    struct WeakTaskPtr
    {
        std::weak_ptr< PrecedenceGraph > graph;
        typename PrecedenceGraph::VertexID vertex;

        WeakTaskPtr( TaskPtr const & other )
            : graph( other.graph )
            , vertex( other.vertex )
        {}

        Task & get() const
        {
            return graph_get(vertex, graph.lock()->graph()).first;
        }

        Task & locked_get() const
        {
            auto g = this->graph.lock();
            auto lock = g->shared_lock();
            return graph_get(vertex, g->graph()).first;
        }
    };
    
    struct TaskPtr
    {
        std::shared_ptr< PrecedenceGraph > graph;
        typename PrecedenceGraph::VertexID vertex;

        Task & get() const
        {
            return graph_get(vertex, graph->graph()).first;
        }

        Task & locked_get() const
        {
            auto lock = graph->shared_lock();
            return graph_get(vertex, graph->graph()).first;
        }
    };

    std::shared_ptr<PrecedenceGraph> main_graph;
    Scheduler< TaskID, TaskPtr, PrecedenceGraph > scheduler;
    ThreadDispatcher< Scheduler<TaskID, TaskPtr, PrecedenceGraph> > thread_dispatcher;

    template <typename... Args>
    static inline void pass(Args&&...) {}

    struct PropBuildHelper
    {
        typename T_TaskProperties::Builder & builder;

        template <typename T>
        inline int build (T const & x)
        {
            trait::BuildProperties<T>::build(builder, x);
            return 0;
        }
    };

public:
    using EventID = typename Scheduler<TaskID, TaskPtr, PrecedenceGraph>::EventID;

    Manager( int n_threads = std::thread::hardware_concurrency() )
        : main_graph( std::make_shared<PrecedenceGraph>() )
        , scheduler( main_graph, n_threads )
        , thread_dispatcher( scheduler, n_threads )
    {}

    ~Manager()
    {
        scheduler.finish();
        thread_dispatcher.finish();
    }

    auto & getScheduler()
    {
        return scheduler;
    }

    template < typename Callable, typename... Args >
    auto emplace_task( Callable && f, typename T_TaskProperties::Builder builder, Args&&... args )
    {
        PropBuildHelper build_helper{ builder };
        pass( build_helper.template build<Args>(args)... );

        auto impl = std::bind(f, std::forward<Args>(args)...);

        auto delayed = make_delayed_functor( std::move(impl) );
        auto result = make_working_future( std::move(delayed.get_future()), scheduler );
        this->push( Task(std::move(delayed), builder ) );
        return result;
    }

    template < typename Callable, typename... Args >
    auto emplace_task( Callable && f, Args&&... args )
    {
        typename TaskProperties::Builder builder;
        return emplace_task( f, builder, std::forward<Args>(args)... );
    }

    /**
     * Enqueue a child of the current task.
     */
    void push( Task && task )
    {
        if( auto parent = scheduler.get_current_task() )
            task.parent = WeakTaskPtr(*parent);

        unsigned int scope_level = thread::scope_level + 1;
        task.hook_before([scope_level]{ thread::scope_level = scope_level; });

        auto task_ptr = scheduler.add_task( task, this->get_current_graph() );
    }

    std::experimental::optional<TaskID> get_current_task_id( void )
    {
        if( auto task_ptr = scheduler.get_current_task() )
            return task_ptr->locked_get().task_id;
        else
            return std::experimental::nullopt;
    }

    void reach_event( EventID event_id )
    {
        scheduler.reach_event( event_id );
    }

    std::experimental::optional<EventID> create_event()
    {
        if( auto task_id = get_current_task_id() )
            return scheduler.scheduling_graph.add_post_dependency( *task_id );
        else
            return std::experimental::nullopt;
    }

    std::shared_ptr<PrecedenceGraph>
    get_current_graph( void )
    {
        if( auto task_ptr = scheduler.get_current_task() )
        {
            auto parent_graph = task_ptr->graph;
            auto l = parent_graph->shared_lock();
            auto g = graph_get(task_ptr->vertex, parent_graph->graph()).second;
            l.unlock();

            if( !g )
            {
                auto new_graph = std::make_shared<PrecedenceGraph>( parent_graph, task_ptr->vertex );
                parent_graph->add_subgraph( task_ptr->vertex, new_graph );
                return new_graph;
            }
            else
                return std::dynamic_pointer_cast<PrecedenceGraph>(g);
        }

        return this->main_graph;
    }

    void update_properties( typename TaskProperties::Patch const & patch )
    {
        if( auto task_ptr = scheduler.get_current_task() )
        {
            task_ptr->locked_get().apply_patch( patch );
            scheduler.update_vertex( *task_ptr );
        }
        else
            throw std::runtime_error("update_properties: currently no task running");
    }

    std::vector<TaskProperties> backtrace()
    {
        std::vector<TaskProperties> bt;

        std::experimental::optional< WeakTaskPtr > task_ptr;

        if( auto parent = scheduler.get_current_task() )
            task_ptr = WeakTaskPtr( *parent );

        while( task_ptr )
        {
            TaskProperties task = task_ptr->locked_get();
            bt.push_back( task );

            if( task.parent )
                task_ptr = WeakTaskPtr(*task.parent);
            else
                task_ptr = std::experimental::nullopt;
        }

        return bt;
    }

    template< typename ImplCallable, typename PropCallable >
    struct TaskFactoryFunctor
    {
        Manager & mgr;
        ImplCallable impl;
        PropCallable prop;

        template <typename... Args>
        auto operator() (Args&&... args)
        {
            return mgr.emplace_task(impl, prop( std::forward<Args>(args)... ), std::forward<Args>(args)...);
        }
    };

    struct DefaultPropFunctor
    {
        template < typename... Args >
        typename T_TaskProperties::Builder operator() (Args&&...)
        {
            return typename T_TaskProperties::Builder();
        }
    };

    template < typename ImplCallable, typename PropCallable = DefaultPropFunctor >
    auto make_functor( ImplCallable && impl, PropCallable && prop = DefaultPropFunctor{}, typename T_TaskProperties::Builder builder = typename T_TaskProperties::Builder() )
    {
        return TaskFactoryFunctor< ImplCallable, PropCallable >{ *this, impl, prop };
    }
};

} // namespace redGrapes