#include "libgsdb/detail/dwarf.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <libgsdb/bit.hpp>
#include <libgsdb/dwarf.hpp>
#include <memory>
#include <optional>
#include <string>
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

std::uint64_t parse_eh_frame_pointer_with_base(cursor& cur,
                                               std::uint8_t encoding,
                                               std::uint64_t base) {
    // the least significant 4 bits of the encoding byte tell us how to
    // interpret the pointer
    switch (encoding & 0x0f) {
        case DW_EH_PE_absptr:
            return base + cur.u64();
        case DW_EH_PE_uleb128:
            return base + cur.uleb128();
        case DW_EH_PE_udata2:
            return base + cur.u16();
        case DW_EH_PE_udata4:
            return base + cur.u32();
        case DW_EH_PE_udata8:
            return base + cur.u64();
        case DW_EH_PE_sleb128:
            return base + cur.sleb128();
        case DW_EH_PE_sdata2:
            return base + cur.s16();
        case DW_EH_PE_sdata4:
            return base + cur.s32();
        case DW_EH_PE_sdata8:
            return base + cur.s64();
        default:
            gsdb::error::send("Unknown eh_frame pointer encoding!");
    }
}

std::uint64_t parse_eh_frame_pointer([[maybe_unused]] const gsdb::elf& elf,
                                     cursor& cur, std::uint8_t encoding,
                                     std::uint64_t pc,
                                     std::uint64_t text_section_start,
                                     std::uint64_t data_section_start,
                                     std::uint64_t func_start) {
    std::uint64_t base = 0;
    // Mask out the most significant bit (0x80) because it corresponds to the
    // indirect encoding scheme, which we don’t need to handle.
    switch (encoding & 0x70) {
        case DW_EH_PE_absptr:
            // If the pointer is absolute, the base address is 0.
            break;
        case DW_EH_PE_pcrel:
            base = pc;
            break;
        case DW_EH_PE_textrel:
            base = text_section_start;
            break;
        case DW_EH_PE_datarel:
            base = data_section_start;
            break;
        case DW_EH_PE_funcrel:
            base = func_start;
            break;
        default:
            gsdb::error::send("Unknown eh_frame pointer encoding!");
    }

    return parse_eh_frame_pointer_with_base(cur, encoding, base);
}

/**
 * Parse a single CIE at a given cursor position
 */
gsdb::call_frame_information::common_information_entry parse_cie(cursor cur) {
    auto start = cur.position();
    // len field does _NOT_ include its own size
    auto len = cur.u32() + 4;
    /* auto id = */ cur.u32();
    auto version = cur.u8();

    if (!(version == 1 or version == 3 or version == 4)) {
        gsdb::error::send("Invalid CIE version!");
    }
    auto augmentation = cur.string();
    if (!augmentation.empty() and augmentation[0] != 'z') {
        gsdb::error::send("Invalid CIE augmentation!");
    }

    if (version == 4) {
        auto address_size = cur.u8();
        auto segment_size = cur.u8();
        if (address_size != 8) {
            gsdb::error::send("Invalid address size!");
        }
        if (segment_size != 0) {
            gsdb::error::send("Invalid segment size!");
        }
    }

    auto code_alignment_factor = cur.uleb128();
    auto data_alignment_factor = cur.sleb128();
    [[maybe_unused]] auto return_address_register =
        version == 1 ? cur.u8() : cur.uleb128();

    std::uint8_t fde_pointer_encoding = DW_EH_PE_udata8 | DW_EH_PE_absptr;
    for (auto c : augmentation) {
        switch (c) {
            case 'z':
                cur.uleb128();
                break;
            case 'R':
                fde_pointer_encoding = cur.u8();
                break;
            case 'L':
                cur.u8();
                break;
            case 'P': {
                auto encoding = cur.u8();
                (void)parse_eh_frame_pointer_with_base(cur, encoding, 0);
                break;
            }
            default:
                gsdb::error::send("Invalid CIE augmentation!");
        }
    }

    gsdb::span<const std::byte> instructions = {cur.position(), start + len};
    bool fde_has_augmentation = !augmentation.empty();
    return {len,
            code_alignment_factor,
            data_alignment_factor,
            fde_has_augmentation,
            fde_pointer_encoding,
            instructions};
}

