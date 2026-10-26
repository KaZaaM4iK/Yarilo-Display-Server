#include "core/surface.hpp"
#include "core/error.hpp"
#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>
#include <cstdio>
#include <cinttypes>

namespace yarilo::core {

core::Result<void> SurfaceManager::validate_surface_graph_invariants(const Surface& s) const {
    uint64_t expected_gid = (static_cast<uint64_t>(s.owner_session_id) << 32) | s.local_id;
    if (s.global_id != expected_gid ||
        static_cast<uint32_t>(s.global_id >> 32) != s.owner_session_id ||
        static_cast<uint32_t>(s.global_id & 0xFFFFFFFFULL) != s.local_id) {
        return std::unexpected(core::make_error(core::ServerError::InternalStateError));
    }

    if (s.pending_commit.has_value() && s.active_commit.has_value()) {
        if (s.pending_commit->binding_uid == s.active_commit->binding_uid ||
            s.pending_commit->commit_seq <= s.active_commit->commit_seq) {
            return std::unexpected(core::make_error(core::ServerError::InternalStateError));
        }
    }

    if (s.pending_commit.has_value()) {
        if (s.pending_commit->commit_seq != s.last_commit_seq) {
            return std::unexpected(core::make_error(core::ServerError::InternalStateError));
        }
        auto link_it = s.attached_bindings.find(s.pending_commit->buffer_id);
        if (link_it == s.attached_bindings.end() || link_it->second != s.pending_commit->binding_uid) {
            return std::unexpected(core::make_error(core::ServerError::InternalStateError));
        }
        auto b_it = bindings_.find(s.pending_commit->binding_uid);
        if (b_it == bindings_.end() ||
            b_it->second.session_id != s.owner_session_id ||
            b_it->second.local_surface_id != s.local_id ||
            b_it->second.buffer_id != s.pending_commit->buffer_id ||
            b_it->second.usage != BufferUsage::Pending ||
            b_it->second.epoch_commit_seq != s.pending_commit->commit_seq) {
            return std::unexpected(core::make_error(core::ServerError::InternalStateError));
        }
        if (b_it->second.release_state != BindingReleaseState::Retained) {
            return std::unexpected(core::make_error(core::ServerError::InternalStateError));
        }
    }

    if (s.active_commit.has_value()) {
        if (s.active_commit->commit_seq > s.last_commit_seq) {
            return std::unexpected(core::make_error(core::ServerError::InternalStateError));
        }
        auto link_it = s.attached_bindings.find(s.active_commit->buffer_id);
        if (link_it == s.attached_bindings.end() || link_it->second != s.active_commit->binding_uid) {
            return std::unexpected(core::make_error(core::ServerError::InternalStateError));
        }
        auto b_it = bindings_.find(s.active_commit->binding_uid);
        if (b_it == bindings_.end() ||
            b_it->second.session_id != s.owner_session_id ||
            b_it->second.local_surface_id != s.local_id ||
            b_it->second.buffer_id != s.active_commit->buffer_id ||
            b_it->second.usage != BufferUsage::Active ||
            b_it->second.epoch_commit_seq != s.active_commit->commit_seq) {
            return std::unexpected(core::make_error(core::ServerError::InternalStateError));
        }
        if (b_it->second.release_state != BindingReleaseState::Retained) {
            return std::unexpected(core::make_error(core::ServerError::InternalStateError));
        }
    }

    for (const auto& [buf_id, b_uid] : s.attached_bindings) {
        auto b_it = bindings_.find(b_uid);
        if (b_it == bindings_.end() ||
            b_it->second.binding_uid != b_uid ||
            b_it->second.session_id != s.owner_session_id ||
            b_it->second.local_surface_id != s.local_id ||
            b_it->second.buffer_id != buf_id) {
            return std::unexpected(core::make_error(core::ServerError::InternalStateError));
        }

        const auto& b = b_it->second;
        if (b.usage == BufferUsage::Pending) {
            if (!s.pending_commit.has_value() || s.pending_commit->binding_uid != b_uid ||
                s.pending_commit->commit_seq != b.epoch_commit_seq) {
                return std::unexpected(core::make_error(core::ServerError::InternalStateError));
            }
            if (b.release_state != BindingReleaseState::Retained) {
                return std::unexpected(core::make_error(core::ServerError::InternalStateError));
            }
        } else if (b.usage == BufferUsage::Active) {
            if (!s.active_commit.has_value() || s.active_commit->binding_uid != b_uid ||
                s.active_commit->commit_seq != b.epoch_commit_seq) {
                return std::unexpected(core::make_error(core::ServerError::InternalStateError));
            }
            if (b.release_state != BindingReleaseState::Retained) {
                return std::unexpected(core::make_error(core::ServerError::InternalStateError));
            }
        } else if (b.usage == BufferUsage::None) {
            if ((s.pending_commit.has_value() && s.pending_commit->binding_uid == b_uid) ||
                (s.active_commit.has_value() && s.active_commit->binding_uid == b_uid)) {
                return std::unexpected(core::make_error(core::ServerError::InternalStateError));
            }
            if (b.release_state == BindingReleaseState::Released && b.active_leases_count != 0) {
                return std::unexpected(core::make_error(core::ServerError::InternalStateError));
            }
            if (b.epoch_commit_seq == 0 && b.release_state != BindingReleaseState::Retained) {
                return std::unexpected(core::make_error(core::ServerError::InternalStateError));
            }
        } else {
            return std::unexpected(core::make_error(core::ServerError::InternalStateError));
        }
    }
    return {};
}

core::Result<void> SurfaceManager::create_surface(uint32_t session_id, uint32_t local_id) {
    if (local_id == 0) return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));
    if (server_uid_counter_ == std::numeric_limits<uint64_t>::max() ||
        global_creation_counter_ == std::numeric_limits<uint64_t>::max()) {
        return std::unexpected(core::make_error(core::ServerError::OutOfMemory));
    }
    
    uint64_t global_id = (static_cast<uint64_t>(session_id) << 32) | local_id;
    if (surfaces_.contains(global_id)) {
        return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));
    }

    uint64_t next_uid = server_uid_counter_ + 1;
    uint64_t next_order = global_creation_counter_ + 1;

    try {
        auto [it, inserted] = surfaces_.try_emplace(global_id, Surface{
            .server_uid = next_uid,
            .global_id = global_id,
            .local_id = local_id,
            .owner_session_id = session_id,
            .creation_order = next_order
        });
        if (!inserted) {
            return std::unexpected(core::make_error(core::ServerError::InternalStateError));
        }
    } catch (const std::bad_alloc&) {
        return std::unexpected(core::make_error(core::ServerError::OutOfMemory));
    } catch (const std::length_error&) {
        return std::unexpected(core::make_error(core::ServerError::OutOfMemory));
    }

    server_uid_counter_ = next_uid;
    global_creation_counter_ = next_order;
    return {};
}

