#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <libgsdb/type.hpp>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libgsdb/bit.hpp"
#include "libgsdb/detail/dwarf.h"
#include "libgsdb/error.hpp"
#include "libgsdb/process.hpp"
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

}  // namespace

std::size_t gsdb::type::byte_size() const {
    if (!byte_size_.has_value()) {
        byte_size_ = compute_byte_size();
    }
    return *byte_size_;
}

std::size_t gsdb::type::compute_byte_size() const {
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
        std::vector<std::byte> data_vec{data_.begin() + offset,
                                        data_.begin() + offset + element_size};
        if (address_) {
            return {std::move(data_vec), value_type, *address_ + offset};
        }
        return {std::move(data_vec), value_type};
    }
}
