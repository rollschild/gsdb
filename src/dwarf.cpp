#include "libgsdb/detail/dwarf.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <libgsdb/bit.hpp>
#include <libgsdb/dwarf.hpp>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "libgsdb/elf.hpp"
#include "libgsdb/error.hpp"
#include "libgsdb/types.hpp"

namespace {
/**
 * The cursor will handle the parsing of the data and advance the location being
 * pointed to
 */
class cursor {
   public:
    explicit cursor(gsdb::span<const std::byte> data)
        : data_(data), pos_(data.begin()) {}

    cursor& operator++() {
        ++pos_;
        return *this;
    }
    cursor& operator+=(std::size_t size) {
        pos_ += size;
        return *this;
    }

    const std::byte* position() const { return pos_; }

    bool finished() const { return pos_ >= data_.end(); }

    /**
     * Parsing of fixed-width integers
     */
    template <class T>
    T fixed_int() {
        auto t = gsdb::from_bytes<T>(pos_);
        pos_ += sizeof(T);
        return t;
    }

    std::uint8_t u8() { return fixed_int<std::uint8_t>(); }
    std::uint16_t u16() { return fixed_int<std::uint16_t>(); }
    std::uint32_t u32() { return fixed_int<std::uint32_t>(); }
    std::uint64_t u64() { return fixed_int<std::uint64_t>(); }
    std::int8_t s8() { return fixed_int<std::int8_t>(); }
    std::int16_t s16() { return fixed_int<std::int16_t>(); }
    std::int32_t s32() { return fixed_int<std::int32_t>(); }
    std::int64_t s64() { return fixed_int<std::int64_t>(); }

    std::string_view string() {
        auto null_terminator = std::find(pos_, data_.end(), std::byte{0});
        std::string_view ret(reinterpret_cast<const char*>(pos_),
                             null_terminator - pos_);
        pos_ = null_terminator + 1;  // why?
        return ret;
    }

    /**
     * ULEB128 to uint64_t
     */
    std::uint64_t uleb128() {
        std::uint64_t res = 0;
        int shift = 0;
        std::uint8_t byte = 0;
        do {
            byte = u8();
            auto masked = static_cast<uint64_t>(byte & 0x7f);
            res |= masked << shift;
            shift += 7;
        } while ((byte & 0x80) != 0);
        return res;
    }

    std::int64_t sleb128() {
        /**
         * In C++17, shifting a negative signed integer left is undefined
  behavior.
         * But C++20 onward: signed integers are now mandated to use two's
  complement representation, and `<<` was redefined purely in terms of the bit
  pattern. The result of `E1 << E2` is now:

              ▎ the unique value congruent to E1 × 2^E2 modulo 2^N,
              ▎ where N is the width of the type.

         * This holds for negative E1 too, so `-1 << 1` is now
  perfectly well-defined (it's -2). The negative-operand UB
  clause was simply deleted.
         */
        std::uint64_t res = 0;
        std::size_t shift = 0;
        std::uint8_t byte = 0;
        do {
            byte = u8();  // advances pos_
            auto masked = static_cast<uint64_t>(byte & 0x7f);
            res |= masked << shift;
            shift += 7;
        } while ((byte & 0x80) != 0);

        // check the second highest bit of the last byte read
        // to determine whether it's negative
        if ((shift < sizeof(res) * 8) and (byte & 0x40)) {
            // if negative and we have not filled the result integer,
            // fill the remaining high bits of the result by bit-flipping 0 to
            // obtain an integer with all bits set and then left-shifting the
            // unnecessary ones off the end
            res |= (~static_cast<std::uint64_t>(0) << shift);
        }

        return res;
    }