core::Result<DestroyClientResult> SurfaceManager::destroy_client_surfaces(uint32_t session_id) {
    struct StagedSurfaceDestruction {
        uint64_t global_id;
        bool wm_notified;
        std::vector<BufferBinding*> bindings;
    };

    std::vector<StagedSurfaceDestruction> staged;
    DestroyClientResult result;
    size_t total_releases = 0;
    size_t total_wm_notified = 0;

    try {
        for (const auto& [gid, s] : surfaces_) {
            bool belongs_by_key   = (static_cast<uint32_t>(gid >> 32) == session_id);
            bool belongs_by_field = (s.owner_session_id == session_id);

            if (belongs_by_key || belongs_by_field) {
                if (!belongs_by_key || !belongs_by_field || s.global_id != gid) {
                    return std::unexpected(core::make_error(core::ServerError::InternalStateError));
                }

                if (auto val_res = validate_surface_graph_invariants(s); !val_res) {
                    return std::unexpected(val_res.error());
                }

                StagedSurfaceDestruction stage;
                stage.global_id = gid;
                stage.wm_notified = (s.map_state == MapDeliveryState::Sent);
                stage.bindings.reserve(s.attached_bindings.size());

                for (const auto& [buf_id, b_uid] : s.attached_bindings) {
                    auto b_it = bindings_.find(b_uid);
                    if (b_it == bindings_.end()) {
                        return std::unexpected(core::make_error(core::ServerError::InternalStateError));
                    }
                    stage.bindings.push_back(&b_it->second);
                }

                if (stage.wm_notified) total_wm_notified++;

                if (std::numeric_limits<size_t>::max() - total_releases < s.attached_bindings.size()) {
                    return std::unexpected(core::make_error(core::ServerError::OutOfMemory));
                }
                total_releases += s.attached_bindings.size();

                staged.push_back(std::move(stage));
            }
        }

        result.wm_surfaces_to_destroy.reserve(total_wm_notified);
        result.releases.reserve(total_releases);

        for (const auto& [b_uid, b] : bindings_) {
            if (b.session_id == session_id) {
                uint64_t expected_gid = (static_cast<uint64_t>(session_id) << 32) | b.local_surface_id;
                auto surf_it = surfaces_.find(expected_gid);
                if (surf_it == surfaces_.end()) {
                    if (b.active_leases_count > 0 && b.usage == BufferUsage::None &&
                        b.release_state == BindingReleaseState::Retained) {
                        continue;
                    }
                    return std::unexpected(core::make_error(core::ServerError::InternalStateError));
                }
                const auto& s = surf_it->second;
                if (s.global_id != expected_gid ||
                    s.owner_session_id != session_id ||
                    s.local_id != b.local_surface_id) {
                    return std::unexpected(core::make_error(core::ServerError::InternalStateError));
                }
                auto link_it = s.attached_bindings.find(b.buffer_id);
                if (link_it == s.attached_bindings.end() || link_it->second != b_uid) {
                    return std::unexpected(core::make_error(core::ServerError::InternalStateError));
                }
            }
        }
    } catch (const std::bad_alloc&) {
        return std::unexpected(core::make_error(core::ServerError::OutOfMemory));
    } catch (const std::length_error&) {
        return std::unexpected(core::make_error(core::ServerError::OutOfMemory));
    }

    for (auto& stage : staged) {
        if (stage.wm_notified) {
            result.wm_surfaces_to_destroy.push_back(stage.global_id);
        }

        surfaces_.erase(stage.global_id);

        for (BufferBinding* binding : stage.bindings) {
            binding->usage = BufferUsage::None;
            auto rel = try_release_binding(binding->binding_uid);
            if (rel) {
                result.releases.push_back(*rel);
            }
        }
    }
    return result;
}