[[maybe_unused]]
gsdb::call_frame_information::frame_description_entry parse_fde(
    const gsdb::call_frame_information& cfi, cursor cur) {
    auto start = cur.position();
    auto len = cur.u32() + 4;

    // retrieve elf object to which this call frame information belongs
    auto elf = cfi.dwarf_info().elf_file();
    // convert the current cursor position into a file offset
    auto current_offset = elf->data_pointer_as_file_offset(cur.position());
    // parse the distance to the linked CIE and subtract it from the current
    // cursor offset to get the offset of the linked CIE from start of the
    // object file
    gsdb::file_offset cie_offset{*elf, current_offset.off() - cur.s32()};
    auto& cie = cfi.get_cie(cie_offset);

    current_offset = elf->data_pointer_as_file_offset(cur.position());
    auto text_section_start =
        elf->get_section_start_address(".text").value_or(gsdb::file_addr{});
    auto initial_location_addr = parse_eh_frame_pointer(
        *elf, cur, cie.fde_pointer_encoding, current_offset.off(),
        text_section_start.addr(), 0, 0);
    gsdb::file_addr initial_location{*elf, initial_location_addr};

    auto address_range =
        parse_eh_frame_pointer_with_base(cur, cie.fde_pointer_encoding, 0);
    if (cie.fde_has_augmentaion) {
        auto augmentation_len = cur.uleb128();
        cur += augmentation_len;
    }
    gsdb::span<const std::byte> instructions = {cur.position(), start + len};
    return {len, &cie, initial_location, address_range, instructions};
}

gsdb::call_frame_information::eh_hdr parse_eh_hdr(gsdb::dwarf& dwarf) {
    auto elf = dwarf.elf_file();
    [[maybe_unused]] auto eh_hdr_start =
        *elf->get_section_start_address(".eh_frame_hdr");
    [[maybe_unused]] auto text_section_start =
        *elf->get_section_start_address(".text");

    auto eh_hdr_data = elf->get_section_contents(".eh_frame_hdr");
    cursor cur(eh_hdr_data);

    auto start = cur.position();
    [[maybe_unused]] auto version = cur.u8();
    auto eh_frame_ptr_enc = cur.u8();
    auto fde_count_enc = cur.u8();
    auto table_enc = cur.u8();
    (void)parse_eh_frame_pointer_with_base(cur, eh_frame_ptr_enc, 0);

    auto fde_count = parse_eh_frame_pointer_with_base(cur, fde_count_enc, 0);

    auto search_table = cur.position();
    return {start, search_table, fde_count, table_enc, nullptr};
}

/**
 * Get the byte size for a given encoding scheme
 */
std::size_t eh_frame_pointer_encoding_size(std::uint8_t encoding) {
    switch (encoding & 0x7) {
        case DW_EH_PE_absptr:
            return 8;
        case DW_EH_PE_udata2:
            return 2;
        case DW_EH_PE_udata4:
            return 4;
        case DW_EH_PE_udata8:
            return 8;
        default:
            gsdb::error::send("Invalid pointer encoding!");
    }
}

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

/**
 * Parse the filepath, parent directory index, modification time, and file
 * length, then make any relative paths absolute
 */
gsdb::line_table::file parse_line_table_file(
    cursor& cur, std::filesystem::path compilation_dir,
    const std::vector<std::filesystem::path>& include_dirs) {
    auto file = cur.string();
    auto dir_index = cur.uleb128();
    auto modification_time = cur.uleb128();
    auto file_length = cur.uleb128();

    std::filesystem::path path = file;
    if (file[0] != '/') {
        if (dir_index == 0) {
            // Relative paths are relative to the compilation directory if
            // `dir_index` is `0`
            path = compilation_dir / std::string(file);
        } else {
            // otherwise, they are relative to the `include_dirs` entry at the
            // specified index, beginning with `1`
            path = include_dirs[dir_index - 1] / std::string(file);
        }
    }

    return {path.string(), modification_time, file_length};
}

