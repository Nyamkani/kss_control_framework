#pragma once
#include "kcf/introspection/supervisor_info.hpp"
#include "kcf/ipc/shared_channel.hpp"
namespace kcf::detail
{
inline constexpr char SUPERVISOR_REGISTRY_PREFIX[] = "kcf_introspection_supervisor_";
std::string SupervisorRegistryName(std::int32_t pid, std::uint64_t ticks);
// One owning Bringup, called only on its controlling thread. No observer locks.
class SupervisorRegistry
{
public:
    // Process-local diagnostics only; no shared layout or protocol changes.
    enum class CreationState { IDLE, RETRY_PENDING, ACTIVE, FAILED, EXHAUSTED };
    static constexpr unsigned MAX_CREATE_ATTEMPTS = 64; // includes Begin's attempt
    ~SupervisorRegistry() { End(); }
    void Begin() noexcept;
    void Update(SupervisorInfo snapshot) noexcept;
    void End() noexcept;
    CreationState GetCreationState() const noexcept { return creation_state_; }
    unsigned GetCreateAttempts() const noexcept { return create_attempts_; }
    int GetCreateError() const noexcept { return create_error_; }
private:
    void TryCreate() noexcept;
    SharedChannel<SupervisorInfo> channel_;
    SupervisorInfo current_{};
    CreationState creation_state_{CreationState::IDLE};
    unsigned create_attempts_{0};
    int create_error_{0};
    bool dirty_{false}, initialized_{false};
};
}