core::Result<DestroySurfaceResult> SurfaceManager::destroy_surface(uint32_t session_id, uint32_t local_id) {
    uint64_t global_id = (static_cast<uint64_t>(session_id) << 32) | local_id;
    auto it = surfaces_.find(global_id);
    if (it == surfaces_.end()) {
        return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));
    }

    Surface& s = it->second;

    if (auto val_res = validate_surface_graph_invariants(s); !val_res) {
        return std::unexpected(val_res.error());
    }

    for (const auto& [b_uid, b] : bindings_) {
        if (b.session_id == session_id && b.local_surface_id == local_id) {
            auto link_it = s.attached_bindings.find(b.buffer_id);
            if (link_it == s.attached_bindings.end() || link_it->second != b_uid) {
                return std::unexpected(core::make_error(core::ServerError::InternalStateError));
            }
        }
    }

    DestroySurfaceResult result;
    std::vector<BufferBinding*> target_bindings;

    try {
        result.releases.reserve(s.attached_bindings.size());
        result.server_uid = s.server_uid; // Защита от ABA
        target_bindings.reserve(s.attached_bindings.size());
        
        for (const auto& [buf_id, b_uid] : s.attached_bindings) {
            auto b_it = bindings_.find(b_uid);
            if (b_it == bindings_.end() ||
                b_it->second.binding_uid != b_uid ||
                b_it->second.session_id != session_id ||
                b_it->second.local_surface_id != local_id ||
                b_it->second.buffer_id != buf_id) {
                return std::unexpected(core::make_error(core::ServerError::InternalStateError));
            }
            target_bindings.push_back(&b_it->second);
        }
    } catch (const std::bad_alloc&) {
        return std::unexpected(core::make_error(core::ServerError::OutOfMemory));
    } catch (const std::length_error&) {
        return std::unexpected(core::make_error(core::ServerError::OutOfMemory));
    }

    result.wm_notified = (s.map_state == MapDeliveryState::Sent);
    surfaces_.erase(it);

    for (BufferBinding* binding : target_bindings) {
        binding->usage = BufferUsage::None;
        auto rel = try_release_binding(binding->binding_uid);
        if (rel) {
            result.releases.push_back(*rel);
        }
    }
    return result;
}