    void skip_form(std::uint64_t form) {
        switch (form) {
            case DW_FORM_flag_present:
                break;
            case DW_FORM_data1:
            case DW_FORM_ref1:
            case DW_FORM_flag:
                pos_ += 1;
                break;
            case DW_FORM_data2:
            case DW_FORM_ref2:
                pos_ += 2;
                break;
            case DW_FORM_data4:
            case DW_FORM_ref4:
            case DW_FORM_ref_addr:
            case DW_FORM_sec_offset:
            case DW_FORM_strp:
                pos_ += 4;
                break;
            case DW_FORM_data8:
            case DW_FORM_addr:
                pos_ += 8;
                break;
            case DW_FORM_sdata:
                // SLEB128
                sleb128();
                break;
            case DW_FORM_udata:
            case DW_FORM_ref_udata:
                // ULEB128
                uleb128();
                break;
            // blocks of data whose size is encoded as ULEB128 or fixed-sized
            // integer
            case DW_FORM_block1:
                pos_ += u8();
                break;
            case DW_FORM_block2:
                pos_ += u16();
                break;
            case DW_FORM_block4:
                pos_ += u32();
                break;
            case DW_FORM_block:
            // DWARF expressions for computing the locations of variables,
            // prefixed with the size of the expression as ULEB128
            case DW_FORM_exprloc:
                pos_ += uleb128();
                break;
            case DW_FORM_string:
                while (!finished() && *pos_ != std::byte(0)) {
                    ++pos_;
                }
                ++pos_;  // advance past the null terminator
                break;
            case DW_FORM_indirect:
                skip_form(uleb128());
                break;
            default:
                gsdb::error::send("Unrecognized DWARF form!");
        }
    }

   private:
    gsdb::span<const std::byte> data_;
    const std::byte* pos_;
};

std::unordered_map<std::uint64_t, gsdb::abbrev> parse_abbrev_table(
    const gsdb::elf& obj, std::size_t offset) {
    cursor cur(obj.get_section_contents(".debug_abbrev"));
    cur += offset;

    std::unordered_map<std::uint64_t, gsdb::abbrev> table;
    std::uint64_t code = 0;  // the parsed abbreviation code
    do {
        code = cur.uleb128();
        auto tag = cur.uleb128();
        auto has_children = static_cast<bool>(cur.u8());

        std::vector<gsdb::attr_spec> attr_specs;
        std::uint64_t attr = 0;

        do {
            attr = cur.uleb128();
            auto form = cur.uleb128();
            if (attr != 0) {
                attr_specs.push_back(gsdb::attr_spec{attr, form});
            }
        } while (attr != 0);

        if (code != 0) {
            table.emplace(code, gsdb::abbrev{code, tag, has_children,
                                             std::move(attr_specs)});
        }
    } while (code != 0);

    return table;
}

std::unique_ptr<gsdb::compile_unit> parse_compile_unit(
    gsdb::dwarf& dwarf, [[maybe_unused]] const gsdb::elf& obj, cursor cur) {
    auto start = cur.position();  // saving current cursor position
    auto size = cur.u32();
    auto version = cur.u16();
    auto abbrev = cur.u32();
    auto address_size = cur.u8();

    if (size == 0xffffffff) {  // DWARF64
        gsdb::error::send("Only DWARF32 is supported!");
    }
    if (version != 4) {
        gsdb::error::send("Only DWARF version 4 is supported!");
    }
    if (address_size != 8) {
        gsdb::error::send("Invalid address size for DWARF!");
    }

    // reported size in the compile unit header does _NOT_ include the size
    // field itself
    size += sizeof(std::uint32_t);
    gsdb::span<const std::byte> data = {start, size};
    return std::make_unique<gsdb::compile_unit>(dwarf, data, abbrev);
}

std::vector<std::unique_ptr<gsdb::compile_unit>> parse_compile_units(
    gsdb::dwarf& dwarf, const gsdb::elf& obj) {
    auto debug_info = obj.get_section_contents(".debug_info");
    cursor cur(debug_info);

    std::vector<std::unique_ptr<gsdb::compile_unit>> units;
    while (!cur.finished()) {
        auto unit = parse_compile_unit(dwarf, obj, cur);
        cur += unit->data().size();
        units.push_back(std::move(unit));
    }

    return units;
}

gsdb::die parse_die(const gsdb::compile_unit& cu, cursor cur) {
    auto pos = cur.position();
    auto abbrev_code = cur.uleb128();
    if (abbrev_code == 0) {
        auto next = cur.position();
        return gsdb::die{next};
    }

    // grab its abbreviation entry
    auto& abbrev_table = cu.abbrev_table();
    // So one
    // `abbrev` describes a reusable template ("a function DIE
    // looks like: name, low_pc, high_pc, …"), and many DIEs in
    // `.debug_info` share it by referencing its code.
    auto& abbrev = abbrev_table.at(abbrev_code);

    // We need to find the location of the next DIE
    // Also need to precompute the locations for each attribute of this DIE
    // `attr_locs` lets you lazily decode any individual attribute later without
    // re-parsing
    std::vector<const std::byte*> attr_locs;
    attr_locs.reserve(abbrev.attr_specs.size());
    for (auto& attr : abbrev.attr_specs) {
        attr_locs.push_back(cur.position());
        // advance the cursor by the required number of bytes
        cur.skip_form(attr.form);
    }

    auto next = cur.position();
    return gsdb::die(pos, &cu, &abbrev, next, std::move(attr_locs));
}

}  // namespace

