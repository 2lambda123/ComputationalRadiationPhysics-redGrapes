
#pragma once

#include <mutex>
#include <condition_variable>
#include <boost/graph/adjacency_list.hpp>
#include <rmngr/graph/refined_graph.hpp>
#include <rmngr/graph/precedence_graph.hpp>
#include <rmngr/graph/util.hpp>

#include <rmngr/thread_schedule.hpp>

namespace rmngr
{
    
template <
    typename T_Task,
    typename T_Graph = boost::adjacency_list<
        boost::setS,
        boost::vecS,
        boost::bidirectionalS,
        T_Task*
    >
>
class SchedulingGraph
{
public:
    using P_Graph = T_Graph;
    using Task = T_Task;

    struct Event
    {
    private:
        std::mutex mutex;
        std::condition_variable cv;
        bool state;

    public:
        Event()
            : state( false )
        {}

        auto lock()
        {
            return std::unique_lock< std::mutex >( mutex );
        }

        void notify()
        {
            {
                auto l = lock();
                state = true;
            }
            cv.notify_all();
        }

        void wait()
        {
            auto l = lock();
            cv.wait( l, [this]{ return state; } );
        }
    };

    using EventGraph = boost::adjacency_list<
        boost::listS,
        boost::listS,
        boost::bidirectionalS
    >;
    using EventID = typename boost::graph_traits< EventGraph >::vertex_descriptor;
    using TaskID = typename boost::graph_traits< P_Graph >::vertex_descriptor;

    std::mutex mutex;
    EventGraph m_graph;
    std::unordered_map< EventID, Event > events;
    std::unordered_map< Task* , EventID > before_events;
    std::unordered_map< Task* , EventID > after_events;

    struct Job
    {
        Task * task;

        void operator() ()
        {
            (*task)();
        }
    };

    using ThreadSchedule = rmngr::ThreadSchedule< Job >;

    RefinedGraph< T_Graph > & precedence_graph;
    std::vector< ThreadSchedule > schedule;

    EventID null_id;

    SchedulingGraph( RefinedGraph< T_Graph > & precedence_graph, int n_threads )
        : precedence_graph( precedence_graph )
        , schedule( n_threads )
        , finishing( false )
    {
        std::lock_guard< std::mutex > lock( mutex );
        null_id = boost::add_vertex( m_graph );

        precedence_graph.set_notify_hook( [this]{ this->notify(); } );
    }

    std::atomic_bool finishing;
    void finish()
    {
        finishing = true;
        notify();
    }

    bool empty()
    {
        std::lock_guard< std::mutex > lock( mutex );
        return finishing && (boost::num_vertices( m_graph ) == 1);
    }

    Job make_job( Task * task )
    {
        EventID before_event = make_event();
        EventID after_event = make_event();
        //std::cerr << "JOB Create task="<<task<<" preevent="<<before_event<<", postevent="<<after_event<<std::endl;

        before_events[ task ] = before_event;
        after_events[ task ] = after_event;

        task->hook_before( [this, before_event]{ finish_event( before_event ); events[before_event].wait(); } );
        task->hook_after( [this, after_event]{ finish_event( after_event ); } );

        std::lock_guard< std::mutex > lock( mutex );
        auto ref = precedence_graph.find_refinement_containing( task );
        if( ref )
        {
            auto l = ref->lock();
            if( auto task_id = graph_find_vertex( task, ref->graph() ) )
            {
                for(
                    auto it = boost::in_edges( *task_id, ref->graph() );
                    it.first != it.second;
                    ++ it.first
                )
                {
                    auto v_id = boost::source( *(it.first), ref->graph() );
                    auto precending_task = graph_get( v_id, ref->graph() );
                    //std::cerr << "depend on task " << precending_task << ", ev= " << after_events[precending_task]<<std::endl;
                    boost::add_edge( after_events[ precending_task ], before_event, m_graph );
                }
            }

            if( ref->parent )
            {
                //std::cerr << "SCHEDGRAPH: make edge to parent: " << after_events[ref->parent] << std::endl;
                boost::add_edge( after_event, after_events[ ref->parent ], m_graph );
            }
        }

        return Job{ task };
    }

    template <typename Refinement>
    void update_vertex( Task * task )
    {
        auto ref = dynamic_cast<Refinement*>(this->precedence_graph.find_refinement_containing( task ));
        std::vector<Task*> selection = ref->update_vertex( task );

        for( Task * other_task : selection )
        {
            boost::remove_edge( after_events[task], before_events[other_task], m_graph );
            notify_event( before_events[other_task] );
        }

        notify();
    }

    void consume_job( std::function<bool()> const & pred = []{ return false; } )
    {
        /*
        std::cout << "consume job" << std::endl;
        if( schedule[thread::id].needs_job() )
            notify();
        */
        schedule[ thread::id ].consume( [this, pred]{ return empty() || pred(); } );
    }

    std::experimental::optional<Task*> get_current_task()
    {
        if( std::experimental::optional<Job> job = schedule[ thread::id ].get_current_job() )
            return std::experimental::optional<Task*>( job->task );
        else
            return std::experimental::nullopt;
    }

    EventID make_event()
    {
        std::lock_guard< std::mutex > lock( mutex );
        EventID event_id = boost::add_vertex( m_graph );
        events.emplace( std::piecewise_construct, std::forward_as_tuple(event_id), std::forward_as_tuple() );

        boost::add_edge( null_id, event_id, m_graph );

        return event_id;
    }

    std::function<void()> notify_hook;
    void set_notify_hook( std::function<void()> h )
    {
        notify_hook = h;
    }

    void notify()
    {
        notify_hook();
        //if( empty() )
        {
            for( auto & thread : schedule )
                thread.notify();
        }
    }

    bool notify_event( EventID id )
    {
        std::unique_lock< std::mutex > lock( mutex );
        //std::cerr << "EVENT Notify " << id << std::endl;
        if( boost::in_degree( id, m_graph ) == 0 )
        {
            events[ id ].notify();

            // collect events to propagate to before to not invalidate the iterators in recursion
            std::vector< EventID > out;
            for( auto it = boost::out_edges( id, m_graph ); it.first != it.second; it.first++ )
                out.push_back( boost::target( *it.first, m_graph ) );

            //std::cerr << "SCHEDGRAPH: remove " << id << std::endl;
            boost::clear_vertex( id, m_graph );
            boost::remove_vertex( id, m_graph );

            lock.unlock();

            // propagate
            for( EventID e : out )
                notify_event( e );

            return true;
        }
        else
            return false;
    }

    void finish_event( EventID id )
    {
        //std::cerr << "EVENT Finish " << id << std::endl;
        {
            auto cv_lock = events[id].lock();
            std::lock_guard< std::mutex > graph_lock( mutex );
            boost::remove_edge( null_id, id, m_graph );
        }

        if( notify_event( id ) )
            notify();
    }
}; // class SchedulingGraph
    
} // namespace rmngr

