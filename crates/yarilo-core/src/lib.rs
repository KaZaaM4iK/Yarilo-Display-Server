use anyhow::{Context, Result, anyhow};
use std::{
    os::unix::io::{AsFd, AsRawFd},
    sync::Arc,
};
use tracing::info;

use drm::Device as BasicDevice;
use drm::control::{Device as ControlDevice, ResourceHandles};

use wgpu::{
    Adapter, Color, CommandEncoderDescriptor, Device, LoadOp, Operations, Queue,
    RenderPassColorAttachment, RenderPassDescriptor, StoreOp,
};

use wgpu::{TexelCopyBufferInfo, TexelCopyBufferLayout, TexelCopyTextureInfo};

use parking_lot::RwLock;
use wgpu::PollType;

// Импортируем типы для низкоуровневых хендлов
mod gpu;
use gpu::open_best_drm;
use parking_lot::Mutex;

#[derive(Clone)]
pub struct Layer {
    texture: wgpu::Texture,
    view: wgpu::TextureView,
    x: u32,
    y: u32,
    width: u32,
    height: u32,
    z: u32,
}

#[derive(Debug, Clone)]
struct DrmDevice(Arc<std::fs::File>);

impl AsFd for DrmDevice {
    fn as_fd(&self) -> std::os::unix::io::BorrowedFd<'_> {
        self.0.as_fd()
    }
}

impl AsRawFd for DrmDevice {
    fn as_raw_fd(&self) -> std::os::unix::io::RawFd {
        self.0.as_raw_fd()
    }
}

impl BasicDevice for DrmDevice {}
impl ControlDevice for DrmDevice {}

impl Layer {
    pub fn new(
        texture: wgpu::Texture,
        view: wgpu::TextureView,
        x: u32,
        y: u32,
        width: u32,
        height: u32,
        z: u32,
    ) -> Self {
        Self {
            texture,
            view,
            x,
            y,
            width,
            height,
            z,
        }
    }

    pub fn texture(&self) -> &wgpu::Texture {
        &self.texture
    }

    pub fn view(&self) -> &wgpu::TextureView {
        &self.view
    }

    pub fn position(&self) -> (u32, u32) {
        (self.x, self.y)
    }

    pub fn size(&self) -> (u32, u32) {
        (self.width, self.height)
    }

    pub fn z(&self) -> u32 {
        self.z
    }
}

pub struct YariloCore {
    drm_dev: DrmDevice,
    gbm_dev: gbm::Device<DrmDevice>,

    device: Device,
    queue: Queue,
    adapter: Adapter,

    width: u32,
    height: u32,
    crtc: drm::control::crtc::Handle,
    connector: drm::control::connector::Handle,

    gbm_bo_front: Mutex<gbm::BufferObject<()>>,
    gbm_bo_back: Mutex<gbm::BufferObject<()>>,

    layers: Arc<RwLock<Vec<Layer>>>,
}