gsdb::dwarf::dwarf(const gsdb::elf& parent) : elf_(&parent) {
    compile_units_ = parse_compile_units(*this, parent);
}

const std::unordered_map<std::uint64_t, gsdb::abbrev>&
gsdb::dwarf::get_abbrev_table(std::size_t offset) {
    if (!abbrev_tables_.count(offset)) {
        abbrev_tables_.emplace(offset, parse_abbrev_table(*elf_, offset));
    }
    return abbrev_tables_.at(offset);
}

const std::unordered_map<std::uint64_t, gsdb::abbrev>&
gsdb::compile_unit::abbrev_table() const {
    return parent_->get_abbrev_table(abbrev_offset_);
}

gsdb::die gsdb::compile_unit::root() const {
    std::size_t header_size = 11;
    cursor cur({data_.begin() + header_size, data_.end()});
    return parse_die(*this, cur);
}

gsdb::die::children_range::iterator::iterator(const gsdb::die& d) {
    // Parse the first child DIE of the one given
    cursor next_cur({d.next_, d.cu_->data().end()});
    // retrieve a die representing the contents
    die_ = parse_die(*d.cu_, next_cur);
}

bool gsdb::die::children_range::iterator::operator==(
    const iterator& rhs) const {
    // null if it does not have a DIE stored, or
    // the stored DIE has an abbreviation code of `0`
    auto lhs_null = !die_.has_value() or !die_->abbrev_entry();
    auto rhs_null = !rhs.die_.has_value() or !rhs.die_->abbrev_entry();

    if (lhs_null and rhs_null) return true;
    if (lhs_null or rhs_null) return false;

    // whether their abbreviation codes and the next DIE pointers match
    return die_->abbrev_ == rhs->abbrev_ and die_->next() == rhs->next();
}

gsdb::die::children_range::iterator&
gsdb::die::children_range::iterator::operator++() {
    if (!die_.has_value() or !die_->abbrev_) {
        return *this;
    }

    if (!die_->abbrev_->has_children) {
        // build cursor from `next_` to the end of the compile unit and parse
        // the DIE there
        cursor next_cur({die_->next_, die_->cu_->data().end()});
        die_ = parse_die(*die_->cu_, next_cur);
    } else if (die_->contains(DW_AT_sibling)) {
        // checks whether the DIE contains a sibling attribute
        // if so, set `die_` to the result of interpreting that attribute value
        // as a DIE reference
        die_ = die_.value()[DW_AT_sibling].as_reference();

    } else {
        iterator sub_children(*die_);
        // iterate over the children
        // advance it until we hit a null entry
        while (sub_children->abbrev_) ++sub_children;
        // the next DIE after the null entry is the sibling
        cursor next_cur({sub_children->next_, die_->cu_->data().end()});
        die_ = parse_die(*die_->cu_, next_cur);
    }

    return *this;
}

gsdb::die::children_range::iterator
gsdb::die::children_range::iterator::operator++(int) {
    // A common C++ pattern: implementing a pre-increment function and then
    // writing the post-increment equivalent by copying the current iterator,
    // calling pre-increment on *this, and returning the unmodified copy.
    auto tmp = *this;
    ++(*this);
    return tmp;
}

// Note that DWARF provides a quick way for compilers to get the sibling of a
// DIE: they can encode a reference to the sibling using a DW_AT_sibling
// attribute.
gsdb::die::children_range gsdb::die::children() const {
    return children_range(*this);
}

gsdb::attr gsdb::die::operator[](std::uint64_t attribute) const {
    auto& specs = abbrev_->attr_specs;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        if (specs[i].attr == attribute) {
            return {cu_, specs[i].attr, specs[i].form, attr_locs_[i]};
        }
    }

    error::send("Attribute not found!");
}

