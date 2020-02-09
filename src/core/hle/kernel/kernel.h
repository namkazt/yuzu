// Copyright 2014 Citra Emulator Project / PPSSPP Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "core/hle/kernel/object.h"

namespace Core {
class ExclusiveMonitor;
class System;
} // namespace Core

namespace Core::Timing {
class CoreTiming;
struct EventType;
} // namespace Core::Timing

namespace Kernel {

class AddressArbiter;
class ClientPort;
class GlobalScheduler;
class HandleTable;
class PhysicalCore;
class Process;
class ResourceLimit;
class Thread;

/// Represents a single instance of the kernel.
class KernelCore {
private:
    using NamedPortTable = std::unordered_map<std::string, std::shared_ptr<ClientPort>>;

public:
    /// Constructs an instance of the kernel using the given System
    /// instance as a context for any necessary system-related state,
    /// such as threads, CPU core state, etc.
    ///
    /// @post After execution of the constructor, the provided System
    ///       object *must* outlive the kernel instance itself.
    ///
    explicit KernelCore(Core::System& system);
    ~KernelCore();

    KernelCore(const KernelCore&) = delete;
    KernelCore& operator=(const KernelCore&) = delete;

    KernelCore(KernelCore&&) = delete;
    KernelCore& operator=(KernelCore&&) = delete;

    /// Resets the kernel to a clean slate for use.
    void Initialize();

    /// Clears all resources in use by the kernel instance.
    void Shutdown();

    /// Retrieves a shared pointer to the system resource limit instance.
    std::shared_ptr<ResourceLimit> GetSystemResourceLimit() const;

    /// Retrieves a shared pointer to a Thread instance within the thread wakeup handle table.
    std::shared_ptr<Thread> RetrieveThreadFromWakeupCallbackHandleTable(Handle handle) const;

    /// Adds the given shared pointer to an internal list of active processes.
    void AppendNewProcess(std::shared_ptr<Process> process);

    /// Makes the given process the new current process.
    void MakeCurrentProcess(Process* process);

    /// Retrieves a pointer to the current process.
    Process* CurrentProcess();

    /// Retrieves a const pointer to the current process.
    const Process* CurrentProcess() const;

    /// Retrieves the list of processes.
    const std::vector<std::shared_ptr<Process>>& GetProcessList() const;

    /// Gets the sole instance of the global scheduler
    Kernel::GlobalScheduler& GlobalScheduler();

    /// Gets the sole instance of the global scheduler
    const Kernel::GlobalScheduler& GlobalScheduler() const;

    /// Gets the an instance of the respective physical CPU core.
    Kernel::PhysicalCore& PhysicalCore(std::size_t id);

    /// Gets the an instance of the respective physical CPU core.
    const Kernel::PhysicalCore& PhysicalCore(std::size_t id) const;

    /// Stops execution of 'id' core, in order to reschedule a new thread.
    void PrepareReschedule(std::size_t id);

    Core::ExclusiveMonitor& GetExclusiveMonitor();

    const Core::ExclusiveMonitor& GetExclusiveMonitor() const;

    void InvalidateAllInstructionCaches();

    /// Adds a port to the named port table
    void AddNamedPort(std::string name, std::shared_ptr<ClientPort> port);

    /// Finds a port within the named port table with the given name.
    NamedPortTable::iterator FindNamedPort(const std::string& name);

    /// Finds a port within the named port table with the given name.
    NamedPortTable::const_iterator FindNamedPort(const std::string& name) const;

    /// Determines whether or not the given port is a valid named port.
    bool IsValidNamedPort(NamedPortTable::const_iterator port) const;

private:
    friend class Object;
    friend class Process;
    friend class Thread;

    /// Creates a new object ID, incrementing the internal object ID counter.
    u32 CreateNewObjectID();

    /// Creates a new process ID, incrementing the internal process ID counter;
    u64 CreateNewKernelProcessID();

    /// Creates a new process ID, incrementing the internal process ID counter;
    u64 CreateNewUserProcessID();

    /// Creates a new thread ID, incrementing the internal thread ID counter.
    u64 CreateNewThreadID();

    /// Retrieves the event type used for thread wakeup callbacks.
    const std::shared_ptr<Core::Timing::EventType>& ThreadWakeupCallbackEventType() const;

    /// Provides a reference to the thread wakeup callback handle table.
    Kernel::HandleTable& ThreadWakeupCallbackHandleTable();

    /// Provides a const reference to the thread wakeup callback handle table.
    const Kernel::HandleTable& ThreadWakeupCallbackHandleTable() const;

    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Kernel