impl YariloCore {
    pub async fn new() -> Result<Self> {
        // 1. Права доступа (Seat)
        let _seat =
            libseat::Seat::open(|_, _| {}).map_err(|e| anyhow!("Failed to open seat: {}", e))?;

        // 2. Автовыбор доступного DRM устройства
        let drm_file = open_best_drm().context("Не удалось найти/открыть DRM device")?;
        let drm_dev = DrmDevice(Arc::new(drm_file));
        let gbm_dev =
            gbm::Device::new(drm_dev.clone()).context("Не удалось инициализировать GBM")?;

        let instance = wgpu::Instance::new(&wgpu::InstanceDescriptor {
            backends: wgpu::Backends::VULKAN,
            flags: wgpu::InstanceFlags::empty(),
            memory_budget_thresholds: Default::default(),
            backend_options: Default::default(),
        });

        // 4. KMS / Mode Setting
        let handles: ResourceHandles = drm_dev.resource_handles()?;
        let (connector_handle, mode) = handles
            .connectors()
            .iter()
            .find_map(|&c| {
                let info = drm_dev.get_connector(c, true).ok()?;
                if info.state() == drm::control::connector::State::Connected
                    && !info.modes().is_empty()
                {
                    Some((c, info.modes()[0]))
                } else {
                    None
                }
            })
            .ok_or_else(|| anyhow!("Нет активного монитора (connector)"))?;

        let (w, h) = mode.size();

        let encoder_handle = drm_dev
            .get_connector(connector_handle, true)?
            .current_encoder()
            .ok_or_else(|| anyhow!("Connector не подключен к Encoder"))?;

        let crtc_handle = drm_dev
            .get_encoder(encoder_handle)?
            .crtc()
            .ok_or_else(|| anyhow!("Encoder не имеет активного CRTC"))?;

        // 5. Создаем Dumb Buffer (или GBM BO) для старта
        let gbm_bo_front = gbm_dev.create_buffer_object::<()>(
            w as u32,
            h as u32,
            gbm::Format::Xrgb8888,
            gbm::BufferObjectFlags::SCANOUT | gbm::BufferObjectFlags::RENDERING,
        )?;

        let gbm_bo_back = gbm_dev.create_buffer_object::<()>(
            w as u32,
            h as u32,
            gbm::Format::Xrgb8888,
            gbm::BufferObjectFlags::SCANOUT | gbm::BufferObjectFlags::RENDERING,
        )?;

        // Initial framebuffer и page_flip на front
        let fb_front = drm_dev.add_framebuffer(&gbm_bo_front, 24, 32)?;
        drm_dev.page_flip(
            crtc_handle,
            fb_front,
            drm::control::PageFlipFlags::EVENT,
            None,
        )?;

        // Initial page flip to front BO
        let fb_front = drm_dev.add_framebuffer(&gbm_bo_front, 24, 32)?;
        drm_dev.page_flip(
            crtc_handle,
            fb_front,
            drm::control::PageFlipFlags::empty(),
            None,
        )?;

        // 6. Создаем Surface (ИСПРАВЛЕНО для wgpu 0.19+)

        // 7. Инициализация адаптера (ИСПРАВЛЕНО определение типа)
        // Мы явно обрабатываем результат, не полагаясь на ok_or_else
        let adapter = instance.request_adapter(&wgpu::RequestAdapterOptions {
            compatible_surface: None, // без surface — offscreen only
            force_fallback_adapter: false,
            power_preference: wgpu::PowerPreference::HighPerformance,
        });
        let adapter = instance
            .request_adapter(&wgpu::RequestAdapterOptions {
                compatible_surface: None,
                force_fallback_adapter: false,
                power_preference: wgpu::PowerPreference::HighPerformance,
            })
            .await
            .map_err(|e| anyhow!("No suitable adapter found: {e}"))?;

        // Универсальный хак: в зависимости от версии wgpu это может быть Option или Result
        // Но так как у тебя компилятор ругался на Result, мы используем match или .unwrap
        // В данном контексте, если это Option, код ниже не скомпилится, но твой лог говорит Result.
        // Я напишу код, который работает если это Option, а если Result - используй .map_err

        // ВНИМАНИЕ: Если компилятор скажет "expected Option, found Result", убери `.ok_or(...)`
        // Судя по твоему логу, у тебя Result.
        // Если код выше упадет, замени его на `.ok_or(anyhow!("..."))?` если это Option.

        let device_desc: wgpu::DeviceDescriptor<'static> = wgpu::DeviceDescriptor {
            label: Some("yarilo-device"),
            required_features: wgpu::Features::empty(),
            required_limits: wgpu::Limits::downlevel_defaults(),
            experimental_features: wgpu::ExperimentalFeatures::disabled(),
            memory_hints: wgpu::MemoryHints::default(),
            trace: wgpu::Trace::Off,
        };

        let device_future = adapter.request_device(&device_desc);

        let device_result: Result<(wgpu::Device, wgpu::Queue), _> = device_future.await;

        let (device, queue) =
            device_result.map_err(|e| anyhow!("Не удалось получить Device: {e}"))?;

        info!("YARILO KERNEL: GPU Init OK. {}×{}", w, h);

        // Читаем поля осмысленно: убираем dead_code и фиксируем диагностику железа.
        let adapter_info = adapter.get_info();
        info!(
            "YARILO KERNEL: Adapter = {} ({:?}), backend={:?}",
            adapter_info.name, adapter_info.device_type, adapter_info.backend
        );
        info!("YARILO KERNEL: DRM fd={}", drm_dev.as_raw_fd());