/**
 * Just read a single 64-bit integer from the start of the attribute bytes and
 * return it
 */
gsdb::file_addr gsdb::attr::as_address() const {
    cursor cur({location_, cu_->data().end()});
    if (form_ != DW_FORM_addr) {
        error::send("Invalid address type!");
    }
    auto elf = cu_->dwarf_info()->elf_file();
    // This address is a file address in the ELF file to which this DWARF
    // information belongs
    return file_addr{*elf, cur.u64()};
}

std::uint32_t gsdb::attr::as_section_offset() const {
    cursor cur({location_, cu_->data().end()});
    if (form_ != DW_FORM_sec_offset) {
        error::send("Invalid offset type!");
    }
    // section offsets are 32 bits in 32-bit DWARF
    return cur.u32();
}

std::uint64_t gsdb::attr::as_int() const {
    cursor cur({location_, cu_->data().end()});

    switch (form_) {
        case DW_FORM_data1:
            return cur.u8();
        case DW_FORM_data2:
            return cur.u16();
        case DW_FORM_data4:
            return cur.u32();
        case DW_FORM_data8:
            return cur.u64();
        case DW_FORM_udata:
            return cur.uleb128();
        default:
            error::send("Invalid integer type!");
    }
}

gsdb::span<const std::byte> gsdb::attr::as_block() const {
    // Get a cursor,
    // Parse the values we want,
    // Throw an exception on surprises
    // Return the requestd data
    std::size_t size;
    cursor cur({location_, cu_->data().end()});
    switch (form_) {
        case DW_FORM_block1:
            size = cur.u8();
            break;
        case DW_FORM_block2:
            size = cur.u16();
            break;
        case DW_FORM_block4:
            size = cur.u32();
            break;
        case DW_FORM_block:
            size = cur.uleb128();
            break;
        default:
            error::send("Invalid block type!");
    }

    return {cur.position(), size};
}

gsdb::die gsdb::attr::as_reference() const {
    cursor cur({location_, cu_->data().end()});
    std::size_t offset;

    switch (form_) {
        case DW_FORM_ref1:
            offset = cur.u8();
            break;
        case DW_FORM_ref2:
            offset = cur.u16();
            break;
        case DW_FORM_ref4:
            offset = cur.u32();
            break;
        case DW_FORM_ref8:
            offset = cur.u64();
            break;
        case DW_FORM_udata:
            offset = cur.uleb128();
            break;
        case DW_FORM_ref_addr: {
            // can reference data in other compile units
            // its offset is relative to the start of the `.debug_info` section
            offset = cur.u32();
            // we read the offset from the start of the .debug_info section and
            // ask the parent ELF file for the contents of that section
            auto section = cu_->dwarf_info()->elf_file()->get_section_contents(
                ".debug_info");
            // position of the referenced DIE
            auto die_pos = section.begin() + offset;
            // we look through all of the compile units contained in the debug
            // information for a compile unit whose data range contains the DIE
            // we’re looking for.
            auto& cus = cu_->dwarf_info()->compile_units();
            auto cu_finder = [=](auto& cu) {
                return cu->data().begin() <= die_pos and
                       cu->data().end() > die_pos;
            };
            auto cu_for_offset =
                std::find_if(std::begin(cus), std::end(cus), cu_finder);
            // create a cursor for the referenced DIE position and parse the DIE
            // there
            cursor ref_cur({die_pos, cu_for_offset->get()->data().end()});
            return parse_die(**cu_for_offset, ref_cur);
        }
        default:
            error::send("Invalid reference type!");
    }

    // All of these forms’ offsets are based on the start of the current DIE’s
    // compile unit
    cursor ref_cur({cu_->data().begin() + offset, cu_->data().end()});
    return parse_die(*cu_, ref_cur);
}

std::string_view gsdb::attr::as_string() const {
    cursor cur({location_, cu_->data().end()});
    switch (form_) {
        case DW_FORM_string:
            return cur.string();
        case DW_FORM_strp: {
            auto offset = cur.u32();
            auto stab = cu_->dwarf_info()->elf_file()->get_section_contents(
                ".debug_str");
            cursor stab_cur({stab.begin() + offset, stab.end()});
            return stab_cur.string();
        }
        default:
            error::send("Invalid string type!");
    }
}

