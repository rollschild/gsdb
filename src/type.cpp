#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <libgsdb/type.hpp>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libgsdb/bit.hpp"
#include "libgsdb/detail/dwarf.h"
#include "libgsdb/dwarf.hpp"
#include "libgsdb/error.hpp"
#include "libgsdb/process.hpp"
#include "libgsdb/register_info.hpp"
#include "libgsdb/registers.hpp"
#include "libgsdb/target.hpp"
#include "libgsdb/types.hpp"

namespace {
/**
 * Read the first 64 bits of the data and visualize them as a pointer
 */
std::string visualize_member_pointer_type(const gsdb::typed_data& data) {
    return std::format("0x{:x}",
                       gsdb::from_bytes<std::uintptr_t>(data.data_ptr()));
}

std::string visualize_pointer_type(const gsdb::process& proc,
                                   const gsdb::typed_data& data) {
    // interpret data as 64-bit integer
    auto ptr = gsdb::from_bytes<std::uint64_t>(data.data_ptr());
    if (ptr == 0) {
        return "0x0";
    }
    if (data.value_type().get_die()[DW_AT_type].as_type().is_char_type()) {
        return std::format("\"{}\"", proc.read_string(gsdb::virt_addr{ptr}));
    }
    return std::format("0x{:x}", ptr);
}

std::string visualize_class_type(const gsdb::process& proc,
                                 const gsdb::typed_data& data, int depth) {
    std::string ret = "{\n";
    for (auto& child : data.value_type().get_die().children()) {
        if (child.abbrev_entry()->tag == DW_TAG_member and
            // members of the class, and _NOT_ declared `static`
            (child.contains(DW_AT_data_member_location) or
             child.contains(DW_AT_data_bit_offset))) {
            auto indent = std::string(depth + 1, '\t');
            auto byte_offset = child.contains(DW_AT_data_member_location)
                                   ? child[DW_AT_data_member_location].as_int()
                                   : child[DW_AT_data_bit_offset].as_int() /
                                         8;  // bits converted to bytes
            auto pos = data.data_ptr() + byte_offset;
            auto subtype = child[DW_AT_type].as_type();
            std::vector<std::byte> member_data{pos, pos + subtype.byte_size()};
            auto data = gsdb::typed_data{member_data, subtype}.fixup_bitfield(
                proc, child);
            auto member_str = data.visualize(proc, depth + 1);
            auto name = child.name().value_or("<unnamed>");
            ret += std::format("{}{}: {}\n", indent, name, member_str);
        }
    }

    auto indent = std::string(depth, '\t');
    ret += indent + "}";
    return ret;
}

std::string visualize_subrange(const gsdb::process& proc,
                               const gsdb::type& value_type,
                               gsdb::span<const std::byte> data,
                               std::vector<std::size_t> dimensions) {
    if (dimensions.empty()) {
        std::vector<std::byte> data_vec{data.begin(), data.end()};
        return gsdb::typed_data{std::move(data_vec), value_type}.visualize(
            proc);
    }

    std::string ret = "[";
    auto size = dimensions.back();
    dimensions.pop_back();
    auto sub_size =
        std::accumulate(dimensions.begin(), dimensions.end(),
                        value_type.byte_size(), std::multiplies<>());
    for (std::size_t i = 0; i < size; ++i) {
        gsdb::span<const std::byte> subdata{data.begin() + i * sub_size,
                                            data.end()};
        ret += visualize_subrange(proc, value_type, subdata, dimensions);

        if (i != size - 1) {
            ret += ", ";
        }
    }
    return ret + "]";
}

std::string visualize_array_type(const gsdb::process& proc,
                                 const gsdb::typed_data& data) {
    std::vector<std::size_t> dimensions;
    for (auto& child : data.value_type().get_die().children()) {
        if (child.abbrev_entry()->tag == DW_TAG_subrange_type) {
            dimensions.push_back(child[DW_AT_upper_bound].as_int() + 1);
        }
    }
    std::reverse(dimensions.begin(), dimensions.end());
    auto value_type = data.value_type().get_die()[DW_AT_type].as_type();
    return visualize_subrange(proc, value_type, data.data(), dimensions);
}

std::string visualize_base_type(const gsdb::typed_data& data) {
    auto& type = data.value_type();
    auto die = type.get_die();
    auto ptr = data.data_ptr();

    switch (die[DW_AT_encoding].as_int()) {
        case DW_ATE_boolean:
            return gsdb::from_bytes<bool>(ptr) ? "true" : "false";
        case DW_ATE_float:
            if (die.name() == "float") {
                return std::format("{}", gsdb::from_bytes<float>(ptr));
            }
            if (die.name() == "double") {
                return std::format("{}", gsdb::from_bytes<double>(ptr));
            }
            if (die.name() == "long double") {
                return std::format("{}", gsdb::from_bytes<long double>(ptr));
            }
            gsdb::error::send("Unsupported floating point type!");
        case DW_ATE_signed:
            switch (type.byte_size()) {
                case 1:
                    return std::format("{}",
                                       gsdb::from_bytes<std::int8_t>(ptr));
                case 2:
                    return std::format("{}",
                                       gsdb::from_bytes<std::int16_t>(ptr));
                case 4:
                    return std::format("{}",
                                       gsdb::from_bytes<std::int32_t>(ptr));
                case 8:
                    return std::format("{}",
                                       gsdb::from_bytes<std::int64_t>(ptr));
                default:
                    gsdb::error::send("Unsupported signed integer size!");
            }
        case DW_ATE_unsigned:
            switch (type.byte_size()) {
                case 1:
                    return std::format("{}",
                                       gsdb::from_bytes<std::uint8_t>(ptr));
                case 2:
                    return std::format("{}",
                                       gsdb::from_bytes<std::uint16_t>(ptr));
                case 4:
                    return std::format("{}",
                                       gsdb::from_bytes<std::uint32_t>(ptr));
                case 8:
                    return std::format("{}",
                                       gsdb::from_bytes<std::uint64_t>(ptr));
                default:
                    gsdb::error::send("Unsupported unsigned integer size!");
            }
        case DW_ATE_signed_char:
            return std::format("{}", gsdb::from_bytes<signed char>(ptr));
        case DW_ATE_unsigned_char:
            return std::format("{}", gsdb::from_bytes<unsigned char>(ptr));
        case DW_ATE_UTF:
            gsdb::error::send("DW_ATE_UTF is not implemented!");
        default:
            gsdb::error::send("Unsupported encoding!");
    }
}

gsdb::parameter_class merge_parameter_classes(gsdb::parameter_class lhs,
                                              gsdb::parameter_class rhs) {
    using namespace gsdb;
    if (lhs == rhs) return lhs;

    if (lhs == parameter_class::no_class) return rhs;
    if (rhs == parameter_class::no_class) return lhs;

    if (lhs == parameter_class::memory or rhs == parameter_class::memory) {
        return gsdb::parameter_class::memory;
    }
    if (lhs == parameter_class::integer or rhs == parameter_class::integer) {
        return gsdb::parameter_class::integer;
    }

    if (lhs == parameter_class::x87 or rhs == parameter_class::x87 or
        lhs == parameter_class::x87up or rhs == parameter_class::x87up or
        lhs == parameter_class::complex_x87 or
        rhs == parameter_class::complex_x87) {
        return parameter_class::memory;
    }

    return parameter_class::sse;
}

void classify_class_field(const gsdb::type& type, const gsdb::die& field,
                          std::array<gsdb::parameter_class, 2>& classes,
                          int bit_offset) {
    // the field _could_ be a bit field
    auto bitfield_info = field.get_bitfield_information(type.byte_size());
    auto field_type = field[DW_AT_type].as_type();

    [[maybe_unused]] auto bit_size =
        bitfield_info ? bitfield_info->bit_size : field_type.byte_size() * 8;
    // the current bit offset into the argument we’re classifying is the result
    // of adding the bit_offset argument to either the bitfield’s offset or the
    // byte offset of the field multiplied by eight
    auto current_bit_offset =
        bitfield_info
            ? bitfield_info->bit_offset + bit_offset
            : field[DW_AT_data_member_location].as_int() * 8 + bit_offset;
    // 0 or 1
    auto eightbyte_index = current_bit_offset / 64;

    if (field_type.is_class_type()) {
        for (auto child : field_type.get_die().children()) {
            if (child.abbrev_entry()->tag == DW_TAG_member and
                (child.contains(DW_AT_data_member_location) or
                 child.contains(DW_AT_data_bit_offset))) {
                classify_class_field(type, child, classes, current_bit_offset);
            }
        }
    } else {
        auto field_classes = field_type.get_parameter_classes();
        // always merge the field’s first parameter class with the current
        // parameter class in the eightbyte to which the field belongs
        classes[eightbyte_index] =
            merge_parameter_classes(classes[eightbyte_index], field_classes[0]);
        if (eightbyte_index == 0) {
            // The field may span 2 eightbytes, however, so we also merge the
            // second parameter classes if this field belongs to the first
            // eightbyte
            classes[1] = merge_parameter_classes(classes[1], field_classes[1]);
        }
    }
}

std::array<gsdb::parameter_class, 2> classify_class_type(
    const gsdb::type& type) {
    if (type.is_non_trivial_for_calls()) {
        // passing NTFPOC types by value is _NOT_ supported
        gsdb::error::send("NTFPOC types are not supported!");
    }

    if (type.byte_size() > 16 or type.has_unaligned_fields()) {
        // if type is larger than 2 eightbytes, or
        // contains unaligned fields
        return {gsdb::parameter_class::memory, gsdb::parameter_class::memory};
    }

    std::array<gsdb::parameter_class, 2> classes = {
        gsdb::parameter_class::no_class,
        gsdb::parameter_class::no_class,
    };

    if (type.get_die().abbrev_entry()->tag == DW_TAG_array_type) {
        // classify its values type and use this as the class for the array
        auto value_type = type.get_die()[DW_AT_type].as_type();
        classes = value_type.get_parameter_classes();
        if (type.byte_size() > 8 and
            classes[1] == gsdb::parameter_class::no_class) {
            classes[1] = classes[0];
        }
    } else {  // type is class or union type
        for (auto child : type.get_die().children()) {
            // loop over all non-static data members
            if (child.abbrev_entry()->tag == DW_TAG_member and
                (child.contains(DW_AT_data_member_location) or
                 child.contains(DW_AT_data_bit_offset))) {
                classify_class_field(type, child, classes, 0);
            }
        }
    }

    // post-merger cleanup pass
    // if either eightbyte is passed in memory
    // or x87up is _NOT_ preceded by x87,
    // pass the whole type in memory
    if (classes[0] == gsdb::parameter_class::memory or
        classes[1] == gsdb::parameter_class::memory) {
        classes[0] = classes[1] = gsdb::parameter_class::memory;
    } else if (classes[1] == gsdb::parameter_class::x87up and
               classes[0] != gsdb::parameter_class::x87) {
        classes[0] = classes[1] = gsdb::parameter_class::memory;
    }

    return classes;
}

bool is_destructor(const gsdb::die& func) {
    auto name = func.name();
    return name and name.value().size() > 1 and name.value()[0] == '~';
}

/**
 * A function is considered to be a copy or move constructor if it has the same
 * name as the type and two parameters: a pointer to the type (implicit,
 * generated by DWARF) and an lvalue or rvalue reference to the type.
 */
bool is_copy_or_move_constructor(const gsdb::type& class_type,
                                 const gsdb::die& func) {
    auto class_name = class_type.get_die().name();
    if (class_name != func.name()) return false;

    int i = 0;
    for (auto child : func.children()) {
        if (child.abbrev_entry()->tag == DW_TAG_formal_parameter) {
            if (i == 0) {
                auto type = child[DW_AT_type].as_type();
                if (type.get_die().abbrev_entry()->tag != DW_TAG_pointer_type) {
                    return false;
                }
                if (type.get_die()[DW_AT_type].as_type().strip_cv_typedef() !=
                    class_type) {
                    return false;
                }
            } else if (i == 1) {
                auto type = child[DW_AT_type].as_type();
                auto tag = type.get_die().abbrev_entry()->tag;
                if (tag != DW_TAG_reference_type and
                    tag != DW_TAG_rvalue_reference_type) {
                    return false;
                }

                auto ref =
                    type.get_die()[DW_AT_type].as_type().strip_cv_typedef();
                if (ref != class_type) {
                    return false;
                }
            } else {
                return false;
            }
        }
        ++i;
    }

    return i == 2;
}

}  // namespace