core::Result<void> SurfaceManager::attach_buffer(uint32_t session_id, uint32_t local_id, 
                                                 uint32_t buffer_id, std::shared_ptr<yarilo::shm::MappedBuffer> mapping) 
{
    if (!mapping || !mapping->is_valid()) {
        return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));
    }
    if (global_binding_counter_ == std::numeric_limits<uint64_t>::max()) {
        return std::unexpected(core::make_error(core::ServerError::OutOfMemory));
    }

    uint64_t global_id = (static_cast<uint64_t>(session_id) << 32) | local_id;
    auto it = surfaces_.find(global_id);
    if (it == surfaces_.end()) return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));

    Surface& s = it->second;

    auto existing_it = s.attached_bindings.find(buffer_id);
    if (existing_it != s.attached_bindings.end()) {
        auto b_it = bindings_.find(existing_it->second);
        if (b_it == bindings_.end() ||
            b_it->second.session_id != session_id ||
            b_it->second.local_surface_id != local_id ||
            b_it->second.buffer_id != buffer_id) {
            return std::unexpected(core::make_error(core::ServerError::InternalStateError));
        }
        if (b_it->second.usage != BufferUsage::None || b_it->second.active_leases_count > 0) {
            return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));
        }
    }

    uint64_t new_b_uid = global_binding_counter_ + 1;
    BufferBinding new_binding{
        .binding_uid = new_b_uid,
        .mapping = std::move(mapping),
        .usage = BufferUsage::None,
        .release_state = BindingReleaseState::Retained,
        .active_leases_count = 0,
        .session_id = session_id,
        .local_surface_id = local_id,
        .buffer_id = buffer_id,
        .epoch_commit_seq = 0
    };

    try {
        auto [inserted_it, inserted] = bindings_.emplace(new_b_uid, std::move(new_binding));
        if (!inserted) {
            return std::unexpected(core::make_error(core::ServerError::InternalStateError));
        }

        try {
            if (existing_it != s.attached_bindings.end()) {
                uint64_t old_b_uid = existing_it->second;
                existing_it->second = new_b_uid;
                bindings_.erase(old_b_uid);
            } else {
                auto [att_it, att_inserted] = s.attached_bindings.emplace(buffer_id, new_b_uid);
                if (!att_inserted) {
                    bindings_.erase(inserted_it);
                    return std::unexpected(core::make_error(core::ServerError::InternalStateError));
                }
            }
        } catch (...) {
            bindings_.erase(inserted_it);
            throw;
        }
    } catch (const std::bad_alloc&) {
        return std::unexpected(core::make_error(core::ServerError::OutOfMemory));
    } catch (const std::length_error&) {
        return std::unexpected(core::make_error(core::ServerError::OutOfMemory));
    }

    global_binding_counter_ = new_b_uid;
    return {};
}

std::optional<ReleaseInfo> SurfaceManager::try_release_binding(uint64_t binding_uid) noexcept {
    auto it = bindings_.find(binding_uid);
    if (it == bindings_.end()) return std::nullopt;

    auto& b = it->second;

    if (b.usage == BufferUsage::None && b.active_leases_count == 0 && 
        b.release_state == BindingReleaseState::Retained) 
    {
        b.release_state = BindingReleaseState::Released;
        
        std::optional<ReleaseInfo> result = std::nullopt;
        if (b.epoch_commit_seq > 0) {
            result = ReleaseInfo{
                .session_id = b.session_id,
                .local_surface_id = b.local_surface_id,
                .buffer_id = b.buffer_id,
                .commit_seq = b.epoch_commit_seq
            };
        }

        uint64_t gid = (static_cast<uint64_t>(b.session_id) << 32) | b.local_surface_id;
        if (!surfaces_.contains(gid)) {
            bindings_.erase(it);
        }
        return result;
    }

    if (b.active_leases_count == 0 && b.release_state == BindingReleaseState::Released) {
        uint64_t gid = (static_cast<uint64_t>(b.session_id) << 32) | b.local_surface_id;
        if (!surfaces_.contains(gid)) {
            bindings_.erase(it);
        }
    }
    return std::nullopt;
}