std::unique_ptr<gsdb::line_table> parse_line_table(
    const gsdb::compile_unit& cu) {
    auto section =
        cu.dwarf_info()->elf_file()->get_section_contents(".debug_line");
    if (!cu.root().contains(DW_AT_stmt_list)) return nullptr;
    auto offset = cu.root()[DW_AT_stmt_list].as_section_offset();
    cursor cur({section.begin() + offset, section.end()});

    auto size = cur.u32();
    auto end = cur.position() + size;

    auto version = cur.u16();
    if (version != 4) {
        gsdb::error::send("Only DWARF 4 is supported!");
    }

    // `(void)` is to avoid compiler warnings?
    (void)cur.u32();  // header length

    auto min_instruction_len = cur.u8();
    if (min_instruction_len != 1) {
        gsdb::error::send("Invalid minimum instruction length!");
    }

    auto max_ops_per_instruction = cur.u8();
    if (max_ops_per_instruction != 1) {
        gsdb::error::send("Invalid max operations per instruction!");
    }

    auto default_is_stmt = cur.u8();
    auto line_base = cur.s8();
    auto line_range = cur.u8();
    auto opcode_base = cur.u8();

    // If all standard opcodes in this line table program,
    // `opcode_base` would be 13.
    // Read a number of values equal to `opcode_base - 1`
    std::array<std::uint8_t, 12> expected_opcode_lens{0, 1, 1, 1, 1, 0,
                                                      0, 0, 1, 0, 0, 1};
    // Ensure these lengths match the opcode lengths specified in the DWARF
    // standard
    for (auto i = 0; i < opcode_base - 1; ++i) {
        if (cur.u8() != expected_opcode_lens[i]) {
            gsdb::error::send("Unexpected opcode length!");
        }
    }

    std::vector<std::filesystem::path> include_dirs;
    std::filesystem::path compilation_dir(
        cu.root()[DW_AT_comp_dir].as_string());
    for (auto dir = cur.string(); !dir.empty(); dir = cur.string()) {
        // include directories can be either absolute paths or paths relative to
        // the compilation directory for this compile unit
        if (dir[0] == '/') {
            include_dirs.push_back(std::string(dir));
        } else {
            include_dirs.push_back(compilation_dir / std::string(dir));
        }
    }

    std::vector<gsdb::line_table::file> file_names;
    // keep reading file entries until we read a null byte
    while (*cur.position() != std::byte(0)) {
        file_names.push_back(
            parse_line_table_file(cur, compilation_dir, include_dirs));
    }
    // move past the null byte,
    // bring the cursor to the beginning of the line table program itself
    cur += 1;

    // Construct the return value
    gsdb::span<const std::byte> data{cur.position(), end};
    return std::make_unique<gsdb::line_table>(
        data, &cu, default_is_stmt, line_base, line_range, opcode_base,
        std::move(include_dirs), std::move(file_names));
}

bool path_ends_in(const std::filesystem::path& lhs,
                  const std::filesystem::path& rhs) {
    // number of elements in each path - number of directories, plus one for the
    // file name
    auto lhs_size = std::distance(lhs.begin(), lhs.end());
    auto rhs_size = std::distance(rhs.begin(), rhs.end());
    if (rhs_size > lhs_size) return false;
    // Checks the path elements/components, _LEXICALLY_!!!
    auto start = std::next(lhs.begin(), lhs_size - rhs_size);
    return std::equal(start, lhs.end(), rhs.begin());
}

/**
 * Parse the `.eh_frame_hdr` section
 */
std::unique_ptr<gsdb::call_frame_information> parse_call_frame_information(
    gsdb::dwarf& dwarf) {
    auto eh_hdr = parse_eh_hdr(dwarf);
    return std::make_unique<gsdb::call_frame_information>(&dwarf, eh_hdr);
}

}  // namespace

gsdb::dwarf::dwarf(const gsdb::elf& parent) : elf_(&parent) {
    compile_units_ = parse_compile_units(*this, parent);
    cfi_ = parse_call_frame_information(*this);
}

const std::unordered_map<std::uint64_t, gsdb::abbrev>&
gsdb::dwarf::get_abbrev_table(std::size_t offset) {
    if (!abbrev_tables_.count(offset)) {
        abbrev_tables_.emplace(offset, parse_abbrev_table(*elf_, offset));
    }
    return abbrev_tables_.at(offset);
}