gsdb::file_addr gsdb::die::low_pc() const {
    if (contains(DW_AT_ranges)) {
        // Return the low address of the first range,
        // because address ranges are always in ascending order of address
        auto first_entry = (*this)[DW_AT_ranges].as_range_list().begin();
        return first_entry->low;
    } else if (contains(DW_AT_low_pc)) {
        return (*this)[DW_AT_low_pc].as_address();
    }
    error::send("DIE does NOT have low PC!");
}

gsdb::file_addr gsdb::die::high_pc() const {
    if (contains(DW_AT_ranges)) {
        auto ranges = (*this)[DW_AT_ranges].as_range_list();
        auto it = ranges.begin();
        // increment it until it points to the element before the end iterator
        // (that is, the last element of the list)
        while (std::next(it) != ranges.end()) ++it;
        return it->high;
    } else if (contains(DW_AT_high_pc)) {
        auto attr = (*this)[DW_AT_high_pc];
        // interpreting the attribute as an address
        if (attr.form() == DW_FORM_addr) {
            // If the form is an address, we extract it
            return attr.as_address();
        } else {
            // interpreting the attribute as an offset form the low program
            // counter the form must be an offset from the low program counter,
            // so we extract the low program counter and then offset it with the
            // high program counter attribute as an integer
            return low_pc() + attr.as_int();
        }
    }
    error::send("DIE does not have high PC!");
}

gsdb::range_list::iterator::iterator(const compile_unit* cu,
                                     gsdb::span<const std::byte> data,
                                     file_addr base_address)
    : cu_(cu), data_(data), base_address_(base_address), pos_(data.begin()) {
    ++(*this);
}

gsdb::range_list::iterator& gsdb::range_list::iterator::operator++() {
    auto elf = cu_->dwarf_info()->elf_file();
    // 64-bit integer with all bits set to 1
    constexpr auto base_address_flag = ~static_cast<std::uint64_t>(0);

    cursor cur({pos_, data_.end()});
    while (true) {
        current_.low = file_addr{*elf, cur.u64()};
        current_.high = file_addr{*elf, cur.u64()};

        if (current_.low.addr() == base_address_flag) {
            base_address_ = current_.high;
        } else if (current_.low.addr() == 0 and current_.high.addr() == 0) {
            pos_ = nullptr;
            break;
        } else {
            pos_ = cur.position();
            current_.low += base_address_.addr();
            current_.high += base_address_.addr();
            break;
        }
    }

    return *this;
}

gsdb::range_list::iterator gsdb::range_list::iterator::operator++(int) {
    auto tmp = *this;
    ++(*this);
    return tmp;
}

gsdb::range_list gsdb::attr::as_range_list() const {
    // DWARF encodes range list attributes as offsets into the `.debug_ranges`
    // section
    auto section =
        cu_->dwarf_info()->elf_file()->get_section_contents(".debug_ranges");
    auto offset = as_section_offset();
    span<const std::byte> data(section.begin() + offset, section.end());

    auto root = cu_->root();
    file_addr base_address = root.contains(DW_AT_low_pc)
                                 ? root[DW_AT_low_pc].as_address()
                                 : file_addr{};

    return {cu_, data, base_address};
}

gsdb::range_list::iterator gsdb::range_list::begin() const {
    return {cu_, data_, base_address_};
}

gsdb::range_list::iterator gsdb::range_list::end() const { return {}; }

bool gsdb::range_list::contains(gsdb::file_addr addr) const {
    // whether any of the entries in the list contain the given address
    return std::any_of(begin(), end(),
                       [=](auto& e) { return e.contains(addr); });
}

bool gsdb::die::contains_address(gsdb::file_addr address) const {
    // ensure the ELF file for the given address is the same as the ELF file to
    // which this DIE belongs
    if (address.elf_file() != this->cu_->dwarf_info()->elf_file()) {
        return false;
    }

    if (contains(DW_AT_ranges)) {
        return (*this)[DW_AT_ranges].as_range_list().contains(address);
    } else if (contains(DW_AT_low_pc)) {
        // whether the given address lies between that attribute and the high PC
        // attribute
        return low_pc() <= address and high_pc() > address;
    }

    return false;
}