std::size_t gsdb::type::byte_size() const {
    if (!byte_size_.has_value()) {
        byte_size_ = compute_byte_size();
    }
    return *byte_size_;
}

std::size_t gsdb::type::compute_byte_size() const {
    if (!is_from_dwarf()) {
        switch (get_builtin_type()) {
            case gsdb::builtin_type::boolean:
                return 1;
            case gsdb::builtin_type::character:
                return 1;
            case gsdb::builtin_type::integer:
                return 8;
            case gsdb::builtin_type::floating_point:
                return 8;
            case gsdb::builtin_type::string:
                return 8;
        }
    }
    auto& die_ = std::get<gsdb::die>(info_);
    auto tag = die_.abbrev_entry()->tag;

    if (tag == DW_TAG_pointer_type) {
        return 8;
    }
    if (tag == DW_TAG_ptr_to_member_type) {
        // If pointer to a member, its size depends on what type it points to;
        // On linux, pointers to member functions are actually twice as large as
        // pointers to member data, because they store an additional 8 bytes
        // used to support multiple inheritance
        auto member_type = die_[DW_AT_type].as_type();
        if (member_type.get_die().abbrev_entry()->tag ==
            DW_TAG_subroutine_type) {
            return 16;
        }
        return 8;
    }
    if (tag == DW_TAG_array_type) {
        // specifies the element type of the array
        auto value_size = die_[DW_AT_type].as_type().byte_size();
        for (auto& child : die_.children()) {
            //  children of type `DW_TAG_subrange_type` that specify the bounds
            //  of each dimension of the array
            if (child.abbrev_entry()->tag == DW_TAG_subrange_type) {
                value_size *= child[DW_AT_upper_bound].as_int() + 1;
            }
        }
        return value_size;
    }
    if (die_.contains(DW_AT_byte_size)) {
        return die_[DW_AT_byte_size].as_int();
    }
    if (die_.contains(DW_AT_type)) {
        return die_[DW_AT_type].as_type().byte_size();
    }

    return 0;
}