core::Result<CommitResult> SurfaceManager::commit_surface(uint32_t session_id, uint32_t local_id, 
                                                          uint32_t buffer_id, uint64_t commit_seq) 
{
    uint64_t global_id = (static_cast<uint64_t>(session_id) << 32) | local_id;
    auto it = surfaces_.find(global_id);
    if (it == surfaces_.end()) return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));

    Surface& s = it->second;

    if (commit_seq <= s.last_commit_seq) {
        return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));
    }

    auto b_link = s.attached_bindings.find(buffer_id);
    if (b_link == s.attached_bindings.end()) {
        return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));
    }

    auto b_it = bindings_.find(b_link->second);
    if (b_it == bindings_.end() ||
        b_it->second.session_id != session_id ||
        b_it->second.local_surface_id != local_id ||
        b_it->second.buffer_id != buffer_id) {
        return std::unexpected(core::make_error(core::ServerError::InternalStateError));
    }

    bool is_first_commit = (b_it->second.epoch_commit_seq == 0);
    bool is_prior_epoch_released = (b_it->second.release_state == BindingReleaseState::Released);
    if (!is_first_commit && !is_prior_epoch_released) {
        return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));
    }

    if (b_it->second.usage != BufferUsage::None || b_it->second.active_leases_count > 0) {
        return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));
    }

    if (s.pending_commit.has_value()) {
        auto link_it = s.attached_bindings.find(s.pending_commit->buffer_id);
        if (link_it == s.attached_bindings.end() || link_it->second != s.pending_commit->binding_uid) {
            return std::unexpected(core::make_error(core::ServerError::InternalStateError));
        }

        auto old_b_it = bindings_.find(s.pending_commit->binding_uid);
        if (old_b_it == bindings_.end() ||
            old_b_it->second.session_id != session_id ||
            old_b_it->second.local_surface_id != local_id ||
            old_b_it->second.buffer_id != s.pending_commit->buffer_id ||
            old_b_it->second.usage != BufferUsage::Pending ||
            old_b_it->second.epoch_commit_seq != s.pending_commit->commit_seq) {
            return std::unexpected(core::make_error(core::ServerError::InternalStateError));
        }
    }

    uint32_t incoming_w = b_it->second.mapping->width();
    uint32_t incoming_h = b_it->second.mapping->height();

    if (s.map_state != MapDeliveryState::None) {
        if (incoming_w != s.fixed_width || incoming_h != s.fixed_height) {
            return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));
        }
    }

    CommitResult result;
    try {
        result.superseded_releases.reserve(1);
    } catch (const std::bad_alloc&) {
        return std::unexpected(core::make_error(core::ServerError::OutOfMemory));
    } catch (const std::length_error&) {
        return std::unexpected(core::make_error(core::ServerError::OutOfMemory));
    }

    if (s.map_state == MapDeliveryState::None) {
        s.fixed_width = incoming_w;
        s.fixed_height = incoming_h;
    }

    if (s.pending_commit.has_value()) {
        auto old_b_it = bindings_.find(s.pending_commit->binding_uid);
        old_b_it->second.usage = BufferUsage::None;
        auto rel = try_release_binding(old_b_it->second.binding_uid);
        if (rel) {
            result.superseded_releases.push_back(*rel);
        }
    }

    s.last_commit_seq = commit_seq;
    b_it->second.usage = BufferUsage::Pending;
    b_it->second.release_state = BindingReleaseState::Retained;
    b_it->second.epoch_commit_seq = commit_seq;

    s.pending_commit = QueuedCommit{
        .binding_uid = b_it->second.binding_uid,
        .buffer_id = buffer_id,
        .commit_seq = commit_seq,
        .width = incoming_w,
        .height = incoming_h
    };

    if (s.map_state == MapDeliveryState::None) {
        s.map_state = MapDeliveryState::Queued;
        result.map_request = MapRequestInfo{
            .global_id = s.global_id,
            .server_uid = s.server_uid,
            .width = s.pending_commit->width,
            .height = s.pending_commit->height
        };
    }

    return result;
}

void SurfaceManager::mark_map_request_sent(uint64_t global_id, uint64_t server_uid) noexcept {
    auto it = surfaces_.find(global_id);
    if (it != surfaces_.end() && it->second.server_uid == server_uid && it->second.map_state == MapDeliveryState::Queued) {
        it->second.map_state = MapDeliveryState::Sent;
    }
}

