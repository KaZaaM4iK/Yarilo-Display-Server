#include "core/surface.hpp"
#include "shm/validator.hpp"

#include <sys/mman.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace yarilo::core {

class SurfaceManagerTestAccess {
public:
    static std::size_t surfaces_count(const SurfaceManager& sm) noexcept {
        return sm.surfaces_.size();
    }

    static std::size_t bindings_count(const SurfaceManager& sm) noexcept {
        return sm.bindings_.size();
    }

    static std::size_t lease_entries_count(const SurfaceManager& sm) noexcept {
        return sm.active_leases_.size();
    }

    static const BufferBinding* find_binding(
        const SurfaceManager& sm,
        uint32_t session_id,
        uint32_t local_surface_id,
        uint32_t buffer_id) noexcept
    {
        for (const auto& [uid, binding] : sm.bindings_) {
            if (binding.session_id == session_id &&
                binding.local_surface_id == local_surface_id &&
                binding.buffer_id == buffer_id) {
                return &binding;
            }
        }
        return nullptr;
    }

    static core::Result<void> validate_lease_registry(
        const SurfaceManager& sm)
    {
        return sm.validate_lease_registry();
    }
};

} // namespace yarilo::core

namespace {

using yarilo::core::BufferUsage;
using yarilo::core::SurfaceManager;
using yarilo::core::SurfaceManagerTestAccess;
using yarilo::shm::MappedBuffer;

#define CHECK(condition)                                                   \
    do {                                                                   \
        if (!(condition)) {                                                \
            std::cerr << "[FAIL] " << __FILE__ << ":" << __LINE__          \
                      << ": " #condition "\n";                              \
            return false;                                                  \
        }                                                                  \
    } while (false)

std::shared_ptr<MappedBuffer> create_test_buffer(
    uint32_t width,
    uint32_t height)
{
    if (width == 0 || height == 0 ||
        width > std::numeric_limits<uint32_t>::max() / 4) {
        throw std::runtime_error("Invalid test buffer dimensions");
    }

    const uint32_t stride = width * 4;

    if (static_cast<std::size_t>(height) >
        std::numeric_limits<std::size_t>::max() /
            static_cast<std::size_t>(stride)) {
        throw std::runtime_error("Test buffer size overflow");
    }

    const std::size_t size =
        static_cast<std::size_t>(stride) * height;

    void* addr = ::mmap(
        nullptr,
        size,
        PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0);

    if (addr == MAP_FAILED) {
        throw std::runtime_error(
            std::string("mmap failed: ") + std::strerror(errno));
    }

    try {
        return std::make_shared<MappedBuffer>(
            addr,
            size,
            static_cast<const uint32_t*>(addr),
            width,
            height,
            stride);
    } catch (...) {
        ::munmap(addr, size);
        throw;
    }
}

bool test_client_crash_lease_drain()
{
    std::cout << "[Test] Client Crash / Lease Drain\n";

    SurfaceManager sm;

    constexpr uint32_t SESSION_ID = 42;
    constexpr uint32_t SURFACE_ID = 1;
    constexpr uint32_t BUFFER_ID = 10;
    constexpr uint64_t COMMIT_SEQ = 100;
    constexpr uint64_t GLOBAL_ID =
        (static_cast<uint64_t>(SESSION_ID) << 32) | SURFACE_ID;

    constexpr uint32_t WIDTH = 640;
    constexpr uint32_t HEIGHT = 480;

    auto mapping = create_test_buffer(WIDTH, HEIGHT);
    std::weak_ptr<MappedBuffer> weak_mapping = mapping;

    CHECK(!weak_mapping.expired());

    // 1. Создание поверхности и фиксация первого коммита.
    auto create_res = sm.create_surface(SESSION_ID, SURFACE_ID);
    CHECK(create_res.has_value());

    auto attach_res = sm.attach_buffer(
        SESSION_ID, SURFACE_ID, BUFFER_ID, mapping);
    CHECK(attach_res.has_value());

    auto commit_res = sm.commit_surface(
        SESSION_ID, SURFACE_ID, BUFFER_ID, COMMIT_SEQ);
    CHECK(commit_res.has_value());
    CHECK(commit_res->map_request.has_value());

    const uint64_t server_uid =
        commit_res->map_request->server_uid;

    // 2. Эмуляция подтверждения Window Manager.
    sm.mark_map_request_sent(GLOBAL_ID, server_uid);

    auto wm_res = sm.apply_wm_configuration(
        GLOBAL_ID, server_uid, 0, 0, 0, true);
    CHECK(wm_res.has_value());

    // 3. Композитор получает снимок и регистрирует аренду.
    auto snapshot_res = sm.take_render_snapshot();
    CHECK(snapshot_res.has_value());
    CHECK(snapshot_res->nodes.size() == 1);

    const uint64_t lease_id =
        snapshot_res->nodes.front().lease_id;

    CHECK(SurfaceManagerTestAccess::lease_entries_count(sm) == 1);
    CHECK(SurfaceManagerTestAccess::bindings_count(sm) == 1);
    CHECK(SurfaceManagerTestAccess::surfaces_count(sm) == 1);

    const auto* binding = SurfaceManagerTestAccess::find_binding(
        sm, SESSION_ID, SURFACE_ID, BUFFER_ID);

    CHECK(binding != nullptr);
    CHECK(binding->usage == BufferUsage::Active);
    CHECK(binding->active_leases_count == 1);
    CHECK(SurfaceManagerTestAccess::validate_lease_registry(sm).has_value());

    // После reset отображение должно удерживаться конвейером,
    // а не локальной переменной теста.
    mapping.reset();
    CHECK(!weak_mapping.expired());

    // 4. Моделируем аварийное отключение клиента.
    std::cout << "  -> Destroying client surfaces\n";

    auto destroy_res = sm.destroy_client_surfaces(SESSION_ID);
    CHECK(destroy_res.has_value());

    CHECK(!sm.surface_exists(GLOBAL_ID));
    CHECK(SurfaceManagerTestAccess::surfaces_count(sm) == 0);

    // Не должно быть преждевременного освобождения буфера.
    CHECK(destroy_res->releases.empty());
    CHECK(SurfaceManagerTestAccess::bindings_count(sm) == 1);
    CHECK(SurfaceManagerTestAccess::lease_entries_count(sm) == 1);
    CHECK(!weak_mapping.expired());

    binding = SurfaceManagerTestAccess::find_binding(
        sm, SESSION_ID, SURFACE_ID, BUFFER_ID);

    CHECK(binding != nullptr);
    CHECK(binding->usage == BufferUsage::None);
    CHECK(binding->active_leases_count == 1);
    CHECK(SurfaceManagerTestAccess::validate_lease_registry(sm).has_value());

    // 5. Композитор завершает чтение и снимает аренду.
    std::cout << "  -> Releasing compositor lease\n";

    auto release_res = sm.release_read_lease(lease_id);
    CHECK(release_res.has_value());

    CHECK(release_res->session_id == SESSION_ID);
    CHECK(release_res->local_surface_id == SURFACE_ID);
    CHECK(release_res->buffer_id == BUFFER_ID);
    CHECK(release_res->commit_seq == COMMIT_SEQ);

    // После последней аренды привязка должна быть удалена.
    CHECK(SurfaceManagerTestAccess::lease_entries_count(sm) == 0);
    CHECK(SurfaceManagerTestAccess::bindings_count(sm) == 0);
    CHECK(SurfaceManagerTestAccess::validate_lease_registry(sm).has_value());

    // RenderNode всё ещё содержит shared_ptr<MappedBuffer>.
    CHECK(!weak_mapping.expired());

    // 6. Сбрасываем последнюю ссылку конвейера.
    snapshot_res->nodes.clear();

    // MappedBuffer должен быть уничтожен: его деструктор вызывает reset().
    CHECK(weak_mapping.expired());

    std::cout << "[PASS] Client Crash / Lease Drain\n";
    return true;
}

} // namespace