bool gsdb::type::is_char_type() const {
    auto stripped = strip_cv_typedef().get_die();
    if (!stripped.contains(DW_AT_encoding)) return false;
    auto encoding = stripped[DW_AT_encoding].as_int();
    return stripped.abbrev_entry()->tag == DW_TAG_base_type and
           (encoding == DW_ATE_signed_char or encoding == DW_ATE_unsigned_char);
}

std::string gsdb::typed_data::visualize(const gsdb::process& proc,
                                        int depth) const {
    auto die = type_.get_die();
    switch (die.abbrev_entry()->tag) {
        case DW_TAG_base_type:
            return visualize_base_type(*this);
        case DW_TAG_pointer_type:
            return visualize_pointer_type(proc, *this);
        case DW_TAG_ptr_to_member_type:
            return visualize_member_pointer_type(*this);
        case DW_TAG_array_type:
            return visualize_array_type(proc, *this);
        case DW_TAG_class_type:
        case DW_TAG_structure_type:
        case DW_TAG_union_type:
            return visualize_class_type(proc, *this, depth);
        case DW_TAG_enumeration_type:
        case DW_TAG_typedef:
        case DW_TAG_const_type:
        case DW_TAG_volatile_type:
            return typed_data{data_, die[DW_AT_type].as_type()}.visualize(proc);
        default:
            gsdb::error::send("Unsupported type!");
    }
}

