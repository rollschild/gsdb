#ifndef GSDB_DWARF_HPP
#define GSDB_DWARF_HPP

#include <libgsdb/detail/dwarf.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "libgsdb/process.hpp"
#include "libgsdb/registers.hpp"
#include "libgsdb/types.hpp"

namespace gsdb {
class compile_unit;
class elf;
class dwarf;
class die;

class call_frame_information {
   public:
    struct common_information_entry {
        std::uint32_t length;
        std::uint64_t code_alignment_factor;
        std::int64_t data_alignment_factor;
        bool fde_has_augmentaion;
        std::uint8_t fde_pointer_encoding;
        span<const std::byte> instructions;
    };
    struct frame_description_entry {
        std::uint32_t length;
        const common_information_entry* cie;
        file_addr initial_location;
        std::uint64_t address_range;
        span<const std::byte> instructions;
    };
    struct eh_hdr {
        // pointer to the start of the `.eh_frame_hdr` section
        const std::byte* start;
        // pointer to start of the search table
        const std::byte* search_table;
        const std::size_t count;
        std::uint8_t encoding;
        call_frame_information* parent;

        /**
         * Takes an instruction's object file offset and returns a pointer to
         * the start of the FDE for that instruction
         */
        const std::byte* operator[](file_addr address) const;
    };

    call_frame_information() = delete;
    call_frame_information(const call_frame_information&) = delete;
    call_frame_information& operator=(const call_frame_information&) = delete;

    const dwarf& dwarf_info() const { return *dwarf_; }

    const common_information_entry& get_cie(file_offset at) const;

    call_frame_information(const dwarf* dwarf, eh_hdr hdr)
        : dwarf_(dwarf), eh_hdr_(hdr) {
        eh_hdr_.parent = this;
    }

    /**
     * Find the FDE for the given program counter value, and execute its call
     * frame information instructions until it produces the table row
     * corresponding to the program counter.
     *
     * Returns a new set of registers that represents the machine state for the
     * caller.
     */
    registers unwind(const process& proc, file_addr pc, registers& regs) const;

   private:
    const dwarf* dwarf_;
    // FDEs reference CIEs by their offset in the object file.
    // When parsing an FDE, we’ll need to be able to quickly retrieve a CIE from
    // this offset.
    // `mutable` because it's used as a cache
    mutable std::unordered_map<std::uint32_t, common_information_entry>
        cie_map_;
    eh_hdr eh_hdr_;
};

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

class dwarf_expression {
   public:
    // address
    struct address_result {
        virt_addr address;
    };
    // register
    struct register_result {
        std::uint64_t reg_num;
    };
    // DW_OP_implicit_result opcode
    struct data_result {
        span<const std::byte> data;
    };
    // DW_OP_stack_value opcode
    struct literal_result {
        std::uint64_t value;
    };

    // empty location description
    struct empty_result {};
    using simple_location =
        std::variant<address_result, register_result, data_result,
                     literal_result, empty_result>;

    struct pieces_result {
        struct piece {
            simple_location location;
            std::uint64_t bit_size;
            // bit offset from the `location` member at which the data is really
            // stored
            std::uint64_t offset = 0;
        };
        std::vector<piece> pieces;
    };
    using result = std::variant<simple_location, pieces_result>;

    dwarf_expression(const dwarf& parent, span<const std::byte> expr_data,
                     bool in_frame_info)
        : parent_(&parent),
          expr_data_(expr_data),
          in_frame_info_(in_frame_info) {}

    // push_cfa: whether the CFA should be pushed to the stack before the
    // expression is evaluated; used for running the `expression` and
    // `val_expression` register restore rules when unwinding the stack
    result eval(const process& proc, const registers& regs,
                bool push_cfa = false) const;

   private:
    const dwarf* parent_;
    span<const std::byte> expr_data_;

    // whether the expression resides in the `.eh_frame` section or a
    // `DW_AT_frame_base` attribute
    bool in_frame_info_;
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

class line_table {
   public:
    // Represent file entries
    struct file {
        std::filesystem::path path;
        std::uint64_t mod_time;
        std::uint64_t file_len;
    };
    struct entry;
    class iterator;

    iterator begin() const;
    iterator end() const;

    line_table(span<const std::byte> data, const compile_unit* cu,
               bool default_is_stmt, std::int8_t line_base,
               std::uint8_t line_range, std::uint8_t opcode_base,
               std::vector<std::filesystem::path> include_dirs,
               std::vector<file> file_names)
        : data_(data),
          cu_(cu),
          default_is_stmt_(default_is_stmt),
          line_base_(line_base),
          line_range_(line_range),
          opcode_base_(opcode_base),
          include_dirs_(std::move(include_dirs)),
          file_names_(std::move(file_names)) {}

    line_table(const line_table&) = delete;
    line_table& operator=(const line_table&) = delete;

    const compile_unit& cu() const { return *cu_; }
    const std::vector<file>& file_names() const { return file_names_; }

