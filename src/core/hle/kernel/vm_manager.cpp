// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstring>
#include <iterator>
#include <utility>
#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/memory_hook.h"
#include "core/core.h"
#include "core/file_sys/program_metadata.h"
#include "core/hle/kernel/errors.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/resource_limit.h"
#include "core/hle/kernel/vm_manager.h"
#include "core/memory.h"

namespace Kernel {
namespace {
const char* GetMemoryStateName(MemoryState state) {
    static constexpr const char* names[] = {
        "Unmapped",       "Io",
        "Normal",         "Code",
        "CodeData",       "Heap",
        "Shared",         "Unknown1",
        "ModuleCode",     "ModuleCodeData",
        "IpcBuffer0",     "Stack",
        "ThreadLocal",    "TransferMemoryIsolated",
        "TransferMemory", "ProcessMemory",
        "Inaccessible",   "IpcBuffer1",
        "IpcBuffer3",     "KernelStack",
    };

    return names[ToSvcMemoryState(state)];
}

// Checks if a given address range lies within a larger address range.
constexpr bool IsInsideAddressRange(VAddr address, u64 size, VAddr address_range_begin,
                                    VAddr address_range_end) {
    const VAddr end_address = address + size - 1;
    return address_range_begin <= address && end_address <= address_range_end - 1;
}
} // Anonymous namespace

bool VirtualMemoryArea::CanBeMergedWith(const VirtualMemoryArea& next) const {
    ASSERT(base + size == next.base);
    if (permissions != next.permissions || state != next.state || attribute != next.attribute ||
        type != next.type) {
        return false;
    }
    if ((attribute & MemoryAttribute::DeviceMapped) == MemoryAttribute::DeviceMapped) {
        // TODO: Can device mapped memory be merged sanely?
        // Not merging it may cause inaccuracies versus hardware when memory layout is queried.
        return false;
    }
    if (type == VMAType::AllocatedMemoryBlock) {
        return true;
    }
    if (type == VMAType::BackingMemory && backing_memory + size != next.backing_memory) {
        return false;
    }
    if (type == VMAType::MMIO && paddr + size != next.paddr) {
        return false;
    }
    return true;
}

VMManager::VMManager(Core::System& system) : system{system} {
    // Default to assuming a 39-bit address space. This way we have a sane
    // starting point with executables that don't provide metadata.
    Reset(FileSys::ProgramAddressSpaceType::Is39Bit);
}

VMManager::~VMManager() = default;

void VMManager::Reset(FileSys::ProgramAddressSpaceType type) {
    Clear();

    InitializeMemoryRegionRanges(type);

    page_table.Resize(address_space_width);

    // Initialize the map with a single free region covering the entire managed space.
    VirtualMemoryArea initial_vma;
    initial_vma.size = address_space_end;
    vma_map.emplace(initial_vma.base, initial_vma);

    UpdatePageTableForVMA(initial_vma);
}

VMManager::VMAHandle VMManager::FindVMA(VAddr target) const {
    if (target >= address_space_end) {
        return vma_map.end();
    } else {
        return std::prev(vma_map.upper_bound(target));
    }
}

bool VMManager::IsValidHandle(VMAHandle handle) const {
    return handle != vma_map.cend();
}

ResultVal<VMManager::VMAHandle> VMManager::MapMemoryBlock(VAddr target,
                                                          std::shared_ptr<PhysicalMemory> block,
                                                          std::size_t offset, u64 size,
                                                          MemoryState state, VMAPermission perm) {
    ASSERT(block != nullptr);
    ASSERT(offset + size <= block->size());

    // This is the appropriately sized VMA that will turn into our allocation.
    CASCADE_RESULT(VMAIter vma_handle, CarveVMA(target, size));
    VirtualMemoryArea& final_vma = vma_handle->second;
    ASSERT(final_vma.size == size);

    final_vma.type = VMAType::AllocatedMemoryBlock;
    final_vma.permissions = perm;
    final_vma.state = state;
    final_vma.backing_block = std::move(block);
    final_vma.offset = offset;
    UpdatePageTableForVMA(final_vma);

    return MakeResult<VMAHandle>(MergeAdjacent(vma_handle));
}

ResultVal<VMManager::VMAHandle> VMManager::MapBackingMemory(VAddr target, u8* memory, u64 size,
                                                            MemoryState state) {
    ASSERT(memory != nullptr);

    // This is the appropriately sized VMA that will turn into our allocation.
    CASCADE_RESULT(VMAIter vma_handle, CarveVMA(target, size));
    VirtualMemoryArea& final_vma = vma_handle->second;
    ASSERT(final_vma.size == size);

    final_vma.type = VMAType::BackingMemory;
    final_vma.permissions = VMAPermission::ReadWrite;
    final_vma.state = state;
    final_vma.backing_memory = memory;
    UpdatePageTableForVMA(final_vma);

    return MakeResult<VMAHandle>(MergeAdjacent(vma_handle));
}

ResultVal<VAddr> VMManager::FindFreeRegion(u64 size) const {
    return FindFreeRegion(GetASLRRegionBaseAddress(), GetASLRRegionEndAddress(), size);
}

ResultVal<VAddr> VMManager::FindFreeRegion(VAddr begin, VAddr end, u64 size) const {
    ASSERT(begin < end);
    ASSERT(size <= end - begin);

    const VMAHandle vma_handle =
        std::find_if(vma_map.begin(), vma_map.end(), [begin, end, size](const auto& vma) {
            if (vma.second.type != VMAType::Free) {
                return false;
            }
            const VAddr vma_base = vma.second.base;
            const VAddr vma_end = vma_base + vma.second.size;
            const VAddr assumed_base = (begin < vma_base) ? vma_base : begin;
            const VAddr used_range = assumed_base + size;

            return vma_base <= assumed_base && assumed_base < used_range && used_range < end &&
                   used_range <= vma_end;
        });

    if (vma_handle == vma_map.cend()) {
        // TODO(Subv): Find the correct error code here.
        return RESULT_UNKNOWN;
    }

    const VAddr target = std::max(begin, vma_handle->second.base);
    return MakeResult<VAddr>(target);
}

ResultVal<VMManager::VMAHandle> VMManager::MapMMIO(VAddr target, PAddr paddr, u64 size,
                                                   MemoryState state,
                                                   Common::MemoryHookPointer mmio_handler) {
    // This is the appropriately sized VMA that will turn into our allocation.
    CASCADE_RESULT(VMAIter vma_handle, CarveVMA(target, size));
    VirtualMemoryArea& final_vma = vma_handle->second;
    ASSERT(final_vma.size == size);

    final_vma.type = VMAType::MMIO;
    final_vma.permissions = VMAPermission::ReadWrite;
    final_vma.state = state;
    final_vma.paddr = paddr;
    final_vma.mmio_handler = std::move(mmio_handler);
    UpdatePageTableForVMA(final_vma);

    return MakeResult<VMAHandle>(MergeAdjacent(vma_handle));
}

VMManager::VMAIter VMManager::Unmap(VMAIter vma_handle) {
    VirtualMemoryArea& vma = vma_handle->second;
    vma.type = VMAType::Free;
    vma.permissions = VMAPermission::None;
    vma.state = MemoryState::Unmapped;
    vma.attribute = MemoryAttribute::None;

    vma.backing_block = nullptr;
    vma.offset = 0;
    vma.backing_memory = nullptr;
    vma.paddr = 0;

    UpdatePageTableForVMA(vma);

    return MergeAdjacent(vma_handle);
}

ResultCode VMManager::UnmapRange(VAddr target, u64 size) {
    CASCADE_RESULT(VMAIter vma, CarveVMARange(target, size));
    const VAddr target_end = target + size;

    const VMAIter end = vma_map.end();
    // The comparison against the end of the range must be done using addresses since VMAs can be
    // merged during this process, causing invalidation of the iterators.
    while (vma != end && vma->second.base < target_end) {
        vma = std::next(Unmap(vma));
    }

    ASSERT(FindVMA(target)->second.size >= size);

    return RESULT_SUCCESS;
}

VMManager::VMAHandle VMManager::Reprotect(VMAHandle vma_handle, VMAPermission new_perms) {
    VMAIter iter = StripIterConstness(vma_handle);

    VirtualMemoryArea& vma = iter->second;
    vma.permissions = new_perms;
    UpdatePageTableForVMA(vma);

    return MergeAdjacent(iter);
}

ResultCode VMManager::ReprotectRange(VAddr target, u64 size, VMAPermission new_perms) {
    CASCADE_RESULT(VMAIter vma, CarveVMARange(target, size));
    const VAddr target_end = target + size;

    const VMAIter end = vma_map.end();
    // The comparison against the end of the range must be done using addresses since VMAs can be
    // merged during this process, causing invalidation of the iterators.
    while (vma != end && vma->second.base < target_end) {
        vma = std::next(StripIterConstness(Reprotect(vma, new_perms)));
    }

    return RESULT_SUCCESS;
}

ResultVal<VAddr> VMManager::SetHeapSize(u64 size) {
    if (size > GetHeapRegionSize()) {
        return ERR_OUT_OF_MEMORY;
    }

    // No need to do any additional work if the heap is already the given size.
    if (size == GetCurrentHeapSize()) {
        return MakeResult(heap_region_base);
    }

    if (heap_memory == nullptr) {
        // Initialize heap
        heap_memory = std::make_shared<PhysicalMemory>(size);
        heap_end = heap_region_base + size;
    } else {
        UnmapRange(heap_region_base, GetCurrentHeapSize());
    }

    // If necessary, expand backing vector to cover new heap extents in
    // the case of allocating. Otherwise, shrink the backing memory,
    // if a smaller heap has been requested.
    heap_memory->resize(size);
    heap_memory->shrink_to_fit();
    RefreshMemoryBlockMappings(heap_memory.get());

    heap_end = heap_region_base + size;
    ASSERT(GetCurrentHeapSize() == heap_memory->size());

    const auto mapping_result =
        MapMemoryBlock(heap_region_base, heap_memory, 0, size, MemoryState::Heap);
    if (mapping_result.Failed()) {
        return mapping_result.Code();
    }

    return MakeResult<VAddr>(heap_region_base);
}

ResultCode VMManager::MapPhysicalMemory(VAddr target, u64 size) {
    // Check how much memory we've already mapped.
    const auto mapped_size_result = SizeOfAllocatedVMAsInRange(target, size);
    if (mapped_size_result.Failed()) {
        return mapped_size_result.Code();
    }

    // If we've already mapped the desired amount, return early.
    const std::size_t mapped_size = *mapped_size_result;
    if (mapped_size == size) {
        return RESULT_SUCCESS;
    }

    // Check that we can map the memory we want.
    const auto res_limit = system.CurrentProcess()->GetResourceLimit();
    const u64 physmem_remaining = res_limit->GetMaxResourceValue(ResourceType::PhysicalMemory) -
                                  res_limit->GetCurrentResourceValue(ResourceType::PhysicalMemory);
    if (physmem_remaining < (size - mapped_size)) {
        return ERR_RESOURCE_LIMIT_EXCEEDED;
    }

    // Keep track of the memory regions we unmap.
    std::vector<std::pair<u64, u64>> mapped_regions;
    ResultCode result = RESULT_SUCCESS;

    // Iterate, trying to map memory.
    {
        const auto end_addr = target + size;
        const auto last_addr = end_addr - 1;
        VAddr cur_addr = target;

        auto iter = FindVMA(target);
        ASSERT(iter != vma_map.end());

        while (true) {
            const auto& vma = iter->second;
            const auto vma_start = vma.base;
            const auto vma_end = vma_start + vma.size;
            const auto vma_last = vma_end - 1;

            // Map the memory block
            const auto map_size = std::min(end_addr - cur_addr, vma_end - cur_addr);
            if (vma.state == MemoryState::Unmapped) {
                const auto map_res =
                    MapMemoryBlock(cur_addr, std::make_shared<PhysicalMemory>(map_size), 0,
                                   map_size, MemoryState::Heap, VMAPermission::ReadWrite);
                result = map_res.Code();
                if (result.IsError()) {
                    break;
                }

                mapped_regions.emplace_back(cur_addr, map_size);
            }

            // Break once we hit the end of the range.
            if (last_addr <= vma_last) {
                break;
            }

            // Advance to the next block.
            cur_addr = vma_end;
            iter = FindVMA(cur_addr);
            ASSERT(iter != vma_map.end());
        }
    }

    // If we failed, unmap memory.
    if (result.IsError()) {
        for (const auto [unmap_address, unmap_size] : mapped_regions) {
            ASSERT_MSG(UnmapRange(unmap_address, unmap_size).IsSuccess(),
                       "Failed to unmap memory range.");
        }

        return result;
    }

    // Update amount of mapped physical memory.
    physical_memory_mapped += size - mapped_size;

    return RESULT_SUCCESS;
}

ResultCode VMManager::UnmapPhysicalMemory(VAddr target, u64 size) {
    // Check how much memory is currently mapped.
    const auto mapped_size_result = SizeOfUnmappablePhysicalMemoryInRange(target, size);
    if (mapped_size_result.Failed()) {
        return mapped_size_result.Code();
    }

    // If we've already unmapped all the memory, return early.
    const std::size_t mapped_size = *mapped_size_result;
    if (mapped_size == 0) {
        return RESULT_SUCCESS;
    }

    // Keep track of the memory regions we unmap.
    std::vector<std::pair<u64, u64>> unmapped_regions;
    ResultCode result = RESULT_SUCCESS;

    // Try to unmap regions.
    {
        const auto end_addr = target + size;
        const auto last_addr = end_addr - 1;
        VAddr cur_addr = target;

        auto iter = FindVMA(target);
        ASSERT(iter != vma_map.end());

        while (true) {
            const auto& vma = iter->second;
            const auto vma_start = vma.base;
            const auto vma_end = vma_start + vma.size;
            const auto vma_last = vma_end - 1;

            // Unmap the memory block
            const auto unmap_size = std::min(end_addr - cur_addr, vma_end - cur_addr);
            if (vma.state == MemoryState::Heap) {
                result = UnmapRange(cur_addr, unmap_size);
                if (result.IsError()) {
                    break;
                }

                unmapped_regions.emplace_back(cur_addr, unmap_size);
            }

            // Break once we hit the end of the range.
            if (last_addr <= vma_last) {
                break;
            }

            // Advance to the next block.
            cur_addr = vma_end;
            iter = FindVMA(cur_addr);
            ASSERT(iter != vma_map.end());
        }
    }

    // If we failed, re-map regions.
    // TODO: Preserve memory contents?
    if (result.IsError()) {
        for (const auto [map_address, map_size] : unmapped_regions) {
            const auto remap_res =
                MapMemoryBlock(map_address, std::make_shared<PhysicalMemory>(map_size), 0, map_size,
                               MemoryState::Heap, VMAPermission::None);
            ASSERT_MSG(remap_res.Succeeded(), "Failed to remap a memory block.");
        }

        return result;
    }

    // Update mapped amount
    physical_memory_mapped -= mapped_size;

    return RESULT_SUCCESS;
}

ResultCode VMManager::MapCodeMemory(VAddr dst_address, VAddr src_address, u64 size) {
    constexpr auto ignore_attribute = MemoryAttribute::LockedForIPC | MemoryAttribute::DeviceMapped;
    const auto src_check_result = CheckRangeState(
        src_address, size, MemoryState::All, MemoryState::Heap, VMAPermission::All,
        VMAPermission::ReadWrite, MemoryAttribute::Mask, MemoryAttribute::None, ignore_attribute);

    if (src_check_result.Failed()) {
        return src_check_result.Code();
    }

    const auto mirror_result =
        MirrorMemory(dst_address, src_address, size, MemoryState::ModuleCode);
    if (mirror_result.IsError()) {
        return mirror_result;
    }

    // Ensure we lock the source memory region.
    const auto src_vma_result = CarveVMARange(src_address, size);
    if (src_vma_result.Failed()) {
        return src_vma_result.Code();
    }
    auto src_vma_iter = *src_vma_result;
    src_vma_iter->second.attribute = MemoryAttribute::Locked;
    Reprotect(src_vma_iter, VMAPermission::Read);

    // The destination memory region is fine as is, however we need to make it read-only.
    return ReprotectRange(dst_address, size, VMAPermission::Read);
}

ResultCode VMManager::UnmapCodeMemory(VAddr dst_address, VAddr src_address, u64 size) {
    constexpr auto ignore_attribute = MemoryAttribute::LockedForIPC | MemoryAttribute::DeviceMapped;
    const auto src_check_result = CheckRangeState(
        src_address, size, MemoryState::All, MemoryState::Heap, VMAPermission::None,
        VMAPermission::None, MemoryAttribute::Mask, MemoryAttribute::Locked, ignore_attribute);

    if (src_check_result.Failed()) {
        return src_check_result.Code();
    }

    // Yes, the kernel only checks the first page of the region.
    const auto dst_check_result =
        CheckRangeState(dst_address, Memory::PAGE_SIZE, MemoryState::FlagModule,
                        MemoryState::FlagModule, VMAPermission::None, VMAPermission::None,
                        MemoryAttribute::Mask, MemoryAttribute::None, ignore_attribute);

    if (dst_check_result.Failed()) {
        return dst_check_result.Code();
    }

    const auto dst_memory_state = std::get<MemoryState>(*dst_check_result);
    const auto dst_contiguous_check_result = CheckRangeState(
        dst_address, size, MemoryState::All, dst_memory_state, VMAPermission::None,
        VMAPermission::None, MemoryAttribute::Mask, MemoryAttribute::None, ignore_attribute);

    if (dst_contiguous_check_result.Failed()) {
        return dst_contiguous_check_result.Code();
    }

    const auto unmap_result = UnmapRange(dst_address, size);
    if (unmap_result.IsError()) {
        return unmap_result;
    }

    // With the mirrored portion unmapped, restore the original region's traits.
    const auto src_vma_result = CarveVMARange(src_address, size);
    if (src_vma_result.Failed()) {
        return src_vma_result.Code();
    }
    auto src_vma_iter = *src_vma_result;
    src_vma_iter->second.state = MemoryState::Heap;
    src_vma_iter->second.attribute = MemoryAttribute::None;
    Reprotect(src_vma_iter, VMAPermission::ReadWrite);

    if (dst_memory_state == MemoryState::ModuleCode) {
        system.InvalidateCpuInstructionCaches();
    }

    return unmap_result;
}

MemoryInfo VMManager::QueryMemory(VAddr address) const {
    const auto vma = FindVMA(address);
    MemoryInfo memory_info{};

    if (IsValidHandle(vma)) {
        memory_info.base_address = vma->second.base;
        memory_info.attributes = ToSvcMemoryAttribute(vma->second.attribute);
        memory_info.permission = static_cast<u32>(vma->second.permissions);
        memory_info.size = vma->second.size;
        memory_info.state = ToSvcMemoryState(vma->second.state);
    } else {
        memory_info.base_address = address_space_end;
        memory_info.permission = static_cast<u32>(VMAPermission::None);
        memory_info.size = 0 - address_space_end;
        memory_info.state = static_cast<u32>(MemoryState::Inaccessible);
    }

    return memory_info;
}

ResultCode VMManager::SetMemoryAttribute(VAddr address, u64 size, MemoryAttribute mask,
                                         MemoryAttribute attribute) {
    constexpr auto ignore_mask =
        MemoryAttribute::Uncached | MemoryAttribute::DeviceMapped | MemoryAttribute::Locked;
    constexpr auto attribute_mask = ~ignore_mask;

    const auto result = CheckRangeState(
        address, size, MemoryState::FlagUncached, MemoryState::FlagUncached, VMAPermission::None,
        VMAPermission::None, attribute_mask, MemoryAttribute::None, ignore_mask);

    if (result.Failed()) {
        return result.Code();
    }

    const auto [prev_state, prev_permissions, prev_attributes] = *result;
    const auto new_attribute = (prev_attributes & ~mask) | (mask & attribute);

    const auto carve_result = CarveVMARange(address, size);
    if (carve_result.Failed()) {
        return carve_result.Code();
    }

    auto vma_iter = *carve_result;
    vma_iter->second.attribute = new_attribute;

    MergeAdjacent(vma_iter);
    return RESULT_SUCCESS;
}

ResultCode VMManager::MirrorMemory(VAddr dst_addr, VAddr src_addr, u64 size, MemoryState state) {
    const auto vma = FindVMA(src_addr);

    ASSERT_MSG(vma != vma_map.end(), "Invalid memory address");
    ASSERT_MSG(vma->second.backing_block, "Backing block doesn't exist for address");

    // The returned VMA might be a bigger one encompassing the desired address.
    const auto vma_offset = src_addr - vma->first;
    ASSERT_MSG(vma_offset + size <= vma->second.size,
               "Shared memory exceeds bounds of mapped block");

    const std::shared_ptr<PhysicalMemory>& backing_block = vma->second.backing_block;
    const std::size_t backing_block_offset = vma->second.offset + vma_offset;

    CASCADE_RESULT(auto new_vma,
                   MapMemoryBlock(dst_addr, backing_block, backing_block_offset, size, state));
    // Protect mirror with permissions from old region
    Reprotect(new_vma, vma->second.permissions);
    // Remove permissions from old region
    ReprotectRange(src_addr, size, VMAPermission::None);

    return RESULT_SUCCESS;
}

void VMManager::RefreshMemoryBlockMappings(const PhysicalMemory* block) {
    // If this ever proves to have a noticeable performance impact, allow users of the function to
    // specify a specific range of addresses to limit the scan to.
    for (const auto& p : vma_map) {
        const VirtualMemoryArea& vma = p.second;
        if (block == vma.backing_block.get()) {
            UpdatePageTableForVMA(vma);
        }
    }
}

void VMManager::LogLayout() const {
    for (const auto& p : vma_map) {
        const VirtualMemoryArea& vma = p.second;
        LOG_DEBUG(Kernel, "{:016X} - {:016X} size: {:016X} {}{}{} {}", vma.base,
                  vma.base + vma.size, vma.size,
                  (u8)vma.permissions & (u8)VMAPermission::Read ? 'R' : '-',
                  (u8)vma.permissions & (u8)VMAPermission::Write ? 'W' : '-',
                  (u8)vma.permissions & (u8)VMAPermission::Execute ? 'X' : '-',
                  GetMemoryStateName(vma.state));
    }
}

VMManager::VMAIter VMManager::StripIterConstness(const VMAHandle& iter) {
    // This uses a neat C++ trick to convert a const_iterator to a regular iterator, given
    // non-const access to its container.
    return vma_map.erase(iter, iter); // Erases an empty range of elements
}

ResultVal<VMManager::VMAIter> VMManager::CarveVMA(VAddr base, u64 size) {
    ASSERT_MSG((size & Memory::PAGE_MASK) == 0, "non-page aligned size: 0x{:016X}", size);
    ASSERT_MSG((base & Memory::PAGE_MASK) == 0, "non-page aligned base: 0x{:016X}", base);

    VMAIter vma_handle = StripIterConstness(FindVMA(base));
    if (vma_handle == vma_map.end()) {
        // Target address is outside the range managed by the kernel
        return ERR_INVALID_ADDRESS;
    }

    const VirtualMemoryArea& vma = vma_handle->second;
    if (vma.type != VMAType::Free) {
        // Region is already allocated
        return ERR_INVALID_ADDRESS_STATE;
    }

    const VAddr start_in_vma = base - vma.base;
    const VAddr end_in_vma = start_in_vma + size;

    if (end_in_vma > vma.size) {
        // Requested allocation doesn't fit inside VMA
        return ERR_INVALID_ADDRESS_STATE;
    }

    if (end_in_vma != vma.size) {
        // Split VMA at the end of the allocated region
        SplitVMA(vma_handle, end_in_vma);
    }
    if (start_in_vma != 0) {
        // Split VMA at the start of the allocated region
        vma_handle = SplitVMA(vma_handle, start_in_vma);
    }

    return MakeResult<VMAIter>(vma_handle);
}

ResultVal<VMManager::VMAIter> VMManager::CarveVMARange(VAddr target, u64 size) {
    ASSERT_MSG((size & Memory::PAGE_MASK) == 0, "non-page aligned size: 0x{:016X}", size);
    ASSERT_MSG((target & Memory::PAGE_MASK) == 0, "non-page aligned base: 0x{:016X}", target);

    const VAddr target_end = target + size;
    ASSERT(target_end >= target);
    ASSERT(target_end <= address_space_end);
    ASSERT(size > 0);

    VMAIter begin_vma = StripIterConstness(FindVMA(target));
    const VMAIter i_end = vma_map.lower_bound(target_end);
    if (std::any_of(begin_vma, i_end,
                    [](const auto& entry) { return entry.second.type == VMAType::Free; })) {
        return ERR_INVALID_ADDRESS_STATE;
    }

    if (target != begin_vma->second.base) {
        begin_vma = SplitVMA(begin_vma, target - begin_vma->second.base);
    }

    VMAIter end_vma = StripIterConstness(FindVMA(target_end));
    if (end_vma != vma_map.end() && target_end != end_vma->second.base) {
        end_vma = SplitVMA(end_vma, target_end - end_vma->second.base);
    }

    return MakeResult<VMAIter>(begin_vma);
}

VMManager::VMAIter VMManager::SplitVMA(VMAIter vma_handle, u64 offset_in_vma) {
    VirtualMemoryArea& old_vma = vma_handle->second;
    VirtualMemoryArea new_vma = old_vma; // Make a copy of the VMA

    // For now, don't allow no-op VMA splits (trying to split at a boundary) because it's probably
    // a bug. This restriction might be removed later.
    ASSERT(offset_in_vma < old_vma.size);
    ASSERT(offset_in_vma > 0);

    old_vma.size = offset_in_vma;
    new_vma.base += offset_in_vma;
    new_vma.size -= offset_in_vma;

    switch (new_vma.type) {
    case VMAType::Free:
        break;
    case VMAType::AllocatedMemoryBlock:
        new_vma.offset += offset_in_vma;
        break;
    case VMAType::BackingMemory:
        new_vma.backing_memory += offset_in_vma;
        break;
    case VMAType::MMIO:
        new_vma.paddr += offset_in_vma;
        break;
    }

    ASSERT(old_vma.CanBeMergedWith(new_vma));

    return vma_map.emplace_hint(std::next(vma_handle), new_vma.base, new_vma);
}

VMManager::VMAIter VMManager::MergeAdjacent(VMAIter iter) {
    const VMAIter next_vma = std::next(iter);
    if (next_vma != vma_map.end() && iter->second.CanBeMergedWith(next_vma->second)) {
        MergeAdjacentVMA(iter->second, next_vma->second);
        vma_map.erase(next_vma);
    }

    if (iter != vma_map.begin()) {
        VMAIter prev_vma = std::prev(iter);
        if (prev_vma->second.CanBeMergedWith(iter->second)) {
            MergeAdjacentVMA(prev_vma->second, iter->second);
            vma_map.erase(iter);
            iter = prev_vma;
        }
    }

    return iter;
}

void VMManager::MergeAdjacentVMA(VirtualMemoryArea& left, const VirtualMemoryArea& right) {
    ASSERT(left.CanBeMergedWith(right));

    // Always merge allocated memory blocks, even when they don't share the same backing block.
    if (left.type == VMAType::AllocatedMemoryBlock &&
        (left.backing_block != right.backing_block || left.offset + left.size != right.offset)) {

        // Check if we can save work.
        if (left.offset == 0 && left.size == left.backing_block->size()) {
            // Fast case: left is an entire backing block.
            left.backing_block->resize(left.size + right.size);
            std::memcpy(left.backing_block->data() + left.size,
                        right.backing_block->data() + right.offset, right.size);
        } else {
            // Slow case: make a new memory block for left and right.
            auto new_memory = std::make_shared<PhysicalMemory>();
            new_memory->resize(left.size + right.size);
            std::memcpy(new_memory->data(), left.backing_block->data() + left.offset, left.size);
            std::memcpy(new_memory->data() + left.size, right.backing_block->data() + right.offset,
                        right.size);

            left.backing_block = std::move(new_memory);
            left.offset = 0;
        }

        // Page table update is needed, because backing memory changed.
        left.size += right.size;
        UpdatePageTableForVMA(left);
    } else {
        // Just update the size.
        left.size += right.size;
    }
}

void VMManager::UpdatePageTableForVMA(const VirtualMemoryArea& vma) {
    auto& memory = system.Memory();

    switch (vma.type) {
    case VMAType::Free:
        memory.UnmapRegion(page_table, vma.base, vma.size);
        break;
    case VMAType::AllocatedMemoryBlock:
        memory.MapMemoryRegion(page_table, vma.base, vma.size, *vma.backing_block, vma.offset);
        break;
    case VMAType::BackingMemory:
        memory.MapMemoryRegion(page_table, vma.base, vma.size, vma.backing_memory);
        break;
    case VMAType::MMIO:
        memory.MapIoRegion(page_table, vma.base, vma.size, vma.mmio_handler);
        break;
    }
}

void VMManager::InitializeMemoryRegionRanges(FileSys::ProgramAddressSpaceType type) {
    u64 map_region_size = 0;
    u64 heap_region_size = 0;
    u64 stack_region_size = 0;
    u64 tls_io_region_size = 0;

    u64 stack_and_tls_io_end = 0;

    switch (type) {
    case FileSys::ProgramAddressSpaceType::Is32Bit:
    case FileSys::ProgramAddressSpaceType::Is32BitNoMap:
        address_space_width = 32;
        code_region_base = 0x200000;
        code_region_end = code_region_base + 0x3FE00000;
        aslr_region_base = 0x200000;
        aslr_region_end = aslr_region_base + 0xFFE00000;
        if (type == FileSys::ProgramAddressSpaceType::Is32Bit) {
            map_region_size = 0x40000000;
            heap_region_size = 0x40000000;
        } else {
            map_region_size = 0;
            heap_region_size = 0x80000000;
        }
        stack_and_tls_io_end = 0x40000000;
        break;
    case FileSys::ProgramAddressSpaceType::Is36Bit:
        address_space_width = 36;
        code_region_base = 0x8000000;
        code_region_end = code_region_base + 0x78000000;
        aslr_region_base = 0x8000000;
        aslr_region_end = aslr_region_base + 0xFF8000000;
        map_region_size = 0x180000000;
        heap_region_size = 0x180000000;
        stack_and_tls_io_end = 0x80000000;
        break;
    case FileSys::ProgramAddressSpaceType::Is39Bit:
        address_space_width = 39;
        code_region_base = 0x8000000;
        code_region_end = code_region_base + 0x80000000;
        aslr_region_base = 0x8000000;
        aslr_region_end = aslr_region_base + 0x7FF8000000;
        map_region_size = 0x1000000000;
        heap_region_size = 0x180000000;
        stack_region_size = 0x80000000;
        tls_io_region_size = 0x1000000000;
        break;
    default:
        UNREACHABLE_MSG("Invalid address space type specified: {}", static_cast<u32>(type));
        return;
    }

    const u64 stack_and_tls_io_begin = aslr_region_base;

    address_space_base = 0;
    address_space_end = 1ULL << address_space_width;

    map_region_base = code_region_end;
    map_region_end = map_region_base + map_region_size;

    heap_region_base = map_region_end;
    heap_region_end = heap_region_base + heap_region_size;
    heap_end = heap_region_base;

    stack_region_base = heap_region_end;
    stack_region_end = stack_region_base + stack_region_size;

    tls_io_region_base = stack_region_end;
    tls_io_region_end = tls_io_region_base + tls_io_region_size;

    if (stack_region_size == 0) {
        stack_region_base = stack_and_tls_io_begin;
        stack_region_end = stack_and_tls_io_end;
    }

    if (tls_io_region_size == 0) {
        tls_io_region_base = stack_and_tls_io_begin;
        tls_io_region_end = stack_and_tls_io_end;
    }
}

void VMManager::Clear() {
    ClearVMAMap();
    ClearPageTable();
}

void VMManager::ClearVMAMap() {
    vma_map.clear();
}

void VMManager::ClearPageTable() {
    std::fill(page_table.pointers.begin(), page_table.pointers.end(), nullptr);
    page_table.special_regions.clear();
    std::fill(page_table.attributes.begin(), page_table.attributes.end(),
              Common::PageType::Unmapped);
}

VMManager::CheckResults VMManager::CheckRangeState(VAddr address, u64 size, MemoryState state_mask,
                                                   MemoryState state, VMAPermission permission_mask,
                                                   VMAPermission permissions,
                                                   MemoryAttribute attribute_mask,
                                                   MemoryAttribute attribute,
                                                   MemoryAttribute ignore_mask) const {
    auto iter = FindVMA(address);

    // If we don't have a valid VMA handle at this point, then it means this is
    // being called with an address outside of the address space, which is definitely
    // indicative of a bug, as this function only operates on mapped memory regions.
    DEBUG_ASSERT(IsValidHandle(iter));

    const VAddr end_address = address + size - 1;
    const MemoryAttribute initial_attributes = iter->second.attribute;
    const VMAPermission initial_permissions = iter->second.permissions;
    const MemoryState initial_state = iter->second.state;

    while (true) {
        // The iterator should be valid throughout the traversal. Hitting the end of
        // the mapped VMA regions is unquestionably indicative of a bug.
        DEBUG_ASSERT(IsValidHandle(iter));

        const auto& vma = iter->second;

        if (vma.state != initial_state) {
            return ERR_INVALID_ADDRESS_STATE;
        }

        if ((vma.state & state_mask) != state) {
            return ERR_INVALID_ADDRESS_STATE;
        }

        if (vma.permissions != initial_permissions) {
            return ERR_INVALID_ADDRESS_STATE;
        }

        if ((vma.permissions & permission_mask) != permissions) {
            return ERR_INVALID_ADDRESS_STATE;
        }

        if ((vma.attribute | ignore_mask) != (initial_attributes | ignore_mask)) {
            return ERR_INVALID_ADDRESS_STATE;
        }

        if ((vma.attribute & attribute_mask) != attribute) {
            return ERR_INVALID_ADDRESS_STATE;
        }

        if (end_address <= vma.EndAddress()) {
            break;
        }

        ++iter;
    }

    return MakeResult(
        std::make_tuple(initial_state, initial_permissions, initial_attributes & ~ignore_mask));
}

ResultVal<std::size_t> VMManager::SizeOfAllocatedVMAsInRange(VAddr address,
                                                             std::size_t size) const {
    const VAddr end_addr = address + size;
    const VAddr last_addr = end_addr - 1;
    std::size_t mapped_size = 0;

    VAddr cur_addr = address;
    auto iter = FindVMA(cur_addr);
    ASSERT(iter != vma_map.end());

    while (true) {
        const auto& vma = iter->second;
        const VAddr vma_start = vma.base;
        const VAddr vma_end = vma_start + vma.size;
        const VAddr vma_last = vma_end - 1;

        // Add size if relevant.
        if (vma.state != MemoryState::Unmapped) {
            mapped_size += std::min(end_addr - cur_addr, vma_end - cur_addr);
        }

        // Break once we hit the end of the range.
        if (last_addr <= vma_last) {
            break;
        }

        // Advance to the next block.
        cur_addr = vma_end;
        iter = std::next(iter);
        ASSERT(iter != vma_map.end());
    }

    return MakeResult(mapped_size);
}

ResultVal<std::size_t> VMManager::SizeOfUnmappablePhysicalMemoryInRange(VAddr address,
                                                                        std::size_t size) const {
    const VAddr end_addr = address + size;
    const VAddr last_addr = end_addr - 1;
    std::size_t mapped_size = 0;

    VAddr cur_addr = address;
    auto iter = FindVMA(cur_addr);
    ASSERT(iter != vma_map.end());

    while (true) {
        const auto& vma = iter->second;
        const auto vma_start = vma.base;
        const auto vma_end = vma_start + vma.size;
        const auto vma_last = vma_end - 1;
        const auto state = vma.state;
        const auto attr = vma.attribute;

        // Memory within region must be free or mapped heap.
        if (!((state == MemoryState::Heap && attr == MemoryAttribute::None) ||
              (state == MemoryState::Unmapped))) {
            return ERR_INVALID_ADDRESS_STATE;
        }

        // Add size if relevant.
        if (state != MemoryState::Unmapped) {
            mapped_size += std::min(end_addr - cur_addr, vma_end - cur_addr);
        }

        // Break once we hit the end of the range.
        if (last_addr <= vma_last) {
            break;
        }

        // Advance to the next block.
        cur_addr = vma_end;
        iter = std::next(iter);
        ASSERT(iter != vma_map.end());
    }

    return MakeResult(mapped_size);
}

u64 VMManager::GetTotalPhysicalMemoryAvailable() const {
    LOG_WARNING(Kernel, "(STUBBED) called");
    return 0xF8000000;
}

VAddr VMManager::GetAddressSpaceBaseAddress() const {
    return address_space_base;
}

VAddr VMManager::GetAddressSpaceEndAddress() const {
    return address_space_end;
}

u64 VMManager::GetAddressSpaceSize() const {
    return address_space_end - address_space_base;
}

u64 VMManager::GetAddressSpaceWidth() const {
    return address_space_width;
}

bool VMManager::IsWithinAddressSpace(VAddr address, u64 size) const {
    return IsInsideAddressRange(address, size, GetAddressSpaceBaseAddress(),
                                GetAddressSpaceEndAddress());
}

VAddr VMManager::GetASLRRegionBaseAddress() const {
    return aslr_region_base;
}

VAddr VMManager::GetASLRRegionEndAddress() const {
    return aslr_region_end;
}

u64 VMManager::GetASLRRegionSize() const {
    return aslr_region_end - aslr_region_base;
}

bool VMManager::IsWithinASLRRegion(VAddr begin, u64 size) const {
    const VAddr range_end = begin + size;
    const VAddr aslr_start = GetASLRRegionBaseAddress();
    const VAddr aslr_end = GetASLRRegionEndAddress();

    if (aslr_start > begin || begin > range_end || range_end - 1 > aslr_end - 1) {
        return false;
    }

    if (range_end > heap_region_base && heap_region_end > begin) {
        return false;
    }

    if (range_end > map_region_base && map_region_end > begin) {
        return false;
    }

    return true;
}

VAddr VMManager::GetCodeRegionBaseAddress() const {
    return code_region_base;
}

VAddr VMManager::GetCodeRegionEndAddress() const {
    return code_region_end;
}

u64 VMManager::GetCodeRegionSize() const {
    return code_region_end - code_region_base;
}

bool VMManager::IsWithinCodeRegion(VAddr address, u64 size) const {
    return IsInsideAddressRange(address, size, GetCodeRegionBaseAddress(),
                                GetCodeRegionEndAddress());
}

VAddr VMManager::GetHeapRegionBaseAddress() const {
    return heap_region_base;
}

VAddr VMManager::GetHeapRegionEndAddress() const {
    return heap_region_end;
}

u64 VMManager::GetHeapRegionSize() const {
    return heap_region_end - heap_region_base;
}

u64 VMManager::GetCurrentHeapSize() const {
    return heap_end - heap_region_base;
}

bool VMManager::IsWithinHeapRegion(VAddr address, u64 size) const {
    return IsInsideAddressRange(address, size, GetHeapRegionBaseAddress(),
                                GetHeapRegionEndAddress());
}

VAddr VMManager::GetMapRegionBaseAddress() const {
    return map_region_base;
}

VAddr VMManager::GetMapRegionEndAddress() const {
    return map_region_end;
}

u64 VMManager::GetMapRegionSize() const {
    return map_region_end - map_region_base;
}

bool VMManager::IsWithinMapRegion(VAddr address, u64 size) const {
    return IsInsideAddressRange(address, size, GetMapRegionBaseAddress(), GetMapRegionEndAddress());
}

VAddr VMManager::GetStackRegionBaseAddress() const {
    return stack_region_base;
}

VAddr VMManager::GetStackRegionEndAddress() const {
    return stack_region_end;
}

u64 VMManager::GetStackRegionSize() const {
    return stack_region_end - stack_region_base;
}

bool VMManager::IsWithinStackRegion(VAddr address, u64 size) const {
    return IsInsideAddressRange(address, size, GetStackRegionBaseAddress(),
                                GetStackRegionEndAddress());
}

VAddr VMManager::GetTLSIORegionBaseAddress() const {
    return tls_io_region_base;
}

VAddr VMManager::GetTLSIORegionEndAddress() const {
    return tls_io_region_end;
}

u64 VMManager::GetTLSIORegionSize() const {
    return tls_io_region_end - tls_io_region_base;
}

bool VMManager::IsWithinTLSIORegion(VAddr address, u64 size) const {
    return IsInsideAddressRange(address, size, GetTLSIORegionBaseAddress(),
                                GetTLSIORegionEndAddress());
}

} // namespace Kernel
