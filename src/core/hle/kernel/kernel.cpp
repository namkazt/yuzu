// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/arm/arm_interface.h"
#include "core/arm/exclusive_monitor.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/core_timing_util.h"
#include "core/hle/kernel/client_port.h"
#include "core/hle/kernel/errors.h"
#include "core/hle/kernel/handle_table.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/physical_core.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/resource_limit.h"
#include "core/hle/kernel/scheduler.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/lock.h"
#include "core/hle/result.h"
#include "core/memory.h"

namespace Kernel {

/**
 * Callback that will wake up the thread it was scheduled for
 * @param thread_handle The handle of the thread that's been awoken
 * @param cycles_late The number of CPU cycles that have passed since the desired wakeup time
 */
static void ThreadWakeupCallback(u64 thread_handle, [[maybe_unused]] s64 cycles_late) {
    const auto proper_handle = static_cast<Handle>(thread_handle);
    const auto& system = Core::System::GetInstance();

    // Lock the global kernel mutex when we enter the kernel HLE.
    std::lock_guard lock{HLE::g_hle_lock};

    std::shared_ptr<Thread> thread =
        system.Kernel().RetrieveThreadFromWakeupCallbackHandleTable(proper_handle);
    if (thread == nullptr) {
        LOG_CRITICAL(Kernel, "Callback fired for invalid thread {:08X}", proper_handle);
        return;
    }

    bool resume = true;

    if (thread->GetStatus() == ThreadStatus::WaitSynch ||
        thread->GetStatus() == ThreadStatus::WaitHLEEvent) {
        // Remove the thread from each of its waiting objects' waitlists
        for (const auto& object : thread->GetWaitObjects()) {
            object->RemoveWaitingThread(thread);
        }
        thread->ClearWaitObjects();

        // Invoke the wakeup callback before clearing the wait objects
        if (thread->HasWakeupCallback()) {
            resume = thread->InvokeWakeupCallback(ThreadWakeupReason::Timeout, thread, nullptr, 0);
        }
    } else if (thread->GetStatus() == ThreadStatus::WaitMutex ||
               thread->GetStatus() == ThreadStatus::WaitCondVar) {
        thread->SetMutexWaitAddress(0);
        thread->SetWaitHandle(0);
        if (thread->GetStatus() == ThreadStatus::WaitCondVar) {
            thread->GetOwnerProcess()->RemoveConditionVariableThread(thread);
            thread->SetCondVarWaitAddress(0);
        }

        auto* const lock_owner = thread->GetLockOwner();
        // Threads waking up by timeout from WaitProcessWideKey do not perform priority inheritance
        // and don't have a lock owner unless SignalProcessWideKey was called first and the thread
        // wasn't awakened due to the mutex already being acquired.
        if (lock_owner != nullptr) {
            lock_owner->RemoveMutexWaiter(thread);
        }
    }

    if (thread->GetStatus() == ThreadStatus::WaitArb) {
        auto& address_arbiter = thread->GetOwnerProcess()->GetAddressArbiter();
        address_arbiter.HandleWakeupThread(thread);
    }

    if (resume) {
        if (thread->GetStatus() == ThreadStatus::WaitCondVar ||
            thread->GetStatus() == ThreadStatus::WaitArb) {
            thread->SetWaitSynchronizationResult(RESULT_TIMEOUT);
        }
        thread->ResumeFromWait();
    }
}

struct KernelCore::Impl {
    explicit Impl(Core::System& system) : system{system}, global_scheduler{system} {}

    void Initialize(KernelCore& kernel) {
        Shutdown();

        InitializePhysicalCores();
        InitializeSystemResourceLimit(kernel);
        InitializeThreads();
        InitializePreemption();
    }

    void Shutdown() {
        next_object_id = 0;
        next_kernel_process_id = Process::InitialKIPIDMin;
        next_user_process_id = Process::ProcessIDMin;
        next_thread_id = 1;

        process_list.clear();
        current_process = nullptr;

        system_resource_limit = nullptr;

        thread_wakeup_callback_handle_table.Clear();
        thread_wakeup_event_type = nullptr;
        preemption_event = nullptr;

        global_scheduler.Shutdown();

        named_ports.clear();

        for (auto& core : cores) {
            core.Shutdown();
        }
        cores.clear();

        exclusive_monitor.reset();
    }

    void InitializePhysicalCores() {
        exclusive_monitor =
            Core::MakeExclusiveMonitor(system.Memory(), global_scheduler.CpuCoresCount());
        for (std::size_t i = 0; i < global_scheduler.CpuCoresCount(); i++) {
            cores.emplace_back(system, i, *exclusive_monitor);
        }
    }

    // Creates the default system resource limit
    void InitializeSystemResourceLimit(KernelCore& kernel) {
        system_resource_limit = ResourceLimit::Create(kernel);

        // If setting the default system values fails, then something seriously wrong has occurred.
        ASSERT(system_resource_limit->SetLimitValue(ResourceType::PhysicalMemory, 0x200000000)
                   .IsSuccess());
        ASSERT(system_resource_limit->SetLimitValue(ResourceType::Threads, 800).IsSuccess());
        ASSERT(system_resource_limit->SetLimitValue(ResourceType::Events, 700).IsSuccess());
        ASSERT(system_resource_limit->SetLimitValue(ResourceType::TransferMemory, 200).IsSuccess());
        ASSERT(system_resource_limit->SetLimitValue(ResourceType::Sessions, 900).IsSuccess());
    }

    void InitializeThreads() {
        thread_wakeup_event_type =
            Core::Timing::CreateEvent("ThreadWakeupCallback", ThreadWakeupCallback);
    }

