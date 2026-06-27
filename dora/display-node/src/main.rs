use std::time::Instant;

use arrow::array::AsArray;
use dora_node_api::{self, DoraNode, Event};
use dora_node_api::futures::StreamExt;
use eyre::{Context, Result};
use minifb::{Key, Window, WindowOptions};

fn main() -> Result<()> {
    let rt = tokio::runtime::Runtime::new()?;
    rt.block_on(async {
        if let Err(e) = run().await {
            eprintln!("Display node error: {:?}", e);
        }
    });
    Ok(())
}

async fn run() -> Result<()> {
    let (_node, mut events) = DoraNode::init_from_env()
        .wrap_err("Failed to initialize dora node")?;

    println!("[display-node] Dora node initialized");

    // 窗口先用默认尺寸，收到第一帧后自适应
    let mut window = Window::new(
        "dora Camera View - ESC to exit",
        640,
        480,
        WindowOptions {
            resize: true,
            ..WindowOptions::default()
        },
    )
    .wrap_err("Failed to create window")?;

    window.set_target_fps(60);

    println!("[display-node] Window opened, waiting for frames...");

    let mut frame_count: u64 = 0;
    let mut last_report = Instant::now();
    let mut last_fps: f64 = 0.0;

    while let Some(event) = events.next().await {
        if !window.is_open() || window.is_key_down(Key::Escape) {
            println!("[display-node] Window closed, exiting");
            break;
        }

        match event {
            Event::Input { id, metadata, data } if id.to_string() == "image" => {
                // 从元数据读取分辨率
                let width = metadata.get_or("width", 640i64) as usize;
                let height = metadata.get_or("height", 480i64) as usize;

                match display_raw_rgb(&data, width, height, &mut window) {
                    Ok(()) => {
                        frame_count += 1;
                        let elapsed = last_report.elapsed().as_secs_f64();
                        if elapsed >= 1.0 {
                            last_fps = frame_count as f64 / elapsed;
                            println!(
                                "[display-node] {}x{} | {} frames | {:.0} fps",
                                width, height, frame_count, last_fps
                            );
                            frame_count = 0;
                            last_report = Instant::now();
                        }
                    }
                    Err(e) => eprintln!("[display-node] Display error: {:?}", e),
                }
            }
            Event::Stop(cause) => {
                println!(
                    "[display-node] Stop: {:?}, exiting ({:.0} fps)",
                    cause, last_fps
                );
                break;
            }
            _ => {}
        }
    }

    println!("[display-node] Shutting down");
    Ok(())
}

/// 直接渲染原始 RGB 数据
fn display_raw_rgb(
    data: &dora_node_api::ArrowData,
    width: usize,
    height: usize,
    window: &mut Window,
) -> Result<()> {
    let uint8_arr = data.as_primitive::<arrow::datatypes::UInt8Type>();
    let rgb_bytes = uint8_arr.values();

    let expected_len = width * height * 3;
    if rgb_bytes.len() < expected_len {
        eprintln!(
            "[display-node] Frame size mismatch: {} bytes (expected {} for {}x{})",
            rgb_bytes.len(),
            expected_len,
            width,
            height
        );
        return Ok(());
    }

    // RGB → u32 缓冲区（0x00RRGGBB）
    let buffer: Vec<u32> = rgb_bytes[..expected_len]
        .chunks_exact(3)
        .map(|pixel| {
            (pixel[0] as u32) << 16 | (pixel[1] as u32) << 8 | pixel[2] as u32
        })
        .collect();

    window
        .update_with_buffer(&buffer, width, height)
        .wrap_err("Failed to update window")?;

    Ok(())
}
