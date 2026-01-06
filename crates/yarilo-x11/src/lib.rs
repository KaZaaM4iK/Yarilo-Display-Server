use anyhow::Result;
use byteorder::{BigEndian, ByteOrder};
use std::sync::Arc;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::UnixListener;
use tokio::net::UnixStream;
use tracing::{info, warn};
use wgpu::{
    CommandEncoderDescriptor, TexelCopyBufferInfo, TexelCopyBufferLayout, TexelCopyTextureInfo,
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
    let mut damage_map: std::collections::HashMap<u32, u32> = std::collections::HashMap::new();
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

                        let layer = Layer::new(texture, view, x, y, width, height, wid);
                        layer.clear_to_black(core.device(), core.queue());
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
                // ConfigureWindow
                // =========================
                12 => {
                    if total_len < 12 {
                        continue;
                    }

                    let wid = BigEndian::read_u32(&leftover[4..8]);
                    let value_mask = BigEndian::read_u16(&leftover[8..10]);

                    // values start at offset 12
                    let mut off = 12usize;

                    let mut new_x: Option<u32> = None;
                    let mut new_y: Option<u32> = None;
                    let mut new_w: Option<u32> = None;
                    let mut new_h: Option<u32> = None;

                    // bit 0 X, 1 Y, 2 Width, 3 Height, 4 BorderWidth (ignored but consumed)
                    for bit in 0..16 {
                        if (value_mask & (1u16 << bit)) == 0 {
                            continue;
                        }
                        if off + 4 > total_len {
                            break;
                        }
                        let v = BigEndian::read_u32(&leftover[off..off + 4]);
                        off += 4;

                        match bit {
                            0 => {
                                // X is i32 in protocol, but we clamp to >=0 for now
                                new_x = Some((v as i32).max(0) as u32);
                            }
                            1 => {
                                new_y = Some((v as i32).max(0) as u32);
                            }
                            2 => {
                                new_w = Some(v.max(1));
                            }
                            3 => {
                                new_h = Some(v.max(1));
                            }
                            4 => {
                                // BorderWidth: ignore (but consumed)
                            }
                            _ => {
                                // ignore other bits
                            }
                        }
                    }

                    let mut layers = core.layers().write();
                    if let Some(layer) = layers.iter_mut().find(|l| l.z() == wid) {
                        let mut moved = false;
                        let mut resized = false;

                        if let Some(x) = new_x {
                            if layer.position().0 != x {
                                moved = true;
                            }
                        }
                        if let Some(y) = new_y {
                            if layer.position().1 != y {
                                moved = true;
                            }
                        }

                        if moved {
                            let x = new_x.unwrap_or(layer.position().0);
                            let y = new_y.unwrap_or(layer.position().1);
                            layer.set_position(x, y);
                        }

                        if let Some(w) = new_w {
                            if layer.size().0 != w {
                                resized = true;
                            }
                        }
                        if let Some(h) = new_h {
                            if layer.size().1 != h {
                                resized = true;
                            }
                        }

                        if resized {
                            let w = new_w.unwrap_or(layer.size().0);
                            let h = new_h.unwrap_or(layer.size().1);
                            layer.resize(w, h, core.device(), core.queue());
                        } else if moved {
                            // move-only: очищаем можно не обязательно, но по задаче — clear_to_black + damaged
                            layer.clear_to_black(core.device(), core.queue());
                            layer.mark_damaged();
                        }
                    }
                }

                // =========================
                // UnmapWindow
                // =========================
                14 => {
                    if total_len >= 8 {
                        let wid = BigEndian::read_u32(&leftover[4..8]);
                        core.layers().write().retain(|l| l.z() != wid);
                    }
                }

                // =========================
                // DestroyWindow
                // =========================
                16 => {
                    if total_len >= 8 {
                        let wid = BigEndian::read_u32(&leftover[4..8]);
                        core.layers().write().retain(|l| l.z() != wid);
                    }
                }

                // =========================
                // PutImage (ZPixmap, depth 24)
                // =========================
                72 => {
                    if total_len < 24 {
                        continue;
                    }

                    let format = leftover[1];
                    if format != 2 {
                        continue; // только ZPixmap
                    }

                    let drawable = BigEndian::read_u32(&leftover[4..8]);
                    let width = BigEndian::read_u16(&leftover[12..14]) as u32;
                    let height = BigEndian::read_u16(&leftover[14..16]) as u32;
                    let dst_x = BigEndian::read_i16(&leftover[16..18]).max(0) as u32;
                    let dst_y = BigEndian::read_i16(&leftover[18..20]).max(0) as u32;
                    let depth = leftover[20];

                    if depth != 24 {
                        continue;
                    }

                    let header_size = 24;
                    let unpadded_bpr = width * 4;
                    let align = wgpu::COPY_BYTES_PER_ROW_ALIGNMENT as u32;
                    let padded_bpr = ((unpadded_bpr + (align - 1)) / align) * align;
                    let data_size = (padded_bpr as usize) * (height as usize);

                    if leftover.len() < header_size + data_size {
                        continue;
                    }

                    let pixel_data = &leftover[header_size..header_size + data_size];

                    let layer_opt = {
                        let layers = core.layers().read();
                        layers.iter().find(|l| l.z() == drawable).cloned()
                    };

                    let Some(layer) = layer_opt else { continue };

                    let staging = core.device().create_buffer(&wgpu::BufferDescriptor {
                        label: Some("PutImage Staging Buffer"),
                        size: data_size as u64,
                        usage: wgpu::BufferUsages::COPY_SRC,
                        mapped_at_creation: true,
                    });

                    {
                        let mut map = staging.slice(..).get_mapped_range_mut();
                        let row_bytes = (width * 4) as usize;
                        let src_stride = row_bytes;
                        let dst_stride = padded_bpr as usize;

                        for row in 0..(height as usize) {
                            let src_off = row * src_stride;
                            let dst_off = row * dst_stride;
                            map[dst_off..dst_off + row_bytes]
                                .copy_from_slice(&pixel_data[src_off..src_off + row_bytes]);
                        }
                    }

                    staging.unmap();

                    let mut encoder =
                        core.device()
                            .create_command_encoder(&CommandEncoderDescriptor {
                                label: Some("PutImage Encoder"),
                            });

                    encoder.copy_buffer_to_texture(
                        TexelCopyBufferInfo {
                            buffer: &staging,
                            layout: TexelCopyBufferLayout {
                                offset: 0,
                                bytes_per_row: Some(padded_bpr),
                                rows_per_image: Some(height),
                            },
                        },
                        TexelCopyTextureInfo {
                            texture: layer.texture(),
                            mip_level: 0,
                            origin: wgpu::Origin3d {
                                x: dst_x,
                                y: dst_y,
                                z: 0,
                            },
                            aspect: wgpu::TextureAspect::All,
                        },
                        wgpu::Extent3d {
                            width,
                            height,
                            depth_or_array_layers: 1,
                        },
                    );

                    core.queue().submit(Some(encoder.finish()));

                    if let Some(l) = core.layers().write().iter_mut().find(|l| l.z() == drawable) {
                        l.mark_damaged();
                    }
                }

                // =========================
                // CopyArea
                // =========================
                62 => {
                    if total_len >= 28 {
                        let src = BigEndian::read_u32(&leftover[4..8]);
                        let dst = BigEndian::read_u32(&leftover[8..12]);

                        // Пока только self-copy
                        if src == dst {
                            if let Some(layer) =
                                core.layers().write().iter_mut().find(|l| l.z() == dst)
                            {
                                layer.mark_damaged();
                            }
                        }
                    }
                }

                // =========================
                // GetImage (ZPixmap, depth 24) — минимальный reply
                // =========================
                70 => {
                    // Request: opcode(1) format(1) length(2) drawable(4) x(2) y(2) width(2) height(2) plane_mask(4)
                    if total_len < 20 {
                        continue;
                    }

                    let format = leftover[1];
                    if format != 2 {
                        continue; // только ZPixmap
                    }

                    let drawable = BigEndian::read_u32(&leftover[4..8]);
                    let x = BigEndian::read_i16(&leftover[8..10]).max(0) as u32;
                    let y = BigEndian::read_i16(&leftover[10..12]).max(0) as u32;
                    let width = BigEndian::read_u16(&leftover[12..14]) as u32;
                    let height = BigEndian::read_u16(&leftover[14..16]) as u32;

                    if width == 0 || height == 0 {
                        continue;
                    }

                    let layer_opt = {
                        let layers = core.layers().read();
                        layers.iter().find(|l| l.z() == drawable).cloned()
                    };
                    let Some(layer) = layer_opt else { continue };

                    let unpadded_bpr = width * 4;
                    let align = wgpu::COPY_BYTES_PER_ROW_ALIGNMENT as u32;
                    let padded_bpr = ((unpadded_bpr + (align - 1)) / align) * align;

                    let readback = core.device().create_buffer(&wgpu::BufferDescriptor {
                        label: Some("GetImage Readback Buffer"),
                        size: (padded_bpr as u64) * (height as u64),
                        usage: wgpu::BufferUsages::COPY_DST | wgpu::BufferUsages::MAP_READ,
                        mapped_at_creation: false,
                    });

                    let mut encoder =
                        core.device()
                            .create_command_encoder(&CommandEncoderDescriptor {
                                label: Some("GetImage Encoder"),
                            });

                    encoder.copy_texture_to_buffer(
                        TexelCopyTextureInfo {
                            texture: layer.texture(),
                            mip_level: 0,
                            origin: wgpu::Origin3d { x, y, z: 0 },
                            aspect: wgpu::TextureAspect::All,
                        },
                        TexelCopyBufferInfo {
                            buffer: &readback,
                            layout: TexelCopyBufferLayout {
                                offset: 0,
                                bytes_per_row: Some(padded_bpr),
                                rows_per_image: Some(height),
                            },
                        },
                        wgpu::Extent3d {
                            width,
                            height,
                            depth_or_array_layers: 1,
                        },
                    );

                    core.queue().submit(Some(encoder.finish()));

                    let slice = readback.slice(..);
                    slice.map_async(wgpu::MapMode::Read, |_| ());
                    let _ = core.device().poll(wgpu::PollType::Wait {
                        submission_index: None,
                        timeout: None,
                    });

                    let mapped = slice.get_mapped_range();

                    // Убираем padding построчно, в X11 отдаём width*height*4
                    let row_bytes = (width * 4) as usize;
                    let src_stride = padded_bpr as usize;

                    let mut out = Vec::with_capacity(row_bytes * (height as usize));
                    for row in 0..(height as usize) {
                        let src_off = row * src_stride;
                        out.extend_from_slice(&mapped[src_off..src_off + row_bytes]);
                    }

                    drop(mapped);
                    readback.unmap();

                    // Reply: 32 bytes header + data
                    // byte0 = 1 (reply), byte1 = depth, bytes2-3 = sequence (0), bytes4-7 = length (u32 words after 32-byte header)
                    let data_len = out.len();
                    let reply_len_words = ((data_len + 3) / 4) as u32;

                    let mut reply = Vec::with_capacity(32 + data_len);
                    reply.push(1); // reply
                    reply.push(24); // depth
                    reply.extend_from_slice(&0u16.to_be_bytes()); // sequence
                    reply.extend_from_slice(&reply_len_words.to_be_bytes()); // length in 4-byte units
                    reply.extend_from_slice(&0u32.to_be_bytes()); // visual (0)
                    reply.extend_from_slice(&[0u8; 20]); // pad to 32 bytes
                    reply.extend_from_slice(&out);

                    stream.write_all(&reply).await?;
                }

                // =========================
                // DAMAGE extension (opcode 143) — упрощённо
                // minor:
                // 1 = Create(drawable, damage_id)
                // 2 = Destroy(damage_id)
                // 3 = Subtract(damage_id, ...ignored)
                // 4 = Add(damage_id, ...ignored)
                // Любое событие => mark_damaged(drawable)
                // =========================
                143 => {
                    if total_len < 8 {
                        continue;
                    }
                    let minor = leftover[1];

                    match minor {
                        // Create: drawable(4) damage(4)
                        1 => {
                            if total_len < 12 {
                                continue;
                            }
                            let drawable = BigEndian::read_u32(&leftover[4..8]);
                            let damage_id = BigEndian::read_u32(&leftover[8..12]);
                            damage_map.insert(damage_id, drawable);

                            if let Some(layer) =
                                core.layers().write().iter_mut().find(|l| l.z() == drawable)
                            {
                                layer.mark_damaged();
                            }
                        }
                        // Destroy: damage(4)
                        2 => {
                            let damage_id = BigEndian::read_u32(&leftover[4..8]);
                            if let Some(drawable) = damage_map.remove(&damage_id) {
                                if let Some(layer) =
                                    core.layers().write().iter_mut().find(|l| l.z() == drawable)
                                {
                                    layer.mark_damaged();
                                }
                            }
                        }
                        // Subtract/Add: damage(4) + regions (ignored)
                        3 | 4 => {
                            let damage_id = BigEndian::read_u32(&leftover[4..8]);
                            if let Some(&drawable) = damage_map.get(&damage_id) {
                                if let Some(layer) =
                                    core.layers().write().iter_mut().find(|l| l.z() == drawable)
                                {
                                    layer.mark_damaged();
                                }
                            }
                        }
                        _ => {
                            // unknown minor
                        }
                    }
                }

                // =========================
                // PolyFillRectangle
                // =========================
                55 => {
                    if total_len >= 16 {
                        let drawable = BigEndian::read_u32(&leftover[4..8]);
                        let mut layers = core.layers().write();
                        if let Some(layer) = layers.iter_mut().find(|l| l.z() == drawable) {
                            layer.mark_damaged();
                        }
                    }
                }
                _ => {
                    warn!("Unknown opcode {} (len={})", opcode, total_len);
                }
            }

            leftover.drain(..total_len);
        }
    }

    Ok(())
}
