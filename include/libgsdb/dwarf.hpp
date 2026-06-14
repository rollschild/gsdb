#ifndef GSDB_DWARF_HPP
#define GSDB_DWARF_HPP

#include <libgsdb/detail/dwarf.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "libgsdb/types.hpp"

namespace gsdb {
class compile_unit;
class elf;
class dwarf;
class die;

class range_list {
   public:
    range_list(const compile_unit* cu, span<const std::byte> data,
               file_addr base_address)
        : cu_(cu), data_(data), base_address_(base_address) {}

    struct entry {
        file_addr low;
        file_addr high;

        bool contains(file_addr addr) const {
            return low <= addr and addr < high;
        }
    };

    class iterator;
    iterator begin() const;
    iterator end() const;

    // whether a given address is in any entry of the range list
    bool contains(file_addr addr) const;

   private:
    const compile_unit* cu_;
    span<const std::byte> data_;
    file_addr base_address_;
};

class range_list::iterator {
   public:
    // type aliases that enable us to use the iterator type with standard
    // algorithms
    using value_type = entry;
    using reference = const entry&;
    using pointer = const entry*;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;  // Multipass

    iterator(const compile_unit* cu, span<const std::byte> data,
             file_addr base_address);

    iterator() = default;
    iterator(const iterator&) = default;
    iterator& operator=(const iterator&) = default;

    const entry& operator*() const { return current_; }
    const entry* operator->() const { return &current_; }

    bool operator==(iterator rhs) const { return pos_ == rhs.pos_; }
    bool operator!=(iterator rhs) const { return pos_ != rhs.pos_; }

    iterator& operator++();
    iterator operator++(int);  // post

   private:
    const compile_unit* cu_ = nullptr;
    span<const std::byte> data_{nullptr, nullptr};
    file_addr base_address_;
    const std::byte* pos_ = nullptr;
    entry current_;
};

class attr {
   public:
    attr(const compile_unit* cu, std::uint64_t type, std::uint64_t form,
         const std::byte* location)
        : cu_(cu), type_(type), form_(form), location_(location) {}

    std::uint64_t name() const { return type_; }
    std::uint64_t form() const { return form_; }

    file_addr as_address() const;
    std::uint32_t as_section_offset() const;
    span<const std::byte> as_block() const;
    std::uint64_t as_int() const;
    std::string_view as_string() const;
    die as_reference() const;

    range_list as_range_list() const;

   private:
    const compile_unit* cu_;
    std::uint64_t type_;
    std::uint64_t form_;
    const std::byte* location_;
};

/**
 * List of attribute specifications for one abbreviation — the part of an
 * abbreviation-table entry that describes, in order, which attributes a DIE has
 * and how each is encoded
 */
struct attr_spec {
    // a DW_AT_* code  — WHICH attribute (name, low_pc, type, …)
    std::uint64_t attr;
    // a DW_FORM_* code — HOW its value is encoded
    // How to read its value from `.debug_info`
    std::uint64_t form;
};

/**
 * Stores values held in a abbreviation table entry:
 *   - an abbreviation code
 *   - a tag
 *   - a flag indicating whether DIE has children
 *   - list of attribute specifications
 */
struct abbrev {
    std::uint64_t code;
    std::uint64_t tag;
    bool has_children;
    std::vector<attr_spec> attr_specs;
};

class compile_unit {
   public:
    compile_unit(dwarf& parent, span<const std::byte> data,
                 std::size_t abbrev_offset)
        : parent_(&parent), data_(data), abbrev_offset_(abbrev_offset) {}

    const dwarf* dwarf_info() const { return parent_; }
    span<const std::byte> data() const { return data_; }

    const std::unordered_map<std::uint64_t, gsdb::abbrev>& abbrev_table() const;

    die root() const;

   private:
    dwarf* parent_;
    span<const std::byte> data_;
    std::size_t abbrev_offset_;
};

class dwarf {
   public:
    dwarf(const elf& parent);
    const elf* elf_file() const { return elf_; }