bool test_wm_recovery()
{
    std::cout << "[Test] WM Disconnect & Recovery State Machine\n";

    SurfaceManager sm;

    constexpr uint32_t SESS1 = 10;
    constexpr uint32_t SESS2 = 20;
    constexpr uint32_t SURF_ID = 1;
    constexpr uint64_t GID1 = (static_cast<uint64_t>(SESS1) << 32) | SURF_ID;
    constexpr uint64_t GID2 = (static_cast<uint64_t>(SESS2) << 32) | SURF_ID;

    auto buf1 = create_test_buffer(640, 480);
    auto buf2 = create_test_buffer(800, 600);

    // 1. Создание двух поверхностей и первичные коммиты
    CHECK(sm.create_surface(SESS1, SURF_ID).has_value());
    CHECK(sm.attach_buffer(SESS1, SURF_ID, 1, buf1).has_value());
    auto com1 = sm.commit_surface(SESS1, SURF_ID, 1, 100);
    CHECK(com1.has_value() && com1->map_request.has_value());

    CHECK(sm.create_surface(SESS2, SURF_ID).has_value());
    CHECK(sm.attach_buffer(SESS2, SURF_ID, 1, buf2).has_value());
    auto com2 = sm.commit_surface(SESS2, SURF_ID, 1, 200);
    CHECK(com2.has_value() && com2->map_request.has_value());

    const uint64_t uid1 = com1->map_request->server_uid;
    const uint64_t uid2 = com2->map_request->server_uid;

    // 2. WM #1 маппит обе поверхности
    sm.mark_map_request_sent(GID1, uid1);
    sm.mark_map_request_sent(GID2, uid2);
    CHECK(sm.apply_wm_configuration(GID1, uid1, 0, 0, 1, true).has_value());
    CHECK(sm.apply_wm_configuration(GID2, uid2, 640, 0, 2, true).has_value());

    CHECK(sm.surface_is_mapped(GID1));
    CHECK(sm.surface_is_mapped(GID2));
    CHECK(sm.get_pending_map_requests().empty());

    // Проверяем, что снимок захватывает 2 узла
    auto snap1 = sm.take_render_snapshot();
    CHECK(snap1.has_value() && snap1->nodes.size() == 2);
    snap1->nodes.clear();

    // 3. АВАРИЯ WM #1: handle_wm_disconnected
    std::cout << "  -> Simulating WM #1 crash (handle_wm_disconnected)...\n";
    sm.handle_wm_disconnected();

    // Инвариант 1: Поверхности не уничтожены, но сняты с экрана
    CHECK(sm.surface_exists(GID1));
    CHECK(sm.surface_exists(GID2));
    CHECK(!sm.surface_is_mapped(GID1));
    CHECK(!sm.surface_is_mapped(GID2));

    // Инвариант 2: Запросы маппинга перешли обратно в Queued
    auto pending = sm.get_pending_map_requests();
    CHECK(pending.size() == 2);

    bool found1 = false, found2 = false;
    for (const auto& req : pending) {
        if (req.global_id == GID1 && req.server_uid == uid1 && req.width == 640 && req.height == 480) found1 = true;
        if (req.global_id == GID2 && req.server_uid == uid2 && req.width == 800 && req.height == 600) found2 = true;
    }
    CHECK(found1 && found2);

    // Инвариант 3: Снимок во время отсутствия WM пуст (не рендерится в KMS)
    auto snap_unmapped = sm.take_render_snapshot();
    CHECK(snap_unmapped.has_value() && snap_unmapped->nodes.empty());

    // 4. ВОССТАНОВЛЕНИЕ: WM #2 подключается и конфигурирует слоты заново
    std::cout << "  -> Simulating WM #2 recovery and re-mapping...\n";
    for (const auto& req : pending) {
        sm.mark_map_request_sent(req.global_id, req.server_uid);
    }
    CHECK(sm.apply_wm_configuration(GID1, uid1, 100, 100, 1, true).has_value());
    CHECK(sm.apply_wm_configuration(GID2, uid2, 800, 100, 2, true).has_value());

    CHECK(sm.surface_is_mapped(GID1));
    CHECK(sm.surface_is_mapped(GID2));
    CHECK(sm.get_pending_map_requests().empty());

    // Инвариант 4: Композитор возобновил рендеринг обоих окон
    auto snap_recovered = sm.take_render_snapshot();
    CHECK(snap_recovered.has_value() && snap_recovered->nodes.size() == 2);
    snap_recovered->nodes.clear();

    std::cout << "  [PASS] WM Disconnect & Recovery State Machine verified.\n";
    return true;
}

int main()
{
    try {
        if (!test_client_crash_lease_drain()) {
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "[FAIL] Unhandled exception: " << e.what() << '\n';
        return 1;
    }

    if (!test_wm_recovery()) {
            return 1;
        }
        
    std::cout << "All SurfaceManager tests passed.\n";
    return 0;
}