use anyhow::Result;
use byteorder::{BigEndian, ByteOrder};
use std::sync::Arc;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::UnixListener;
use tokio::net::UnixStream;
use tracing::{info, warn};
use wgpu::{
    Color, CommandEncoderDescriptor, LoadOp, Operations, RenderPassColorAttachment,
    RenderPassDescriptor, StoreOp,
};
use yarilo_core::{Layer, YariloCore};

const X11_SOCKET: &str = "/tmp/.X11-unix/X0";

pub async fn run_x11_server(core: Arc<YariloCore>) -> Result<()> {
    // Удаляем старый сокет если есть
    let _ = std::fs::remove_file(X11_SOCKET);

    let listener = UnixListener::bind(X11_SOCKET)?;
    let core = core.clone(); // для замыкания
    info!("X11 сервер слушает на {}", X11_SOCKET);

    loop {
        let (stream, _) = listener.accept().await?;
        let core = core.clone();
        tokio::spawn(handle_client(stream, core));
    }
}

async fn handle_client(mut stream: UnixStream, core: Arc<YariloCore>) -> Result<()> {
    let mut buf = [0u8; 12]; // setup request минимум 12 байт
    stream.read_exact(&mut buf).await?;

    // Парсим setup: byte order, protocol major/minor, auth info
    let byte_order = buf[0];
    let _is_lsb = byte_order == b'l';
    let _major = BigEndian::read_u16(&buf[6..8]);
    let _minor = BigEndian::read_u16(&buf[8..10]);
    let _auth_name_len = BigEndian::read_u16(&buf[10..12]);

    // Пропускаем остаток setup (auth строки и т.д.)
    // В минимальной версии игнорим auth — всегда Success
    info!("X11 клиент подключился (игнорируем auth)");

    // Формируем Success reply
    let mut reply = Vec::with_capacity(40);
    reply.push(1); // success
    reply.push(0); // unused
    reply.extend_from_slice(&11u16.to_be_bytes()); // protocol major 11
    reply.extend_from_slice(&0u16.to_be_bytes()); // protocol minor 0
    reply.extend_from_slice(&8u16.to_be_bytes()); // extra length (в 4-байтовых словах)

    // Базовая информация о экране (берём из yarilo-core — пока хардкод 1920x1080)
    let width_px: u16 = 1920;
    let height_px: u16 = 1080;
    let width_mm: u16 = 508;
    let height_mm: u16 = 286;

    reply.extend_from_slice(&0u32.to_be_bytes()); // release number
    reply.extend_from_slice(&1u32.to_be_bytes()); // resource id base (пока 1)
    reply.extend_from_slice(&0u32.to_be_bytes()); // resource id mask
    reply.extend_from_slice(&0u32.to_be_bytes()); // motion buffer size
    reply.extend_from_slice(&1u16.to_be_bytes()); // vendor length (пока минимально)
    reply.extend_from_slice(&0u16.to_be_bytes()); // maximum request length
    reply.push(1); // screens count
    reply.push(1); // pixmap formats count
    reply.push(0); // image byte order LSB
    reply.push(0); // bitmap bit order LSB
    reply.push(32); // bitmap scanline unit
    reply.push(32); // bitmap scanline pad
    reply.push(1); // min keycode
    reply.push(255); // max keycode
    reply.extend_from_slice(&[0u8; 4]); // padding

    // Pixmap format: depth 24, bpp 32
    reply.push(24); // depth
    reply.push(32); // bpp
    reply.push(32); // scanline pad
    reply.extend_from_slice(&[0u8; 5]); // padding

    // Root screen
    reply.extend_from_slice(&1u32.to_be_bytes()); // root window id
    reply.extend_from_slice(&0u32.to_be_bytes()); // default colormap
    reply.extend_from_slice(&0x00ffffffu32.to_be_bytes()); // white pixel
    reply.extend_from_slice(&0x00000000u32.to_be_bytes()); // black pixel
    reply.extend_from_slice(&0u32.to_be_bytes()); // current input masks
    reply.extend_from_slice(&width_px.to_be_bytes());
    reply.extend_from_slice(&height_px.to_be_bytes());
    reply.extend_from_slice(&width_mm.to_be_bytes());
    reply.extend_from_slice(&height_mm.to_be_bytes());
    reply.extend_from_slice(&1u16.to_be_bytes()); // min installed maps
    reply.extend_from_slice(&1u16.to_be_bytes()); // max installed maps
    reply.extend_from_slice(&1u32.to_be_bytes()); // root visual
    reply.push(1); // backing stores never
    reply.push(1); // save unders true
    reply.push(24); // root depth
    reply.push(1); // allowed depths count

    // Visual
    reply.extend_from_slice(&1u32.to_be_bytes()); // visual id
    reply.push(5); // class TrueColor
    reply.push(24); // bits per rgb
    reply.push(16); // colormap entries
    reply.extend_from_slice(&0xff0000u32.to_be_bytes()); // red mask
    reply.extend_from_slice(&0x00ff00u32.to_be_bytes()); // green mask
    reply.extend_from_slice(&0x0000ffu32.to_be_bytes()); // blue mask
    reply.extend_from_slice(&[0u8; 4]); // padding

    stream.write_all(&reply).await?;
    info!("X11 Connection Setup Success отправлен");

    // Основной цикл чтения запросов
    let mut buf = vec![0u8; 4096];
    let mut leftover = Vec::new(); // для неполных сообщений
    loop {
        let n = stream.read(&mut buf).await?;
        if n == 0 {
            break;
        }
        leftover.extend_from_slice(&buf[..n]);

        while leftover.len() >= 4 {
            // минимум длина X11 request
            let opcode = leftover[0];
            let length_field = BigEndian::read_u16(&leftover[2..4]) as usize;
            let total_len = length_field * 4;

            let total_len = if length_field > 0 {
                length_field as usize * 4 // в 4-байтных словах
            } else {
                4 // минимальный запрос
            };

            if leftover.len() < total_len {
                break;
            } // ждём остаток

            match opcode {
                // =========================
                // CreateWindow
                // =========================
                1 => {
                    if total_len >= 32 {
                        let wid = BigEndian::read_u32(&leftover[4..8]);
                        let _parent = BigEndian::read_u32(&leftover[8..12]);
                        let x = BigEndian::read_i16(&leftover[12..14]) as u32;
                        let y = BigEndian::read_i16(&leftover[14..16]) as u32;
                        let width = BigEndian::read_u16(&leftover[16..18]) as u32;
                        let height = BigEndian::read_u16(&leftover[18..20]) as u32;

                        info!(
                            "CreateWindow wid={:x} {}x{} @ {},{}",
                            wid, width, height, x, y
                        );

                        let texture_desc = wgpu::TextureDescriptor {
                            label: Some("X11 Window Backing Store"),
                            size: wgpu::Extent3d {
                                width,
                                height,
                                depth_or_array_layers: 1,
                            },
                            mip_level_count: 1,
                            sample_count: 1,
                            dimension: wgpu::TextureDimension::D2,
                            format: wgpu::TextureFormat::Bgra8UnormSrgb,
                            usage: wgpu::TextureUsages::RENDER_ATTACHMENT
                                | wgpu::TextureUsages::COPY_SRC
                                | wgpu::TextureUsages::TEXTURE_BINDING,
                            view_formats: &[],
                        };

                        let texture = core.device().create_texture(&texture_desc);
                        let view = texture.create_view(&wgpu::TextureViewDescriptor::default());

                        // initial clear = black
                        {
                            // initial clear = black
                            let mut encoder = core
                                .device()
                                .create_command_encoder(&CommandEncoderDescriptor::default());

                            {
                                encoder.begin_render_pass(&RenderPassDescriptor {
                                    label: Some("Initial Clear"),
                                    color_attachments: &[Some(RenderPassColorAttachment {
                                        view: &view,
                                        resolve_target: None,
                                        depth_slice: None,
                                        ops: Operations {
                                            load: LoadOp::Clear(Color::BLACK),
                                            store: StoreOp::Store,
                                        },
                                    })],
                                    depth_stencil_attachment: None,
                                    timestamp_writes: None,
                                    occlusion_query_set: None,
                                    multiview_mask: None,
                                });
                            } // RenderPass дропнулся здесь, borrow закончился

                            core.queue().submit(Some(encoder.finish()));
                        }

                        let layer = Layer::new(texture, view, x, y, width, height, wid);

                        core.layers().write().push(layer);
                    }
                }

                // =========================
                // MapWindow
                // =========================
                8 => {
                    if total_len >= 8 {
                        let wid = BigEndian::read_u32(&leftover[4..8]);
                        info!("MapWindow wid={:x}", wid);
                    }
                }

                // =========================
                // PolyFillRectangle
                // =========================
                55 => {
                    if total_len >= 16 {
                        let drawable = BigEndian::read_u32(&leftover[4..8]);

                        let fill_color = Color {
                            r: 1.0,
                            g: 1.0,
                            b: 1.0,
                            a: 1.0,
                        };

                        let layers = core.layers().read();
                        if let Some(layer) = layers.iter().find(|l| l.z() == drawable) {
                            let mut encoder =
                                core.device()
                                    .create_command_encoder(&CommandEncoderDescriptor {
                                        label: Some("PolyFill"),
                                    });

                            let mut encoder =
                                core.device()
                                    .create_command_encoder(&CommandEncoderDescriptor {
                                        label: Some("PolyFill"),
                                    });

                            {
                                encoder.begin_render_pass(&RenderPassDescriptor {
                                    label: Some("Clear Window"),
                                    color_attachments: &[Some(RenderPassColorAttachment {
                                        view: layer.view(),
                                        resolve_target: None,
                                        depth_slice: None,
                                        ops: Operations {
                                            load: LoadOp::Clear(fill_color),
                                            store: StoreOp::Store,
                                        },
                                    })],
                                    depth_stencil_attachment: None,
                                    timestamp_writes: None,
                                    occlusion_query_set: None,
                                    multiview_mask: None,
                                });
                            }

                            core.queue().submit(Some(encoder.finish()));
                        }
                    }
                }

                // =========================
                // Unknown
                // =========================
                _ => {
                    warn!("Unknown opcode {} (len={})", opcode, total_len);
                }
            }

            leftover.drain(..total_len);
        }
    }

    Ok(())
}