    void InitializePreemption() {
        preemption_event =
            Core::Timing::CreateEvent("PreemptionCallback", [this](u64 userdata, s64 cycles_late) {
                global_scheduler.PreemptThreads();
                s64 time_interval = Core::Timing::msToCycles(std::chrono::milliseconds(10));
                system.CoreTiming().ScheduleEvent(time_interval, preemption_event);
            });

        s64 time_interval = Core::Timing::msToCycles(std::chrono::milliseconds(10));
        system.CoreTiming().ScheduleEvent(time_interval, preemption_event);
    }

    void MakeCurrentProcess(Process* process) {
        current_process = process;

        if (process == nullptr) {
            return;
        }

        system.Memory().SetCurrentPageTable(*process);
    }

    std::atomic<u32> next_object_id{0};
    std::atomic<u64> next_kernel_process_id{Process::InitialKIPIDMin};
    std::atomic<u64> next_user_process_id{Process::ProcessIDMin};
    std::atomic<u64> next_thread_id{1};

    // Lists all processes that exist in the current session.
    std::vector<std::shared_ptr<Process>> process_list;
    Process* current_process = nullptr;
    Kernel::GlobalScheduler global_scheduler;

    std::shared_ptr<ResourceLimit> system_resource_limit;

    std::shared_ptr<Core::Timing::EventType> thread_wakeup_event_type;
    std::shared_ptr<Core::Timing::EventType> preemption_event;

    // TODO(yuriks): This can be removed if Thread objects are explicitly pooled in the future,
    // allowing us to simply use a pool index or similar.
    Kernel::HandleTable thread_wakeup_callback_handle_table;

    /// Map of named ports managed by the kernel, which can be retrieved using
    /// the ConnectToPort SVC.
    NamedPortTable named_ports;

    std::unique_ptr<Core::ExclusiveMonitor> exclusive_monitor;
    std::vector<Kernel::PhysicalCore> cores;

    // System context
    Core::System& system;
};

KernelCore::KernelCore(Core::System& system) : impl{std::make_unique<Impl>(system)} {}
KernelCore::~KernelCore() {
    Shutdown();
}

void KernelCore::Initialize() {
    impl->Initialize(*this);
}

void KernelCore::Shutdown() {
    impl->Shutdown();
}

std::shared_ptr<ResourceLimit> KernelCore::GetSystemResourceLimit() const {
    return impl->system_resource_limit;
}

std::shared_ptr<Thread> KernelCore::RetrieveThreadFromWakeupCallbackHandleTable(
    Handle handle) const {
    return impl->thread_wakeup_callback_handle_table.Get<Thread>(handle);
}

void KernelCore::AppendNewProcess(std::shared_ptr<Process> process) {
    impl->process_list.push_back(std::move(process));
}

void KernelCore::MakeCurrentProcess(Process* process) {
    impl->MakeCurrentProcess(process);
}

Process* KernelCore::CurrentProcess() {
    return impl->current_process;
}

const Process* KernelCore::CurrentProcess() const {
    return impl->current_process;
}

const std::vector<std::shared_ptr<Process>>& KernelCore::GetProcessList() const {
    return impl->process_list;
}

Kernel::GlobalScheduler& KernelCore::GlobalScheduler() {
    return impl->global_scheduler;
}

const Kernel::GlobalScheduler& KernelCore::GlobalScheduler() const {
    return impl->global_scheduler;
}

Kernel::PhysicalCore& KernelCore::PhysicalCore(std::size_t id) {
    return impl->cores[id];
}

const Kernel::PhysicalCore& KernelCore::PhysicalCore(std::size_t id) const {
    return impl->cores[id];
}

Core::ExclusiveMonitor& KernelCore::GetExclusiveMonitor() {
    return *impl->exclusive_monitor;
}

const Core::ExclusiveMonitor& KernelCore::GetExclusiveMonitor() const {
    return *impl->exclusive_monitor;
}

void KernelCore::InvalidateAllInstructionCaches() {
    for (std::size_t i = 0; i < impl->global_scheduler.CpuCoresCount(); i++) {
        PhysicalCore(i).ArmInterface().ClearInstructionCache();
    }
}

void KernelCore::PrepareReschedule(std::size_t id) {
    if (id < impl->global_scheduler.CpuCoresCount()) {
        impl->cores[id].Stop();
    }
}

void KernelCore::AddNamedPort(std::string name, std::shared_ptr<ClientPort> port) {
    impl->named_ports.emplace(std::move(name), std::move(port));
}

KernelCore::NamedPortTable::iterator KernelCore::FindNamedPort(const std::string& name) {
    return impl->named_ports.find(name);
}

KernelCore::NamedPortTable::const_iterator KernelCore::FindNamedPort(
    const std::string& name) const {
    return impl->named_ports.find(name);
}

bool KernelCore::IsValidNamedPort(NamedPortTable::const_iterator port) const {
    return port != impl->named_ports.cend();
}

u32 KernelCore::CreateNewObjectID() {
    return impl->next_object_id++;
}

u64 KernelCore::CreateNewThreadID() {
    return impl->next_thread_id++;
}

u64 KernelCore::CreateNewKernelProcessID() {
    return impl->next_kernel_process_id++;
}

u64 KernelCore::CreateNewUserProcessID() {
    return impl->next_user_process_id++;
}

const std::shared_ptr<Core::Timing::EventType>& KernelCore::ThreadWakeupCallbackEventType() const {
    return impl->thread_wakeup_event_type;
}

Kernel::HandleTable& KernelCore::ThreadWakeupCallbackHandleTable() {
    return impl->thread_wakeup_callback_handle_table;
}

const Kernel::HandleTable& KernelCore::ThreadWakeupCallbackHandleTable() const {
    return impl->thread_wakeup_callback_handle_table;
}

} // namespace Kernel