gsdb::typed_data gsdb::typed_data::deref_pointer(const process& proc) const {
    auto stripped_type_die = type_.strip_cv_typedef().get_die();
    auto tag = stripped_type_die.abbrev_entry()->tag;
    if (tag != DW_TAG_pointer_type) {
        gsdb::error::send("Not a pointer type!");
    }
    virt_addr address{gsdb::from_bytes<std::uint64_t>(data_.data())};
    auto value_type = stripped_type_die[DW_AT_type].as_type();
    auto data_vec = proc.read_memory(address, value_type.byte_size());
    return {std::move(data_vec), value_type, address};
}

/**
 * Find the DIE corresponding to the member with the given name and read the
 * data at its location
 */
gsdb::typed_data gsdb::typed_data::read_member(
    const process& proc, std::string_view member_name) const {
    auto die = type_.get_die();
    auto children = die.children();
    auto it = std::find_if(children.begin(), children.end(), [&](auto& child) {
        return child.name().value_or("") == member_name;
    });
    if (it == children.end()) {
        gsdb::error::send("No such member!");
    }

    auto var = *it;
    auto value_type = var[DW_AT_type].as_type();

    auto byte_offset = var.contains(DW_AT_data_member_location)
                           ? var[DW_AT_data_member_location].as_int()
                           : var[DW_AT_data_bit_offset].as_int() / 8;
    auto data_start = data_.begin() + byte_offset;
    std::vector<std::byte> member_data{data_start,
                                       data_start + value_type.byte_size()};

    auto data = address_ ? typed_data{std::move(member_data), value_type,
                                      *address_ + byte_offset}
                         : typed_data{std::move(member_data), value_type};
    return data.fixup_bitfield(proc, var);
}

