#include "kcf/introspection/detail/supervisor_registry.hpp"
#include "kcf/introspection/detail/runtime_registry.hpp"
#include <cstring>
#include <limits>
#include <new>
#include <cerrno>
#include <unistd.h>
namespace kcf::detail
{
std::string SupervisorRegistryName(std::int32_t pid, std::uint64_t ticks)
{
    return std::string("/") + SUPERVISOR_REGISTRY_PREFIX + std::to_string(pid) + "_" + std::to_string(ticks);
}
void SupervisorRegistry::TryCreate() noexcept
{
    if (creation_state_ != CreationState::RETRY_PENDING) return;
    ++create_attempts_;
    try { create_error_ = channel_.TryCreate(SupervisorRegistryName(current_.pid, current_.process_start_ticks)); }
    catch (const std::bad_alloc&) { create_error_ = -ENOMEM; }
    catch (...) { create_error_ = -EIO; }
    if (create_error_ == 0) creation_state_ = CreationState::ACTIVE;
    else if (create_error_ != -EAGAIN) creation_state_ = CreationState::FAILED;
    else if (create_attempts_ >= MAX_CREATE_ATTEMPTS) creation_state_ = CreationState::EXHAUSTED;
}
void SupervisorRegistry::Begin() noexcept
{
    // Only End starts a new lifecycle; repeated Begin must not reset the budget.
    if (creation_state_ != CreationState::IDLE) return;
    current_ = {};
    current_.pid = getpid();
    try {
        if (!ReadProcessIdentity(current_.pid, current_.process_start_ticks)) create_error_ = -ESRCH;
    }
    catch (const std::bad_alloc&) { create_error_ = -ENOMEM; }
    catch (...) { create_error_ = -EIO; }
    if (create_error_) { creation_state_ = CreationState::FAILED; return; }
    creation_state_ = CreationState::RETRY_PENDING;
    TryCreate();
}
void SupervisorRegistry::Update(SupervisorInfo snapshot) noexcept
{
    if (creation_state_ == CreationState::IDLE) return;
    snapshot.pid = current_.pid;
    snapshot.process_start_ticks = current_.process_start_ticks;
    snapshot.revision = current_.revision;
    // All fields/reserved bytes are value-initialized, with no implicit padding
    // in the supported Linux layout (sizes asserted in the public model).
    if (!initialized_ || std::memcmp(&snapshot, &current_, sizeof(snapshot)) != 0)
    {
        if (current_.revision == std::numeric_limits<std::uint64_t>::max()) return;
        snapshot.revision++;
        current_ = snapshot; dirty_ = true; initialized_ = true;
    }
    // Cache the latest state before attempting recovery. One nonblocking attempt
    // per Update; publication retries do not consume the creation budget.
    TryCreate();
    if (creation_state_ == CreationState::ACTIVE && dirty_ &&
        channel_.TryPublishSnapshot(current_) == 0) dirty_ = false;
}
void SupervisorRegistry::End() noexcept
{
    (void)channel_.Close();
    if (creation_state_ == CreationState::ACTIVE) (void)channel_.Unlink();
    creation_state_ = CreationState::IDLE;
    create_attempts_ = 0; create_error_ = 0;
    current_ = {}; dirty_ = false; initialized_ = false;
}
}