    const std::unordered_map<std::uint64_t, abbrev>& get_abbrev_table(
        std::size_t offset);

    const std::vector<std::unique_ptr<compile_unit>>& compile_units() const {
        return compile_units_;
    }

   private:
    const elf* elf_;

    // byte offset -> abbreviation table
    // integer -> abbreviation entry
    std::unordered_map<std::size_t, std::unordered_map<std::uint64_t, abbrev>>
        abbrev_tables_;  // store the parsed tables

    std::vector<std::unique_ptr<compile_unit>> compile_units_;
};

class die {
   public:
    explicit die(const std::byte* next) : next_(next) {}
    die(const std::byte* pos, const compile_unit* cu, const abbrev* abbrev,
        const std::byte* next, std::vector<const std::byte*> attr_locs)
        : pos_(pos),
          cu_(cu),
          abbrev_(abbrev),
          next_(next),
          attr_locs_(std::move(attr_locs)) {}

    const compile_unit* cu() const { return cu_; }
    const abbrev* abbrev_entry() const { return abbrev_; }
    const std::byte* position() const { return pos_; }
    const std::byte* next() const { return next_; }

    /**
     * Wrap a DIE and provide begin and end member functions that we can call to
     * retrieve iterators to the children
     */
    class children_range;
    children_range children() const;

    bool contains(std::uint64_t attributes) const;
    attr operator[](std::uint64_t attribute) const;

    file_addr low_pc() const;
    file_addr high_pc() const;

    /**
     * Check whether the given file address lies within the ranges for the given
     * DIE
     */
    bool contains_address(file_addr address) const;

   private:
    const std::byte* pos_ = nullptr;
    const compile_unit* cu_ = nullptr;
    const abbrev* abbrev_ = nullptr;
    const std::byte* next_ = nullptr;
    std::vector<const std::byte*> attr_locs_;
};

class die::children_range {
   public:
    children_range(die die) : die_(std::move(die)) {}
    /* Default construction: leaving the iterator empty (generally expected of
     * iterators in C++);
     * Construction with an gsdb::die;
     * Copy construction and assignment, so we can easily copy iterators;
     * operator* and operator->, to
     * access the wrapped DIE;
     * operator++, for advancing to the next DIE; and
     * operator== and operator!=, for determining when we’ve finished iterating.
     */
    class iterator {
       public:
        using value_type = die;
        using reference = const die&;  // type returned by operator*
        using pointer = const die*;    // type returned by operator->
        // type returned by subtracting two iterators
        // std::ptrdiff_t - difference between pointers
        using difference_type = std::ptrdiff_t;
        /*
        The category can be one of the following:
            - std::output_iterator_tag Iterators that can be written to (such as
        std::ostream_iterator)
            - std::input_iterator_tag One-pass iterators
            - std::forward_iterator_tag Multipass iterators
            - std::bidirectional_iterator_tag Iterators that can be both
        incremented and decremented
            - std::random_access_iterator_tag Iterators that let you move to an
        arbitrary element in constant time
        */
        using iterator_category = std::forward_iterator_tag;

        iterator() = default;
        iterator(const iterator&) = default;
        iterator& operator=(const iterator&) = default;

        explicit iterator(reference die);

        reference operator*() const { return *die_; }
        pointer operator->() const { return &die_.value(); }

        iterator& operator++();
        // Note that the int parameter isn’t a real parameter; it’s just a way
        // to distinguish the pre-increment (++value) and post-increment
        // (value++) operators
        iterator operator++(int);  // post-increment

        bool operator==(const iterator& rhs) const;
        bool operator!=(const iterator& rhs) const { return !(*this == rhs); }

       private:
        std::optional<die> die_;
    };
    iterator begin() const {
        if (die_.abbrev_->has_children) {
            return iterator{die_};
        }
        return end();
    }
    iterator end() const { return iterator{}; }

   private:
    die die_;
};

}  // namespace gsdb

#endif