    /**
     * Takes a file address and returns an iterator to the line table entry that
     * corresponds to that address
     */
    iterator get_entry_by_address(file_addr address) const;
    /**
     * A single line of source code may correspond to multiple line table
     * entries
     */
    std::vector<iterator> get_entries_by_line(std::filesystem::path path,
                                              std::size_t line) const;

   private:
    span<const std::byte> data_;
    // for retrieving elements such as the compilation directory
    const compile_unit* cu_;
    bool default_is_stmt_;
    std::int8_t line_base_;
    std::uint8_t line_range_;
    std::uint8_t opcode_base_;
    std::vector<std::filesystem::path> include_dirs_;
    // allows it to be modified even when the `line_table` is marked `const`
    mutable std::vector<file> file_names_;
};

/**
 * Registers the abstract machine stores
 */
struct line_table::entry {
    file_addr address;
    std::uint64_t file_index = 1;
    std::uint64_t line = 1;
    std::uint64_t column = 0;
    bool is_stmt;
    bool basic_block_start = false;
    bool end_sequence = false;
    bool prologue_end = false;
    bool epilogue_begin = false;
    std::uint64_t discriminator = 0;
    //  A pointer to the actual file data for this entry, as this information is
    //  much more useful to users than the file_index member
    file* file_entry = nullptr;

    bool operator==(const entry& rhs) const {
        return address == rhs.address and file_index == rhs.file_index and
               line == rhs.line and column == rhs.column and
               discriminator == rhs.discriminator;
    }
};

class line_table::iterator {
   public:
    using value_type = entry;
    using pointer = const entry*;
    using reference = const entry&;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;  // multipass

    iterator(const line_table* table_);
    iterator() = default;
    iterator(const iterator&) = default;
    iterator& operator=(const iterator&) = default;

    const line_table::entry& operator*() const { return current_; }
    const line_table::entry* operator->() const { return &current_; }

    bool operator==(const iterator& rhs) const { return pos_ == rhs.pos_; }
    bool operator!=(const iterator& rhs) const { return pos_ != rhs.pos_; }

    iterator& operator++();
    iterator operator++(int);

   private:
    const line_table* table_;
    line_table::entry current_;
    line_table::entry registers_;
    const std::byte* pos_;

    bool execute_instruction();
};

class compile_unit {
   public:
    compile_unit(dwarf& parent, span<const std::byte> data,
                 std::size_t abbrev_offset);

    const dwarf* dwarf_info() const { return parent_; }
    span<const std::byte> data() const { return data_; }

    const std::unordered_map<std::uint64_t, gsdb::abbrev>& abbrev_table() const;

    die root() const;

    const line_table& lines() const { return *line_table_; }

   private:
    dwarf* parent_;
    span<const std::byte> data_;
    std::size_t abbrev_offset_;
    std::unique_ptr<line_table> line_table_;
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

    // retrieve the compile unit to which a given file address belongs
    const compile_unit* compile_unit_containing_address(
        file_addr address) const;
    // retrieve the function to which a given file address belongs
    std::optional<die> function_containing_address(file_addr address) const;

    // retrieve the function DIEs for functions that match the given name
    std::vector<die> find_functions(std::string name) const;

    /**
     * Finds the compile unit corresponding to the given file address, retrieves
     * the line table for that compile unit, and returns the relevant entry
     */
    line_table::iterator line_entry_at_address(file_addr address) const {
        auto cu = compile_unit_containing_address(address);
        if (!cu) return {};
        return cu->lines().get_entry_by_address(address);
    }

    /**
     * Calculates the inline stack at a given file address.
     *  The first element is the outer, non-inlined function. Subsequent
     * elements represent functions inlined into the preceding function that
     * contain the given address.
     */
    std::vector<die> inline_stack_at_address(file_addr address) const;

    const call_frame_information& cfi() const { return *cfi_; }

   private:
    const elf* elf_;

    // byte offset -> abbreviation table
    // integer -> abbreviation entry
    std::unordered_map<std::size_t, std::unordered_map<std::uint64_t, abbrev>>
        abbrev_tables_;  // store the parsed tables

    std::vector<std::unique_ptr<compile_unit>> compile_units_;

    // Index the entire set of DIEs in the `dwarf` object
    void index() const;
    // index a single DIE
    void index_die(const die& current) const;

    struct index_entry {
        const compile_unit* cu;
        const std::byte* pos;  // pointer to beginning of the DIE
    };

    // Maps function names to index entries.
    // Multiple functions can share the same thus, thus the multimap.
    // `mutable`: allows for implementing caches that don't affect the logical
    // state of the type, but speed up certain operations.
    // `mutable` lets a data member be modified even through a
    // const object or inside a const member function. Normally,
    // inside a const method every member is treated as
    // read-only; mutable carves out an exception for this one
    // field.
    mutable std::unordered_multimap<std::string, index_entry> function_index_;

    std::unique_ptr<call_frame_information> cfi_;
};

struct source_location {
    const line_table::file* file;
    std::uint64_t line;
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

    std::optional<std::string_view> name() const;

    source_location location() const;
    const line_table::file& file() const;
    std::uint64_t line() const;

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
