
/**
 * @file rmngr/access/ioaccess.hpp
 */

#pragma once

#include <boost/graph/adjacency_matrix.hpp>
#include <rmngr/dependency_manager.hpp>

namespace rmngr
{
namespace access
{

struct IOAccess
{
    enum
    {
        root,
        read,
        write,
        aadd,
        amul,
    } mode;

    static bool
    is_serial(
        IOAccess a,
        IOAccess b
    )
    {
        using Graph = boost::adjacency_matrix<boost::undirectedS>;
        struct Initializer
        {
            void operator() (Graph& g) const
            {
                // atomic operations
                boost::add_edge(root, read, g);
                boost::add_edge(root, aadd, g);
                boost::add_edge(root, amul, g);

                // non-atomic
                boost::add_edge(root, write, g);
                boost::add_edge(write, write, g);
            };
        }; // struct Initializer

        static StaticDependencyManager<
            Graph,
            Initializer,
            5
        > const m;
        return m.is_serial(a.mode, b.mode);
    }

}; // struct IOAccess

} // namespace access

} // namespace rmngr
