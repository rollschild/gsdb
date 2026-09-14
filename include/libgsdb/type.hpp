#ifndef GSDB_TYPE_HPP
#define GSDB_TYPE_HPP

#include <cstddef>
#include <libgsdb/dwarf.hpp>
#include <optional>
#include <string_view>
#include <utility>

#include "libgsdb/detail/dwarf.h"

namespace gsdb {

class type {
   public:
    type(die die) : die_(std::move(die)) {}

    die get_die() const { return die_; }
    std::size_t byte_size() const;
    bool is_char_type() const;

    /**
     * Strip layers of qualifiers, pointers, or reference types off of a type.
     */
    template <int... Tags>  // takes any number of `int` template arguments,
                            // stored in `Tag` - a **parameter pack**
    type strip() const {
        auto ret = *this;
        auto tag = ret.get_die().abbrev_entry()->tag;
        // **fold expression**: runs some expression across _every element_ in a
        // parameter pack, and collects the result using a binary operator.
        // The outcome will indicate whether any tag in the given set of
        // template arguments matches the tag we’re currently examining
        while (/* fold expression */ ((tag == Tags) or ...)) {
            ret = ret.get_die()[DW_AT_type].as_type().get_die();
            tag = ret.get_die().abbrev_entry()->tag;
        }
        return ret;
    }

    /**
     * Strip just qualifiers and typedefs from type
     */
    type strip_cv_typedef() const {
        return strip<DW_TAG_const_type, DW_TAG_volatile_type, DW_TAG_typedef>();
    }
    /**
     * Strip qualifiers, typedefs, and reference types from type
     */
    type strip_cvref_typedef() const {
        return strip<DW_TAG_const_type, DW_TAG_volatile_type, DW_TAG_typedef,
                     DW_TAG_reference_type, DW_TAG_rvalue_reference_type>();
    }
    /**
     * Strip qualifiers, typedefs, reference types, and pointer types from type
     */
    type strip_all() const {
        return strip<DW_TAG_const_type, DW_TAG_volatile_type, DW_TAG_typedef,
                     DW_TAG_reference_type, DW_TAG_rvalue_reference_type,
                     DW_TAG_pointer_type>();
    }

   private:
    std::size_t compute_byte_size() const;

    die die_;

    // mutable because it's a cache variable that `const` member function can
    // modify
    mutable std::optional<std::size_t> byte_size_;
};

}  // namespace gsdb

#endif  // !GSDB_TYPE_HPP
