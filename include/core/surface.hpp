#pragma once
#include "shm/validator.hpp"
#include "core/types.hpp"
#include "platform/input/types.hpp"
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <memory>
#include <optional>
#include <utility>
#include <limits>
#include <type_traits>

namespace yarilo::core {

// КОНТРАКТ ПОТОКОВОЙ МОДЕЛИ:
// Класс SurfaceManager строго однопоточен (Single-Threaded Reactor). 
// Все методы вызываются исключительно из главного потока событий сервера.

enum class BufferUsage {
    None,      // Не привязан к коммитам
    Pending,   // Удерживается в очереди pending_commit
    Active     // Удерживается в active_commit
};

enum class BindingReleaseState {
    Retained,  // Удерживается сервером
    Released   // Логическое освобождение зафиксировано, сформирован ReleaseInfo
};

struct BufferBinding {
    uint64_t binding_uid{0};
    std::shared_ptr<yarilo::shm::MappedBuffer> mapping;
    BufferUsage usage{BufferUsage::None};
    BindingReleaseState release_state{BindingReleaseState::Retained};
    uint32_t active_leases_count{0};

    uint32_t session_id{0};
    uint32_t local_surface_id{0};
    uint32_t buffer_id{0};
    uint64_t epoch_commit_seq{0};
};

struct QueuedCommit {
    uint64_t binding_uid{0};
    uint32_t buffer_id{0};
    uint64_t commit_seq{0};
    uint32_t width{0};
    uint32_t height{0};
};

enum class MapDeliveryState {
    None,
    Queued,
    Sent
};

class Surface {
public:
    uint64_t server_uid{0};
    uint64_t global_id{0};
    uint32_t local_id{0};
    uint32_t owner_session_id{0};
    uint64_t creation_order{0};

    int32_t x{0}, y{0};
    uint32_t width{0}, height{0};
    uint32_t fixed_width{0}, fixed_height{0}; // Размеры, зафиксированные при первом коммите
    uint32_t z_index{0};
    
    bool is_mapped_by_wm{false};
    MapDeliveryState map_state{MapDeliveryState::None};
    uint64_t last_commit_seq{0};

    std::unordered_map<uint32_t, uint64_t> attached_bindings;
    std::optional<QueuedCommit> pending_commit;
    std::optional<QueuedCommit> active_commit;
};

struct RenderNode {
    uint64_t lease_id;
    uint64_t global_id;
    int32_t dst_x, dst_y;
    uint32_t src_width, src_height;
    uint32_t src_stride;
    std::shared_ptr<yarilo::shm::MappedBuffer> mapping_ref;
    uint32_t z_index;
    uint64_t creation_order;
    
    uint32_t session_id;
    uint32_t local_surface_id;
    uint32_t buffer_id;
    uint64_t commit_seq;
};

struct MapRequestInfo {
    uint64_t global_id;
    uint64_t server_uid;
    uint32_t width;
    uint32_t height;
};

struct ReleaseInfo {
    uint32_t session_id;
    uint32_t local_surface_id;
    uint32_t buffer_id;
    uint64_t commit_seq;
};

struct CommitResult {
    std::optional<MapRequestInfo> map_request;
    std::vector<ReleaseInfo> superseded_releases;
};

struct DestroySurfaceResult {
    bool wm_notified{false};
    uint64_t server_uid{0};
    std::vector<ReleaseInfo> releases;
};

struct DestroyClientResult {
    std::vector<uint64_t> wm_surfaces_to_destroy;
    std::vector<ReleaseInfo> releases;
};

struct SnapshotResult {
    std::vector<RenderNode> nodes;
    std::vector<ReleaseInfo> newly_released;
};

struct ActiveLease {
    uint64_t lease_id{0};
    uint64_t binding_uid{0};
};

static_assert(
    std::is_nothrow_copy_assignable_v<std::optional<QueuedCommit>>,
    "std::optional<QueuedCommit> copy assignment must be noexcept for snapshot phase 3"
);

static_assert(
    std::is_nothrow_move_constructible_v<RenderNode>,
    "RenderNode move construction must be noexcept for snapshot phase 3"
);

static_assert(
    std::is_nothrow_copy_constructible_v<ReleaseInfo>,
    "ReleaseInfo copy construction must be noexcept for snapshot phase 3"
);

static_assert(
    std::is_nothrow_move_constructible_v<ActiveLease>,
    "ActiveLease move construction must be noexcept for snapshot phase 2"
);

class SurfaceManager {
public:
    core::Result<void> create_surface(uint32_t session_id, uint32_t local_id);
    
    core::Result<DestroyClientResult> destroy_client_surfaces(uint32_t session_id);
    core::Result<DestroySurfaceResult> destroy_surface(uint32_t session_id, uint32_t local_id);

    core::Result<void> attach_buffer(uint32_t session_id, uint32_t local_id, 
                                     uint32_t buffer_id, std::shared_ptr<yarilo::shm::MappedBuffer> mapping);

    core::Result<CommitResult> commit_surface(uint32_t session_id, uint32_t local_id, 
                                              uint32_t buffer_id, uint64_t commit_seq);

    void mark_map_request_sent(uint64_t global_id, uint64_t server_uid) noexcept;
    std::vector<MapRequestInfo> get_pending_map_requests() const;
    void handle_wm_disconnected() noexcept;

    core::Result<void> apply_wm_configuration(uint64_t global_id, uint64_t server_uid, int32_t x, int32_t y, 
                                              uint32_t z, bool mapped);

    // Strong Exception Guarantee: атомарная фиксация снимка
    core::Result<SnapshotResult> take_render_snapshot();

    std::optional<ReleaseInfo> release_read_lease(uint64_t lease_id) noexcept;

    [[nodiscard]]
    std::optional<platform::input::HitTestResult>
    hit_test(double cursor_x, double cursor_y) const noexcept;

    [[nodiscard]]
    std::optional<std::pair<int32_t, int32_t>>
    get_surface_origin(uint64_t gid) const noexcept;

    [[nodiscard]]
    bool surface_exists(uint64_t gid) const noexcept;

    [[nodiscard]]
    bool surface_is_mapped(uint64_t gid) const noexcept;

private:
    core::Result<void> validate_surface_graph_invariants(const Surface& s) const;
    std::optional<ReleaseInfo> try_release_binding(uint64_t binding_uid) noexcept;

    // Диагностическая проверка согласованности реестра аренд (для тестов и отладки)
    core::Result<void> validate_lease_registry() const;

    std::unordered_map<uint64_t, Surface> surfaces_;
    std::unordered_map<uint64_t, BufferBinding> bindings_;
    std::unordered_map<uint64_t, ActiveLease> active_leases_;
    
    uint64_t global_creation_counter_{0};
    uint64_t server_uid_counter_{0};
    uint64_t global_binding_counter_{0};
    uint64_t global_lease_counter_{0};
};

} // namespace yarilo::core