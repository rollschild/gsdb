#ifndef GSDB_BREAKPOINT_HPP
#define GSDB_BREAKPOINT_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <libgsdb/breakpoint_site.hpp>
#include <libgsdb/stoppoint_collection.hpp>
#include <libgsdb/types.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace gsdb {
class target;
class breakpoint {
   public:
    // If a class has any virtual functions and is meant to be deleted through a
    // base pointer, give it a virtual destructor.
    virtual ~breakpoint() = default;

    breakpoint() = delete;
    breakpoint(const breakpoint&) = delete;
    breakpoint& operator=(const breakpoint&) = delete;

    using id_type = std::int32_t;
    id_type id() const { return id_; }

    void enable();
    void disable();

    bool is_enabled() const { return is_enabled_; }
    bool is_hardware() const { return is_hardware_; }
    bool is_internal() const { return is_internal_; }

    /**
     * Creates breakpoint sites based on the breakpoint type and the arguments
     * passed to it
     */
    virtual void resolve() = 0;
    /**
     * Return the set of breakpoint sites resolved by a breakpoint
     */
    stoppoint_collection<breakpoint_site, false>& breakpoint_sites() {
        return breakpoint_sites_;
    }
    const stoppoint_collection<breakpoint_site, false>& breakpoint_sites()
        const {
        return breakpoint_sites_;
    }

    /**
     * Check whether at least one of the breakpoint sites resolved by this
     * breakpoint is at the requested address or in the requested range
     */
    bool at_address(virt_addr addr) const {
        return breakpoint_sites_.contains_address(addr);
    }
    /**
     * Check whether at least one of the breakpoint sites resolved by this
     * breakpoint is at the requested address or in the requested range
     */
    bool in_range(virt_addr low, virt_addr high) const {
        return !breakpoint_sites_.get_in_region(low, high).empty();
    }

   protected:
    // the type that's managing `gsdb::breakpoint`
    // it a `friend` so that it can construct new `gsdb::breakpoint` objects
    friend target;
    // protected constructor, _NOT_ private
    // because we want derived types to be able to access all members
    breakpoint(target& tgt, bool is_hardware = false, bool is_internal = false);

    id_type id_;
    target* target_;
    bool is_enabled_ = false;
    bool is_hardware_ = false;
    bool is_internal_ = false;
    stoppoint_collection<breakpoint_site, false> breakpoint_sites_;
    breakpoint_site::id_type next_site_id_ = 1;
};

class function_breakpoint : public breakpoint {
   public:
    void resolve() override;
    std::string_view function_name() const { return function_name_; }

   private:
    friend target;
    function_breakpoint(target& target, std::string function_name,
                        bool is_hardware = false, bool is_internal = false)
        : breakpoint(target, is_hardware, is_internal),
          function_name_(std::move(function_name)) {
        // initialize the breakpoint sites for the breakpoint
        resolve();
    }

    std::string function_name_;
};

class line_breakpoint : public breakpoint {
   public:
    void resolve() override;
    const std::filesystem::path file() const { return file_; }
    std::size_t line() const { return line_; }

   private:
    friend target;
    line_breakpoint(target& target, std::filesystem::path file,
                    std::size_t line, bool is_hardware = false,
                    bool is_internal = false)
        : breakpoint(target, is_hardware, is_internal),
          file_(std::move(file)),
          line_(line) {
        resolve();
    }
    std::filesystem::path file_;
    std::size_t line_;
};

class address_breakpoint : public breakpoint {
   public:
    void resolve() override;
    virt_addr address() const { return address_; }

   private:
    friend target;
    address_breakpoint(target& target, virt_addr address,
                       bool is_hardware = false, bool is_internal = false)
        : breakpoint(target, is_hardware, is_internal), address_(address) {
        resolve();
    }
    virt_addr address_;
};
}  // namespace gsdb

#endif