gsdb::typed_data gsdb::typed_data::index(const process& proc,
                                         std::size_t index) const {
    auto parent_type = type_.strip_cv_typedef().get_die();
    auto tag = parent_type.abbrev_entry()->tag;
    if (tag != DW_TAG_array_type and tag != DW_TAG_pointer_type) {
        gsdb::error::send("Not an array or pointer type!");
    }

    // calculate the size of the values that are pointed to or stored in the
    // array
    auto value_type = parent_type[DW_AT_type].as_type();
    auto element_size = value_type.byte_size();
    auto offset = index * element_size;
    if (tag == DW_TAG_pointer_type) {
        //  interpret the given data as a virtual address
        virt_addr address{gsdb::from_bytes<std::uint64_t>(data_.data())};
        address += offset;
        auto data_vec = proc.read_memory(address, element_size);
        return {std::move(data_vec), value_type, address};
    } else {
        if (element_size == 0 or index >= data_.size() / element_size) {
            gsdb::error::send("Index out of range!");
        }
        std::vector<std::byte> data_vec{data_.begin() + offset,
                                        data_.begin() + offset + element_size};
        if (address_) {
            return {std::move(data_vec), value_type, *address_ + offset};
        }
        return {std::move(data_vec), value_type};
    }
}

bool gsdb::type::operator==(const type& rhs) const {
    if (!is_from_dwarf() and !rhs.is_from_dwarf()) {
        // if both types are builtin
        return get_builtin_type() == rhs.get_builtin_type();
    }

    // One type is builtin and the other is not
    const gsdb::type* from_dwarf = nullptr;
    const gsdb::type* builtin = nullptr;
    if (!is_from_dwarf()) {
        from_dwarf = &rhs;
        builtin = this;
    } else if (!rhs.is_from_dwarf()) {
        from_dwarf = this;
        builtin = &rhs;
    }

    if (from_dwarf and builtin) {
        auto die = from_dwarf->strip_cvref_typedef().get_die();
        auto tag = die.abbrev_entry()->tag;

        if (tag == DW_TAG_base_type) {
            switch (die[DW_AT_encoding].as_int()) {
                case DW_ATE_boolean:
                    return builtin->get_builtin_type() == builtin_type::boolean;
                case DW_ATE_float:
                    return builtin->get_builtin_type() ==
                           builtin_type::floating_point;
                case DW_ATE_signed:
                case DW_ATE_unsigned:
                    return builtin->get_builtin_type() == builtin_type::integer;
                case DW_ATE_signed_char:
                case DW_ATE_unsigned_char:
                    return builtin->get_builtin_type() ==
                           builtin_type::character;
                default:
                    return false;
            }
        }

        if (tag == DW_TAG_pointer_type) {
            // the DWARF pointer type pionts to a char type, and builtin type is
            // a string
            return die[DW_AT_type].as_type().is_char_type() and
                   builtin->get_builtin_type() == builtin_type::string;
        }

        return false;
    }

    // Both types come from DWARF
    auto lhs_stripped = strip_all();
    auto rhs_stripped = rhs.strip_all();

    auto lhs_name = lhs_stripped.get_die().name();
    auto rhs_name = rhs_stripped.get_die().name();
    if (lhs_name and rhs_name and *lhs_name == *rhs_name) {
        return true;
    }

    return false;
}