        Ok(Self {
            drm_dev,
            gbm_dev,
            device,
            queue,
            adapter,
            width: w as u32,
            height: h as u32,
            crtc: crtc_handle,
            connector: connector_handle,
            gbm_bo_front: Mutex::new(gbm_bo_front),
            gbm_bo_back: Mutex::new(gbm_bo_back),
            layers: Arc::new(RwLock::new(vec![])),
        })
    }

    pub fn device(&self) -> &Device {
        &self.device
    }

    pub fn queue(&self) -> &Queue {
        &self.queue
    }

    pub fn layers(&self) -> &Arc<RwLock<Vec<Layer>>> {
        &self.layers
    }

    pub async fn run(self: Arc<Self>) -> Result<()> {
        info!("YARILO LOOP STARTED — offscreen + GBM double buffer flip");

        let texture_desc = wgpu::TextureDescriptor {
            label: Some("Offscreen Framebuffer"),
            size: wgpu::Extent3d {
                width: self.width,
                height: self.height,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Bgra8UnormSrgb,
            usage: wgpu::TextureUsages::RENDER_ATTACHMENT | wgpu::TextureUsages::COPY_SRC,
            view_formats: &[],
        };
        let offscreen_texture = self.device.create_texture(&texture_desc);
        let offscreen_view = offscreen_texture.create_view(&wgpu::TextureViewDescriptor::default());
        let layers_for_x11 = self.layers.clone();

        // Staging buffer для copy texture → CPU → mmap BO (простой путь)
        let staging_buffer = self.device.create_buffer(&wgpu::BufferDescriptor {
            label: Some("Staging Buffer"),
            size: (self.width * self.height * 4) as u64,
            usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
            mapped_at_creation: false,
        });

        loop {
            let mut encoder = self
                .device
                .create_command_encoder(&CommandEncoderDescriptor::default());

            // Рендер зелёного в offscreen
            {
                let _pass = encoder.begin_render_pass(&RenderPassDescriptor {
                    label: Some("Green Clear"),
                    color_attachments: &[Some(RenderPassColorAttachment {
                        view: &offscreen_view,
                        resolve_target: None,
                        depth_slice: None,
                        ops: Operations {
                            load: LoadOp::Clear(Color {
                                r: 0.0,
                                g: 0.5,
                                b: 0.0,
                                a: 1.0,
                            }),
                            store: StoreOp::Store,
                        },
                    })],
                    depth_stencil_attachment: None,
                    timestamp_writes: None,
                    occlusion_query_set: None,
                    multiview_mask: None,
                });
            }

            // Copy offscreen → staging
            encoder.copy_texture_to_buffer(
                TexelCopyTextureInfo {
                    texture: &offscreen_texture,
                    mip_level: 0,
                    origin: wgpu::Origin3d::ZERO,
                    aspect: wgpu::TextureAspect::All,
                },
                TexelCopyBufferInfo {
                    buffer: &staging_buffer,
                    layout: TexelCopyBufferLayout {
                        offset: 0,
                        bytes_per_row: Some(4 * self.width),
                        rows_per_image: Some(self.height),
                    },
                },
                wgpu::Extent3d {
                    width: self.width,
                    height: self.height,
                    depth_or_array_layers: 1,
                },
            );

            // Композиция слоёв (пока пусто, потом X11 окна)
            for layer in self.layers.read().iter() {
                encoder.copy_texture_to_texture(
                    TexelCopyTextureInfo {
                        texture: &layer.texture,
                        mip_level: 0,
                        origin: wgpu::Origin3d::ZERO,
                        aspect: wgpu::TextureAspect::All,
                    },
                    TexelCopyTextureInfo {
                        texture: &offscreen_texture,
                        mip_level: 0,
                        origin: wgpu::Origin3d {
                            x: layer.x,
                            y: layer.y,
                            z: 0,
                        },
                        aspect: wgpu::TextureAspect::All,
                    },
                    wgpu::Extent3d {
                        width: layer.width,
                        height: layer.height,
                        depth_or_array_layers: 1,
                    },
                );
            }

            self.queue.submit(Some(encoder.finish()));

            // Map staging и copy в mmap'нутый BO
            let buffer_slice = staging_buffer.slice(..);
            buffer_slice.map_async(wgpu::MapMode::Read, |_| ());
            let _ = self.device.poll(PollType::Wait {
                submission_index: None,
                timeout: None,
            });
            let data = buffer_slice.get_mapped_range();
            {
                let mut back = self.gbm_bo_back.lock();
                back.map_mut(
                    0,
                    0,
                    self.width,
                    self.height,
                    |map: &mut gbm::MappedBufferObject<'_, ()>| {
                        let dst = map.buffer_mut();
                        dst[..data.len()].copy_from_slice(&data);
                    },
                )?;
            } // back guard дропнет

            drop(data);
            staging_buffer.unmap();

            // Page flip
            // Page flip
            let fb = {
                let back = self.gbm_bo_back.lock();
                self.drm_dev.add_framebuffer(&*back, 24, 32)?
            };

            self.drm_dev
                .page_flip(self.crtc, fb, drm::control::PageFlipFlags::EVENT, None)?;

            // Swap back buffer
            {
                let mut front = self.gbm_bo_front.lock();
                let mut back = self.gbm_bo_back.lock();
                std::mem::swap(&mut *front, &mut *back);
            }

            tokio::time::sleep(std::time::Duration::from_millis(16)).await;
        }
    }
}