core::Result<void> SurfaceManager::apply_wm_configuration(uint64_t global_id, uint64_t server_uid, int32_t x, int32_t y, 
                                                          uint32_t z, bool mapped) 
{
    auto it = surfaces_.find(global_id);
    if (it == surfaces_.end() || it->second.server_uid != server_uid) {
        return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));
    }

    Surface& s = it->second;
    
    if (mapped && s.map_state != MapDeliveryState::Sent) {
        return std::unexpected(core::make_error(core::ServerError::ProtocolViolation));
    }

    s.x = x;
    s.y = y;
    s.z_index = z;
    s.is_mapped_by_wm = mapped;
    return {};
}

// ---------------------------------------------------------
// Sprint 5 Additions for Window Manager Late Joiner Support
// ---------------------------------------------------------

std::vector<MapRequestInfo> SurfaceManager::get_pending_map_requests() const {
    std::vector<MapRequestInfo> requests;
    for (const auto& [gid, s] : surfaces_) {
        if (s.map_state == MapDeliveryState::Queued && s.fixed_width > 0 && s.fixed_height > 0) {
            requests.push_back(MapRequestInfo{
                .global_id = s.global_id,
                .server_uid = s.server_uid,
                .width = s.fixed_width,
                .height = s.fixed_height
            });
        }
    }
    return requests;
}

void SurfaceManager::handle_wm_disconnected() noexcept {
    for (auto& [gid, s] : surfaces_) {
        s.is_mapped_by_wm = false;

        if (s.map_state == MapDeliveryState::Sent) {
            s.map_state = MapDeliveryState::Queued;
        }
    }
}

// ---------------------------------------------------------