std::array<gsdb::parameter_class, 2> gsdb::type::get_parameter_classes() const {
    std::array<parameter_class, 2> classes = {
        parameter_class::no_class,
        parameter_class::no_class,
    };
    // builtin types, which include literal arguments the user passes on CLI
    if (!is_from_dwarf()) {
        switch (get_builtin_type()) {
            case gsdb::builtin_type::boolean:
                classes[0] = parameter_class::integer;
                break;
            case gsdb::builtin_type::character:
                classes[0] = parameter_class::integer;
                break;
            case gsdb::builtin_type::integer:
                classes[0] = parameter_class::integer;
                break;
            case gsdb::builtin_type::floating_point:
                classes[0] = parameter_class::sse;
                break;
            case gsdb::builtin_type::string:
                classes[0] = parameter_class::integer;
                break;
        }
        return classes;
    }

    // arguments that come from the program itself (that is, named variables)
    auto stripped = strip_cv_typedef();
    auto die = stripped.get_die();
    auto tag = die.abbrev_entry()->tag;
    if (tag == DW_TAG_base_type and stripped.byte_size() <= 8) {
        //  base types that fit in a single eightbyte
        switch (die[DW_AT_encoding].as_int()) {
            case DW_ATE_boolean:
                classes[0] = parameter_class::integer;
                break;
            case DW_ATE_float:
                classes[0] = parameter_class::sse;
                break;
            case DW_ATE_signed:
                classes[0] = parameter_class::integer;
                break;
            case DW_ATE_signed_char:
                classes[0] = parameter_class::integer;
                break;
            case DW_ATE_unsigned:
                classes[0] = parameter_class::integer;
                break;
            case DW_ATE_unsigned_char:
                classes[0] = parameter_class::integer;
                break;
            default:
                gsdb::error::send("Unimplemented base type encoding!");
        }
    } else if (tag == DW_TAG_pointer_type or tag == DW_TAG_reference_type or
               tag == DW_TAG_rvalue_reference_type) {
        // pointer and reference types
        classes[0] = parameter_class::integer;
    } else if (tag == DW_TAG_base_type and
               die[DW_AT_encoding].as_int() == DW_ATE_float and
               stripped.byte_size() == 16) {
        // `long double` only
        classes[0] = parameter_class::x87;
        classes[1] = parameter_class::x87up;
    } else if (tag == DW_TAG_class_type or tag == DW_TAG_structure_type or
               tag == DW_TAG_union_type or tag == DW_TAG_array_type) {
        // class and array types
        classes = classify_class_type(*this);
    }

    return classes;
}

bool gsdb::type::is_class_type() const {
    if (!is_from_dwarf()) {
        return false;
    }

    auto stripped = strip_cv_typedef().get_die();
    auto tag = stripped.abbrev_entry()->tag;
    return tag == DW_TAG_class_type or tag == DW_TAG_structure_type or
           tag == DW_TAG_union_type;
}

bool gsdb::type::is_reference_type() const {
    if (!is_from_dwarf()) {
        return false;
    }

    auto stripped = strip_cv_typedef().get_die();
    auto tag = stripped.abbrev_entry()->tag;
    return tag == DW_TAG_reference_type or tag == DW_TAG_rvalue_reference_type;
}