gsdb::compile_unit::compile_unit(gsdb::dwarf& parent,
                                 span<const std::byte> data,
                                 std::size_t abbrev_offset)
    : parent_(&parent), data_(data), abbrev_offset_(abbrev_offset) {
    line_table_ = parse_line_table(*this);
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

bool gsdb::die::contains(std::uint64_t attribute) const {
    auto& specs = abbrev_->attr_specs;
    return std::find_if(std::begin(specs), std::end(specs), [=](auto spec) {
               return spec.attr == attribute;
           }) != std::end(specs);
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

const gsdb::compile_unit* gsdb::dwarf::compile_unit_containing_address(
    gsdb::file_addr addr) const {
    for (auto& cu : compile_units_) {
        if (cu->root().contains_address(addr)) {
            return cu.get();
        }
    }
    return nullptr;
}

std::optional<gsdb::die> gsdb::dwarf::function_containing_address(
    file_addr addr) const {
    // indexing the DWARF information to ensure we populate `function_index_`
    index();

    for (auto& [name, entry] : function_index_) {
        cursor cur({entry.pos, entry.cu->data().end()});
        auto d = parse_die(*entry.cu, cur);
        // whether the DIE contains the given address and is a regular function
        if (d.contains_address(addr) and
            d.abbrev_entry()->tag == DW_TAG_subprogram) {
            return d;
        }
    }

    return std::nullopt;
}

std::vector<gsdb::die> gsdb::dwarf::find_functions(std::string name) const {
    index();

    std::vector<die> found;
    auto [begin, end] = function_index_.equal_range(name);
    std::transform(begin, end, std::back_inserter(found), [](auto& pair) {
        // The iterators returned by `std::unordered_multimap` dereference to
        // key-value pairs,
        auto [name, entry] = pair;
        cursor cur({entry.pos, entry.cu->data().end()});
        return parse_die(*entry.cu, cur);
    });

    return found;
}

void gsdb::dwarf::index() const {
    // Make sure we index the DWARF info only once
    if (!function_index_.empty()) {
        return;
    }
    // Loop through all compile units and index their root DIEs
    for (auto& cu : compile_units_) {
        index_die(cu->root());
    }
}

std::optional<std::string_view> gsdb::die::name() const {
    if (contains(DW_AT_name)) {
        return (*this)[DW_AT_name].as_string();
    }
    if (contains(DW_AT_specification)) {
        // Resolve the attribute as a reference to another DIE.
        // We call .name rather than just grabbing the DW_AT_name attribute on
        // the result to account for chains of references (for example,
        // out-of-line definitions that were inlined).
        return (*this)[DW_AT_specification].as_reference().name();
    }
    if (contains(DW_AT_abstract_origin)) {
        return (*this)[DW_AT_abstract_origin].as_reference().name();
    }

    return std::nullopt;
}

void gsdb::dwarf::index_die(const die& current) const {
    // A DIE has an address range if it contains a DW_AT_low_pc or a
    // DW_AT_ranges attribute.
    bool has_range =
        current.contains(DW_AT_low_pc) or current.contains(DW_AT_ranges);
    // DWARF specifies functions with the `DW_TAG_subprogram` tag or, if the DIE
    // represents a function whose body the compiler has copied from elsewhere,
    // the `DW_TAG_inlined_subroutine` tag.
    bool is_function = current.abbrev_entry()->tag == DW_TAG_subprogram or
                       current.abbrev_entry()->tag == DW_TAG_inlined_subroutine;

    if (has_range and is_function) {
        if (auto name = current.name(); name) {
            index_entry entry{current.cu(), current.position()};
            function_index_.emplace(*name, entry);
        }
    }

    for (auto child : current.children()) {
        index_die(child);
    }
}

gsdb::line_table::iterator::iterator(const gsdb::line_table* table)
    : table_(table), pos_(table->data_.begin()) {
    registers_.is_stmt = table->default_is_stmt_;
    ++(*this);
}

gsdb::line_table::iterator gsdb::line_table::begin() const {
    return iterator(this);
}
gsdb::line_table::iterator gsdb::line_table::end() const { return {}; }

gsdb::line_table::iterator& gsdb::line_table::iterator::operator++() {
    if (pos_ == table_->data_.end()) {
        pos_ = nullptr;
        return *this;
    }

    bool emitted = false;
    do {
        emitted = execute_instruction();
    } while (!emitted);
    // at this point, `current_` should hold the row data

    // file indices begin at 1 rather than 0
    current_.file_entry = &table_->file_names_[current_.file_index - 1];
    return *this;
}

gsdb::line_table::iterator gsdb::line_table::iterator::operator++(int) {
    auto tmp = *this;
    ++(*this);
    return tmp;
}

bool gsdb::line_table::iterator::execute_instruction() {
    auto elf = table_->cu_->dwarf_info()->elf_file();
    cursor cur({pos_, table_->data_.end()});
    auto opcode = cur.u8();
    // _most_ instructions do _NOT_ emit matrix rows
    bool emitted = false;

    // standard opcodes are between `1` and `opcode_base - 1`
    if (opcode > 0 and opcode < table_->opcode_base_) {
        switch (opcode) {
            case DW_LNS_copy:
                current_ = registers_;
                registers_.basic_block_start = false;
                registers_.prologue_end = false;
                registers_.epilogue_begin = false;
                registers_.discriminator = 0;
                emitted = true;
                break;
            case DW_LNS_advance_pc:
                registers_.address += cur.uleb128();
                break;
            case DW_LNS_advance_line:
                registers_.line += cur.sleb128();
                break;
            case DW_LNS_set_file:
                registers_.file_index = cur.uleb128();
                break;
            case DW_LNS_set_column:
                registers_.column = cur.uleb128();
                break;
            case DW_LNS_negate_stmt:
                registers_.is_stmt = !registers_.is_stmt;
                break;
            case DW_LNS_set_basic_block:
                registers_.basic_block_start = true;
                break;
            case DW_LNS_const_add_pc:
                registers_.address +=
                    (255 - table_->opcode_base_) / table_->line_range_;
                break;
            case DW_LNS_fixed_advance_pc:
                registers_.address += cur.u16();
                break;
            case DW_LNS_set_prologue_end:
                registers_.prologue_end = true;
                break;
            case DW_LNS_set_epilogue_begin:
                registers_.epilogue_begin = true;
                break;
            case DW_LNS_set_isa:
                break;
            default:
                error::send("Unexpected standard opcode!");
        }
    } else if (opcode == 0) {
        // extended opcodes begin with a `0` byte
        [[maybe_unused]] auto len = cur.uleb128();
        auto extended_opcode = cur.u8();

        switch (extended_opcode) {
            case DW_LNE_end_sequence:  // no operands
                registers_.end_sequence = true;
                current_ = registers_;
                registers_ = entry{};  // reset current registers
                registers_.is_stmt = table_->default_is_stmt_;
                emitted = true;
                break;
            case DW_LNE_set_address:  // one `uint64_t` operand
                registers_.address = file_addr(*elf, cur.u64());
                break;
            case DW_LNE_define_file: {  // one file entry
                auto compilation_dir =
                    table_->cu_->root()[DW_AT_comp_dir].as_string();
                auto file = parse_line_table_file(
                    cur, std::string(compilation_dir), table_->include_dirs_);
                table_->file_names_.push_back(file);
                break;
            }
            case DW_LNE_set_discriminator:  // one uleb128 operand
                registers_.discriminator = cur.uleb128();
                break;
            default:
                error::send("Unexpected extended opcode!");
        }
    } else {
        auto adjusted_opcode = opcode - table_->opcode_base_;
        registers_.address += adjusted_opcode / table_->line_range_;
        registers_.line +=
            table_->line_base_ + (adjusted_opcode % table_->line_range_);
        current_ = registers_;
        registers_.basic_block_start = false;
        registers_.prologue_end = false;
        registers_.epilogue_begin = false;
        registers_.discriminator = 0;
        emitted = true;
    }

    pos_ = cur.position();
    return emitted;
}

gsdb::line_table::iterator gsdb::line_table::get_entry_by_address(
    gsdb::file_addr address) const {
    // the given entry might lie between two entries
    auto prev = begin();
    // if line table is empty
    if (prev == end()) return prev;

    auto it = prev;
    for (++it; it != end(); prev = it++) {
        if (prev->address <= address and it->address > address and
            !prev->end_sequence) {
            return prev;
        }
    }

    return end();
}

std::vector<gsdb::line_table::iterator> gsdb::line_table::get_entries_by_line(
    std::filesystem::path path, std::size_t line) const {
    std::vector<iterator> entries;

    for (auto it = begin(); it != end(); ++it) {
        auto& entry_path = it->file_entry->path;
        if (it->line == line) {
            if ((path.is_absolute() and entry_path == path) or
                (path.is_relative() and path_ends_in(entry_path, path))) {
                entries.push_back(it);
            }
        }
    }

    return entries;
}

gsdb::source_location gsdb::die::location() const { return {&file(), line()}; }

// Line table encodes the file for a DIE as a 1-based index into its filenames
// list.
// If function is inlined, the line table encodes this index as
// `DW_AT_call_file` attribute; otherwise it uses `DW_AT_decl_file`
const gsdb::line_table::file& gsdb::die::file() const {
    std::uint64_t idx;
    if (abbrev_->tag == DW_TAG_inlined_subroutine) {
        idx = (*this)[DW_AT_call_file].as_int();
    } else {
        idx = (*this)[DW_AT_decl_file].as_int();
    }
    return this->cu_->lines().file_names()[idx - 1];
}

std::uint64_t gsdb::die::line() const {
    if (abbrev_->tag == DW_TAG_inlined_subroutine) {
        return (*this)[DW_AT_call_line].as_int();
    }
    return (*this)[DW_AT_decl_line].as_int();
}

std::vector<gsdb::die> gsdb::dwarf::inline_stack_at_address(
    gsdb::file_addr address) const {
    // optional
    auto func = function_containing_address(address);
    std::vector<gsdb::die> stack;

    if (func) {
        stack.push_back(*func);
        // looking for child DIEs that represent inlined functions and contain
        // the given address
        while (true) {
            const auto& chilren = stack.back().children();
            auto found =
                std::find_if(chilren.begin(), chilren.end(), [=](auto& child) {
                    return child.abbrev_entry()->tag ==
                               DW_TAG_inlined_subroutine and
                           child.contains_address(address);
                });
            if (found == chilren.end()) {
                break;
            } else {
                stack.push_back(*found);
            }
        }
    }

    return stack;
}

const gsdb::call_frame_information::common_information_entry&
gsdb::call_frame_information::get_cie(gsdb::file_offset at) const {
    auto offset = at.off();
    // Look up the given offset in the CIE cache.
    if (cie_map_.count(offset)) {
        // If we've already parsed this CIE, return it
        return cie_map_.at(offset);
    }

    auto section = at.elf_file()->get_section_contents(".eh_frame");
    // Construct a cursor from the specified offset into the ELF file up to the
    // end of the `.eh_frame` section
    cursor cur({at.elf_file()->file_offset_as_data_pointer(at), section.end()});
    auto cie = parse_cie(cur);
    cie_map_.emplace(offset, cie);
    return cie_map_.at(offset);
}

/**
 * Binary search
 */
const std::byte* gsdb::call_frame_information::eh_hdr::operator[](
    file_addr address) const {
    auto elf = address.elf_file();
    // used for decoding pointers
    auto text_section_start = *elf->get_section_start_address(".text");
    auto encoding_size = eh_frame_pointer_encoding_size(encoding);
    // Each row of the table stores two values, so the row size is twice the
    // size of the pointer encoding.
    auto row_size = encoding_size * 2;

    std::size_t low = 0;
    std::size_t high = count - 1;
    while (low <= high) {
        std::size_t mid = (low + high) / 2;
        cursor cur(
            {search_table + mid * row_size, search_table + count * row_size});
        auto current_offset = elf->data_pointer_as_file_offset(cur.position());
        auto eh_hdr_offset = elf->data_pointer_as_file_offset(start);
        auto entry_address = parse_eh_frame_pointer(
            *elf, cur, encoding,
            current_offset.off() /* as the program counter value */,
            text_section_start.addr(), eh_hdr_offset.off(), 0);

        if (entry_address < address.addr()) {
            low = mid + 1;
        } else if (entry_address > address.addr()) {
            if (mid == 0) {
                gsdb::error::send("Address not found in eh_hdr!");
            }
            high = mid - 1;
        } else {
            high = mid;
            break;
        }
    }

    // `search_table + high * row_size` gives us a pointer to the start of the
    // desired entry; adding `encoding_size` skips over the `initial_address`
    // value, resulting in a pointer to the encoded FDE pointer
    cursor cur({search_table + high * row_size + encoding_size,
                search_table + count * row_size});
    auto current_offset = elf->data_pointer_as_file_offset(cur.position());
    auto eh_hdr_offset = elf->data_pointer_as_file_offset(start);
    auto fde_offset_int = parse_eh_frame_pointer(
        *elf, cur, encoding, current_offset.off(), text_section_start.addr(),
        eh_hdr_offset.off(), 0);
    gsdb::file_offset fde_offset{*elf, fde_offset_int};
    return elf->file_offset_as_data_pointer(fde_offset);
}