core::Result<SnapshotResult> SurfaceManager::take_render_snapshot() {
    struct PendingTransition {
        Surface* surface;
        BufferBinding* old_binding;
        BufferBinding* new_binding;
        bool has_old_binding;
    };

    struct PreparedNode {
        uint64_t lease_id;
        RenderNode node;
        BufferBinding* binding;
    };

    std::vector<PendingTransition> staging_transitions;
    std::vector<PreparedNode> staging_nodes;
    SnapshotResult result;

    try {
        size_t surface_count = surfaces_.size();

        if (staging_transitions.max_size() < surface_count ||
            staging_nodes.max_size() < surface_count ||
            result.nodes.max_size() < surface_count ||
            result.newly_released.max_size() < surface_count) {
            return std::unexpected(core::make_error(core::ServerError::OutOfMemory));
        }

        staging_transitions.reserve(surface_count);
        staging_nodes.reserve(surface_count);
        result.nodes.reserve(surface_count);
        result.newly_released.reserve(surface_count);

        size_t needed_leases = 0;
        std::vector<uint64_t> old_binding_uids;
        old_binding_uids.reserve(surface_count);

        for (auto& [gid, s] : surfaces_) {
            if (s.global_id != gid) {
                return std::unexpected(core::make_error(core::ServerError::InternalStateError));
            }

            if (auto val_res = validate_surface_graph_invariants(s); !val_res) {
                return std::unexpected(val_res.error());
            }

            bool will_render = s.is_mapped_by_wm && (s.pending_commit.has_value() || s.active_commit.has_value());
            if (will_render) {
                const auto& target_commit = s.pending_commit ? *s.pending_commit : *s.active_commit;
                uint64_t b_uid = target_commit.binding_uid;
                auto b_it = bindings_.find(b_uid);

                if (b_it == bindings_.end()) {
                    return std::unexpected(core::make_error(core::ServerError::InternalStateError));
                }

                if (!b_it->second.mapping || !b_it->second.mapping->is_valid() ||
                    b_it->second.mapping->width() != target_commit.width ||
                    b_it->second.mapping->height() != target_commit.height) {
                    return std::unexpected(core::make_error(core::ServerError::InternalStateError));
                }

                if (b_it->second.active_leases_count == std::numeric_limits<uint32_t>::max()) {
                    return std::unexpected(core::make_error(core::ServerError::OutOfMemory));
                }
                if (needed_leases == std::numeric_limits<size_t>::max()) {
                    return std::unexpected(core::make_error(core::ServerError::OutOfMemory));
                }
                needed_leases++;
            }
        }

        if (needed_leases > 0) {
            if (std::numeric_limits<uint64_t>::max() - global_lease_counter_ < static_cast<uint64_t>(needed_leases)) {
                return std::unexpected(core::make_error(core::ServerError::OutOfMemory));
            }
            if (needed_leases > active_leases_.max_size() - active_leases_.size()) {
                return std::unexpected(core::make_error(core::ServerError::OutOfMemory));
            }
            active_leases_.reserve(active_leases_.size() + needed_leases);
        }

        uint64_t temp_lease_counter = global_lease_counter_;

        for (auto& [gid, s] : surfaces_) {
            if (s.pending_commit.has_value()) {
                BufferBinding* old_binding = nullptr;
                if (s.active_commit.has_value()) {
                    auto old_b_it = bindings_.find(s.active_commit->binding_uid);
                    if (old_b_it == bindings_.end()) {
                        return std::unexpected(core::make_error(core::ServerError::InternalStateError));
                    }
                    if (old_b_it->second.session_id != s.owner_session_id ||
                        old_b_it->second.local_surface_id != s.local_id ||
                        old_b_it->second.buffer_id != s.active_commit->buffer_id ||
                        old_b_it->second.usage != BufferUsage::Active ||
                        old_b_it->second.epoch_commit_seq != s.active_commit->commit_seq) {
                        return std::unexpected(core::make_error(core::ServerError::InternalStateError));
                    }
                    old_binding = &old_b_it->second;
                    old_binding_uids.push_back(s.active_commit->binding_uid);
                }

                auto new_b_it = bindings_.find(s.pending_commit->binding_uid);
                if (new_b_it == bindings_.end()) {
                    return std::unexpected(core::make_error(core::ServerError::InternalStateError));
                }
                if (new_b_it->second.session_id != s.owner_session_id ||
                    new_b_it->second.local_surface_id != s.local_id ||
                    new_b_it->second.buffer_id != s.pending_commit->buffer_id ||
                    new_b_it->second.usage != BufferUsage::Pending ||
                    new_b_it->second.epoch_commit_seq != s.pending_commit->commit_seq) {
                    return std::unexpected(core::make_error(core::ServerError::InternalStateError));
                }

                staging_transitions.push_back(PendingTransition{
                    .surface = &s,
                    .old_binding = old_binding,
                    .new_binding = &new_b_it->second,
                    .has_old_binding = s.active_commit.has_value()
                });
            }

            bool will_render = s.is_mapped_by_wm && (s.pending_commit.has_value() || s.active_commit.has_value());
            if (will_render) {
                const auto& target_commit = s.pending_commit ? *s.pending_commit : *s.active_commit;
                auto b_it = bindings_.find(target_commit.binding_uid);
                if (b_it == bindings_.end()) {
                    return std::unexpected(core::make_error(core::ServerError::InternalStateError));
                }

                uint64_t lease_id = ++temp_lease_counter;
                staging_nodes.push_back(PreparedNode{
                    .lease_id = lease_id,
                    .node = RenderNode{
                        .lease_id = lease_id,
                        .global_id = s.global_id,
                        .dst_x = s.x,
                        .dst_y = s.y,
                        .src_width = target_commit.width,
                        .src_height = target_commit.height,
                        .src_stride = b_it->second.mapping->stride(),
                        .mapping_ref = b_it->second.mapping,
                        .z_index = s.z_index,
                        .creation_order = s.creation_order,
                        .session_id = s.owner_session_id,
                        .local_surface_id = s.local_id,
                        .buffer_id = target_commit.buffer_id,
                        .commit_seq = target_commit.commit_seq
                    },
                    .binding = &b_it->second
                });
            }
        }

        std::sort(old_binding_uids.begin(), old_binding_uids.end());
        if (std::adjacent_find(old_binding_uids.begin(), old_binding_uids.end()) != old_binding_uids.end()) {
            return std::unexpected(core::make_error(core::ServerError::InternalStateError));
        }

        std::stable_sort(staging_nodes.begin(), staging_nodes.end(),
            [](const PreparedNode& a, const PreparedNode& b) {
                if (a.node.z_index != b.node.z_index) return a.node.z_index < b.node.z_index;
                return a.node.creation_order < b.node.creation_order;
            });

    } catch (const std::bad_alloc&) {
        return std::unexpected(core::make_error(core::ServerError::OutOfMemory));
    } catch (const std::length_error&) {
        return std::unexpected(core::make_error(core::ServerError::OutOfMemory));
    }

    std::vector<uint64_t> inserted_lease_ids;
    try {
        inserted_lease_ids.reserve(staging_nodes.size());
        for (const auto& prep : staging_nodes) {
            auto [it, inserted] = active_leases_.emplace(prep.lease_id, ActiveLease{
                .lease_id = prep.lease_id,
                .binding_uid = prep.binding->binding_uid
            });
            if (!inserted) {
                for (uint64_t id : inserted_lease_ids) {
                    active_leases_.erase(id);
                }
                return std::unexpected(core::make_error(core::ServerError::InternalStateError));
            }
            inserted_lease_ids.push_back(prep.lease_id);
        }
    } catch (const std::bad_alloc&) {
        for (uint64_t id : inserted_lease_ids) {
            active_leases_.erase(id);
        }
        return std::unexpected(core::make_error(core::ServerError::OutOfMemory));
    } catch (const std::length_error&) {
        for (uint64_t id : inserted_lease_ids) {
            active_leases_.erase(id);
        }
        return std::unexpected(core::make_error(core::ServerError::OutOfMemory));
    } catch (...) {
        for (uint64_t id : inserted_lease_ids) {
            active_leases_.erase(id);
        }
        return std::unexpected(core::make_error(core::ServerError::InternalStateError));
    }

    for (auto& trans : staging_transitions) {
        Surface& s = *trans.surface;

        if (trans.has_old_binding && trans.old_binding != nullptr) {
            trans.old_binding->usage = BufferUsage::None;
            auto rel = try_release_binding(trans.old_binding->binding_uid);
            if (rel) {
                result.newly_released.push_back(*rel);
            }
        }

        s.active_commit = s.pending_commit;
        s.width = s.active_commit->width;
        s.height = s.active_commit->height;
        s.pending_commit.reset();

        if (trans.new_binding != nullptr) {
            trans.new_binding->usage = BufferUsage::Active;
        }
    }

    for (auto& prep : staging_nodes) {
        prep.binding->active_leases_count++;
        result.nodes.push_back(std::move(prep.node));
    }

    global_lease_counter_ += staging_nodes.size();
    return result;
}


