// Copyright 2019 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>

#include "core/hle/kernel/object.h"
#include "core/hle/kernel/physical_memory.h"

union ResultCode;

namespace Memory {
class Memory;
}

namespace Kernel {

class KernelCore;
class Process;

enum class MemoryPermission : u32;

/// Defines the interface for transfer memory objects.
///
/// Transfer memory is typically used for the purpose of
/// transferring memory between separate process instances,
/// thus the name.
///
class TransferMemory final : public Object {
public:
    explicit TransferMemory(KernelCore& kernel, Memory::Memory& memory);
    ~TransferMemory() override;

    static constexpr HandleType HANDLE_TYPE = HandleType::TransferMemory;

    static std::shared_ptr<TransferMemory> Create(KernelCore& kernel, Memory::Memory& memory,
                                                  VAddr base_address, u64 size,
                                                  MemoryPermission permissions);

    TransferMemory(const TransferMemory&) = delete;
    TransferMemory& operator=(const TransferMemory&) = delete;

    TransferMemory(TransferMemory&&) = delete;
    TransferMemory& operator=(TransferMemory&&) = delete;

    std::string GetTypeName() const override {
        return "TransferMemory";
    }

    std::string GetName() const override {
        return GetTypeName();
    }

    HandleType GetHandleType() const override {
        return HANDLE_TYPE;
    }

    /// Gets a pointer to the backing block of this instance.
    const u8* GetPointer() const;

    /// Gets the size of the memory backing this instance in bytes.
    u64 GetSize() const;

    /// Attempts to map transfer memory with the given range and memory permissions.
    ///
    /// @param address     The base address to being mapping memory at.
    /// @param size        The size of the memory to map, in bytes.
    /// @param permissions The memory permissions to check against when mapping memory.
    ///
    /// @pre The given address, size, and memory permissions must all match
    ///      the same values that were given when creating the transfer memory
    ///      instance.
    ///
    ResultCode MapMemory(VAddr address, u64 size, MemoryPermission permissions);

    /// Unmaps the transfer memory with the given range
    ///
    /// @param address The base address to begin unmapping memory at.
    /// @param size    The size of the memory to unmap, in bytes.
    ///
    /// @pre The given address and size must be the same as the ones used
    ///      to create the transfer memory instance.
    ///
    ResultCode UnmapMemory(VAddr address, u64 size);

    /// Reserves the region to be used for the transfer memory, called after the transfer memory is
    /// created.
    ResultCode Reserve();

    /// Resets the region previously used for the transfer memory, called after the transfer memory
    /// is closed.
    ResultCode Reset();

private:
    /// Memory block backing this instance.
    std::shared_ptr<PhysicalMemory> backing_block;

    /// The base address for the memory managed by this instance.
    VAddr base_address = 0;

    /// Size of the memory, in bytes, that this instance manages.
    u64 memory_size = 0;

    /// The memory permissions that are applied to this instance.
    MemoryPermission owner_permissions{};

    /// The process that this transfer memory instance was created under.
    Process* owner_process = nullptr;

    /// Whether or not this transfer memory instance has mapped memory.
    bool is_mapped = false;

    Memory::Memory& memory;
};

} // namespace Kernel
