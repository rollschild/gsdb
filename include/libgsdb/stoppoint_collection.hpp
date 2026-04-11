#ifndef GSDB_STOPPOINT_COLLECTION_HPP
#define GSDB_STOPPOINT_COLLECTION_HPP

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <libgsdb/types.hpp>
#include <memory>
#include <vector>

#include "libgsdb/error.hpp"

namespace gsdb {
// C++20
template <class T>
concept stoppoint_concept =
    requires { typename T::id_type; } && requires(T t, virt_addr a) {
        { t.at_address(a) } -> std::convertible_to<bool>;
    } && requires(T t) {
        { t.is_enabled() } -> std::convertible_to<bool>;
    } && requires(T t) {
        { t.disable() } -> std::same_as<void>;  // type constraint
    } && requires(T t) {
        { t.enable() } -> std::same_as<void>;  // type constraint
    };

// This is a template - the implementations must go in the header file rather
// than in a .cpp file so that the compiler can generate code from those member
// functions whenever it sees a new specialization of the `stoppoint_collection`
// template
template <stoppoint_concept Stoppoint>
class stoppoint_collection {
   public:
    /**
     * Adds a new stoppoint to the collection
     */
    Stoppoint& push(std::unique_ptr<Stoppoint> bs);
    bool contains_id(typename Stoppoint::id_type id) const;
    bool contains_address(virt_addr address) const;

    /**
     * Determine if there's an enabled stop point in the collction with the
     * given address
     */
    bool enabled_stoppoint_at_address(virt_addr address) const;

    // Need the `typename` keyword for any types that depend on the `Stoppoint`
    // template parameter. It tell compiler that this name refers to a type
    // rather than a value
    Stoppoint& get_by_id(typename Stoppoint::id_type id);
    const Stoppoint& get_by_id(typename Stoppoint::id_type id) const;
    Stoppoint& get_by_address(virt_addr address);
    const Stoppoint& get_by_address(virt_addr address) const;

    void remove_by_id(typename Stoppoint::id_type id);
    void remove_by_address(virt_addr address);

    /**
     * Passed a function/lambda to be called with a reference to each breakpoint
     * site
     */
    template <class F>
    void for_each(F f);
    template <class F>
    void for_each(F f) const;

    std::size_t size() const { return stoppoints_.size(); }
    bool empty() const { return stoppoints_.empty(); }

   private:
    using points_t = std::vector<std::unique_ptr<Stoppoint>>;

    typename points_t::iterator find_by_id(typename Stoppoint::id_type id);
    typename points_t::const_iterator find_by_id(
        typename Stoppoint::id_type id) const;
    typename points_t::iterator find_by_address(virt_addr address);
    typename points_t::const_iterator find_by_address(virt_addr address) const;

    points_t stoppoints_;
};

template <stoppoint_concept Stoppoint>
Stoppoint& stoppoint_collection<Stoppoint>::push(
    std::unique_ptr<Stoppoint> bs) {
    // Remember to move unique pointers with std::move when transferring
    // ownership
    stoppoints_.push_back(std::move(bs));
    return *stoppoints_.back();
}

template <stoppoint_concept Stoppoint>
auto stoppoint_collection<Stoppoint>::find_by_id(typename Stoppoint::id_type id)
    -> typename points_t::iterator {
    return std::find_if(begin(stoppoints_), end(stoppoints_),
                        [=](auto& point) { return point->id() == id; });
}

template <stoppoint_concept Stoppoint>
auto stoppoint_collection<Stoppoint>::find_by_id(
    typename Stoppoint::id_type id) const -> typename points_t::const_iterator {
    return const_cast<stoppoint_collection*>(this)->find_by_id(id);
}

template <stoppoint_concept Stoppoint>
auto stoppoint_collection<Stoppoint>::find_by_address(virt_addr address) ->
    typename points_t::iterator {
    return std::find_if(begin(stoppoints_), end(stoppoints_), [=](auto& point) {
        return point->at_address(address);
    });
}

template <stoppoint_concept Stoppoint>
auto stoppoint_collection<Stoppoint>::find_by_address(virt_addr address) const
    -> typename points_t::const_iterator {
    // `const_cast` casts _away_ the constness
    // `this` is a `const` member, because this is a `const`-qualified member
    // function
    return const_cast<stoppoint_collection*>(this)->find_by_address(address);
}

template <stoppoint_concept Stoppoint>
bool stoppoint_collection<Stoppoint>::contains_id(
    typename Stoppoint::id_type id) const {
    return find_by_id(id) != end(stoppoints_);
}

template <stoppoint_concept Stoppoint>
bool stoppoint_collection<Stoppoint>::contains_address(
    virt_addr address) const {
    return find_by_address(address) != end(stoppoints_);
}

template <stoppoint_concept Stoppoint>
bool stoppoint_collection<Stoppoint>::enabled_stoppoint_at_address(
    virt_addr address) const {
    return contains_address(address) and get_by_address(address).is_enabled();
}

template <stoppoint_concept Stoppoint>
Stoppoint& stoppoint_collection<Stoppoint>::get_by_id(
    typename Stoppoint::id_type id) {
    auto it = find_by_id(id);
    if (it == end(stoppoints_)) {
        error::send("Invalid stoppoint ID!");
    }

    // * for the iterator
    // ** for the unique_ptr
    return **it;
}
template <stoppoint_concept Stoppoint>
const Stoppoint& stoppoint_collection<Stoppoint>::get_by_id(
    typename Stoppoint::id_type id) const {
    return const_cast<stoppoint_collection*>(this)->get_by_id(id);
}

template <stoppoint_concept Stoppoint>
Stoppoint& stoppoint_collection<Stoppoint>::get_by_address(virt_addr address) {
    auto it = find_by_address(address);
    if (it == end(stoppoints_)) {
        error::send("Stoppoint with given address not found!");
    }
    return **it;
}
template <stoppoint_concept Stoppoint>
const Stoppoint& stoppoint_collection<Stoppoint>::get_by_address(
    virt_addr address) const {
    return const_cast<stoppoint_collection*>(this)->get_by_address(address);
}

template <stoppoint_concept Stoppoint>
void stoppoint_collection<Stoppoint>::remove_by_id(
    typename Stoppoint::id_type id) {
    auto it = find_by_id(id);
    (**it).disable();
    stoppoints_.erase(it);
}
template <stoppoint_concept Stoppoint>
void stoppoint_collection<Stoppoint>::remove_by_address(virt_addr address) {
    auto it = find_by_address(address);
    (**it).disable();
    stoppoints_.erase(it);
}

template <stoppoint_concept Stoppoint>
template <class F>
void stoppoint_collection<Stoppoint>::for_each(F f) {
    for (auto& point : stoppoints_) {
        f(*point);
    }
}
template <stoppoint_concept Stoppoint>
template <class F>
void stoppoint_collection<Stoppoint>::for_each(F f) const {
    for (const auto& point : stoppoints_) {
        f(*point);
    }
}

}  // namespace gsdb

#endif