bool gsdb::type::is_non_trivial_for_calls() const {
    auto stripped = strip_cv_typedef().get_die();
    auto tag = stripped.abbrev_entry()->tag;

    if (tag == DW_TAG_class_type or tag == DW_TAG_structure_type or
        tag == DW_TAG_union_type) {  // loop through all child DIEs
        for (auto& child : stripped.children()) {
            //  If any non-static data members are NTFPOC, we return true
            if (child.abbrev_entry()->tag == DW_TAG_member and
                (child.contains(DW_AT_data_member_location) or
                 child.contains(DW_AT_data_bit_offset))) {
                if (child[DW_AT_type].as_type().is_non_trivial_for_calls()) {
                    return true;
                }
            }

            //  If any base classes are NTFPOC, we return true
            if (child.abbrev_entry()->tag == DW_TAG_inheritance) {
                if (child[DW_AT_type].as_type().is_non_trivial_for_calls()) {
                    return true;
                }
            }

            // If the type has any virtual data members or virtual base classes,
            // represented by a `DW_AT_virtuality` member with a value of either
            // `DW_VIRTUALITY_virtual` or `DW_VIRTUALITY_pure_virtual`, we
            // return true
            if (child.contains(DW_AT_virtuality) and
                child[DW_AT_virtuality].as_int() != DW_VIRTUALITY_none) {
                return true;
            }

            if (child.abbrev_entry()->tag == DW_TAG_subprogram) {
                // If the type has a copy/move constructor or destructor
                // child that isn’t defaulted
                if (is_copy_or_move_constructor(*this, child)) {
                    if (!child.contains(DW_AT_defaulted) or
                        ((!child[DW_AT_defaulted].as_int()) !=
                         DW_DEFAULTED_in_class)) {
                        return true;
                    }
                } else if (is_destructor(child)) {
                    if (!child.contains(DW_AT_defaulted) or
                        ((!child[DW_AT_defaulted].as_int()) !=
                         DW_DEFAULTED_in_class)) {
                        return true;
                    }
                }
            }
        }
    }

    if (tag == DW_TAG_array_type) {
        return stripped[DW_AT_type].as_type().is_non_trivial_for_calls();
    }

    return false;
}

std::size_t gsdb::type::alignment() const {
    if (!is_from_dwarf()) {
        return byte_size();
    }

    if (is_class_type()) {
        // Class types should be aligned to the same boundary as their most
        // strictly aligned member (that is, the member with the largest
        // alignment boundary expectation).
        std::size_t max_alignment = 0;
        for (auto child : get_die().children()) {
            if (child.abbrev_entry()->tag == DW_TAG_member and
                (child.contains(DW_AT_data_member_location) or
                 child.contains(DW_AT_data_bit_offset))) {
                auto member_type = child[DW_AT_type].as_type();
                if (member_type.alignment() > max_alignment) {
                    max_alignment = member_type.alignment();
                }
            }
        }
        return max_alignment;
    }

    // Arrays should be aligned to the same boundary as their element type.
    if (get_die().abbrev_entry()->tag == DW_TAG_array_type) {
        return get_die()[DW_AT_type].as_type().alignment();
    }

    // Other types should be aligned to the same boundary as their byte size.
    return byte_size();
}

bool gsdb::type::has_unaligned_fields() const {
    if (!is_from_dwarf()) {
        return false;
    }

    if (is_class_type()) {
        for (auto child : get_die().children()) {
            if (child.abbrev_entry()->tag == DW_TAG_member and
                child.contains(DW_AT_data_member_location)) {
                // If the member’s byte offset isn’t aligned to the expected
                // boundary
                auto member_type = child[DW_AT_type].as_type();
                if (child[DW_AT_data_member_location].as_int() %
                        member_type.alignment() !=
                    0) {
                    return true;
                }
                // If the member itself has unaligned fields
                if (member_type.has_unaligned_fields()) {
                    return true;
                }
            }
        }
    }

    return false;
}
