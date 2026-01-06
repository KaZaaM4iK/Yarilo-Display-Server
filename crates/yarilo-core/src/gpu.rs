use anyhow::{Context, Result, anyhow};
use std::fs::{self, File};
use std::os::unix::io::{AsFd, AsRawFd};
use std::path::PathBuf;

use drm::Device as BasicDevice;
use drm::control::{Device as ControlDevice, connector};

/// Лёгкий wrapper вокруг File, чтобы можно было вызвать методы drm::control
#[derive(Debug)]
struct DrmProbe(File);

impl AsFd for DrmProbe {
    fn as_fd(&self) -> std::os::unix::io::BorrowedFd<'_> {
        self.0.as_fd()
    }
}
impl AsRawFd for DrmProbe {
    fn as_raw_fd(&self) -> std::os::unix::io::RawFd {
        self.0.as_raw_fd()
    }
}
impl BasicDevice for DrmProbe {}
impl ControlDevice for DrmProbe {}

/// Пробывает открыть и проверить cardX: возвращает true если это валидный DRM/KMS device
fn probe_card(path: &PathBuf) -> bool {
    // Попробуем открыть файл rw (нужны права)
    let f = match File::open(path) {
        Ok(f) => f,
        Err(_) => return false,
    };

    // Некоторым операциям нужен writable дескриптор (set mode), попытка clone с write
    let f_rw = match f.try_clone() {
        Ok(f2) => f2,
        Err(_) => return false,
    };

    let probe = DrmProbe(f_rw);

    // Попытка извлечь resource handles и найти подключённый коннектор с режимами
    if let Ok(handles) = probe.resource_handles() {
        for &conn in handles.connectors() {
            if let Ok(info) = probe.get_connector(conn, true) {
                if info.state() == connector::State::Connected && !info.modes().is_empty() {
                    return true;
                }
            }
        }
    }

    false
}

/// Читает vendor из sysfs для указанного /dev/dri/cardX
fn read_sys_vendor(card_name: &str) -> Option<String> {
    let sys_path = format!("/sys/class/drm/{}/device/vendor", card_name);
    match fs::read_to_string(sys_path) {
        Ok(s) => Some(s.trim().to_string()),
        Err(_) => None,
    }
}

/// Ищет и возвращает путь к первому подходящему /dev/dri/cardX
/// Правила выбора:
/// - предпочитаем записи начинающиеся с `card` (не renderD)
/// - пропускаем устройства с vendor == 0x10de (NVIDIA) если есть другие опции
pub fn find_best_drm_card() -> Result<PathBuf> {
    let mut cards: Vec<PathBuf> = fs::read_dir("/dev/dri")?
        .filter_map(|e| e.ok())
        .map(|e| e.path())
        .filter(|p| {
            if let Some(name) = p.file_name().and_then(|n| n.to_str()) {
                name.starts_with("card") || name.starts_with("renderD")
            } else {
                false
            }
        })
        .collect();

    // Сортируем так, чтобы card* были первыми
    cards.sort_by_key(|p| {
        p.file_name()
            .and_then(|n| n.to_str())
            .map(|s| if s.starts_with("card") { 0 } else { 1 })
            .unwrap_or(1)
    });

    if cards.is_empty() {
        return Err(anyhow!("/dev/dri пуст или нет устройств card/renderD"));
    }

    // Сначала попытаемся найти валидный не-NVIDIA card
    let mut fallback: Option<PathBuf> = None;

    for p in &cards {
        let name = p.file_name().and_then(|n| n.to_str()).unwrap_or("unknown");

        // Узнаём vendor через sysfs
        let vendor = read_sys_vendor(name);

        // Если это NVIDIA, отметим как возможный fallback, но не используем сразу
        let is_nvidia = vendor
            .as_deref()
            .map(|v| v.contains("10de") || v.contains("0x10de"))
            .unwrap_or(false);

        // Пробуем валидность KMS
        if probe_card(p) {
            if is_nvidia {
                // сохранём как fallback, но сначала ищем другое
                fallback = Some(p.clone());
                continue;
            } else {
                return Ok(p.clone());
            }
        }
    }

    // Если не нашли ничего кроме NVIDIA — используем её как fallback
    if let Some(p) = fallback {
        // финальная проверка probe — если провал, вернём ошибку
        if probe_card(&p) {
            return Ok(p);
        }
    }

    Err(anyhow!("Не найден подходящий DRM/KMS device (cardX)"))
}

/// Открывает найденный card и возвращает File
pub fn open_best_drm() -> Result<std::fs::File> {
    let path = find_best_drm_card()?;
    // Открываем с правами read/write — требуется для modeset и KMS
    let f = std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .open(&path)
        .with_context(|| format!("Не удалось открыть {:?}", path))?;
    Ok(f)
}