std::optional<ReleaseInfo> SurfaceManager::release_read_lease(uint64_t lease_id) noexcept {
    auto lease_it = active_leases_.find(lease_id);
    if (lease_it == active_leases_.end()) {
        return std::nullopt;
    }

    uint64_t target_b_uid = lease_it->second.binding_uid;
    auto b_it = bindings_.find(target_b_uid);
    if (b_it == bindings_.end()) {
        std::fprintf(stderr, "Yarilo Critical: Missing binding %" PRIu64 " for lease %" PRIu64 "\n", target_b_uid, lease_id);
        return std::nullopt;
    }

    if (b_it->second.active_leases_count == 0) {
        std::fprintf(stderr, "Yarilo Critical: active_leases_count underflow on lease %" PRIu64 "\n", lease_id);
        return std::nullopt;
    }

    active_leases_.erase(lease_it);
    b_it->second.active_leases_count--;

    return try_release_binding(target_b_uid);
}

core::Result<void> SurfaceManager::validate_lease_registry() const {
    std::unordered_map<uint64_t, size_t> lease_count_per_binding;
    for (const auto& [lease_id, lease] : active_leases_) {
        auto b_it = bindings_.find(lease.binding_uid);
        if (b_it == bindings_.end()) {
            return std::unexpected(core::make_error(core::ServerError::InternalStateError));
        }
        lease_count_per_binding[lease.binding_uid]++;
    }

    for (const auto& [b_uid, b] : bindings_) {
        auto it = lease_count_per_binding.find(b_uid);
        size_t actual_count = (it == lease_count_per_binding.end()) ? 0 : it->second;

        if (actual_count > std::numeric_limits<uint32_t>::max()) {
            return std::unexpected(core::make_error(core::ServerError::InternalStateError));
        }

        if (b.active_leases_count != static_cast<uint32_t>(actual_count)) {
            return std::unexpected(core::make_error(core::ServerError::InternalStateError));
        }
    }

    return {};
}

} // namespace yarilo::core