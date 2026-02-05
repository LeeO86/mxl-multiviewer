// SPDX-FileCopyrightText: 2026 Contributors to the MXL Multiviewer project.
// SPDX-License-Identifier: Apache-2.0

mod config;

use std::sync::{
    Arc,
    atomic::{AtomicBool, Ordering},
};

use anyhow::{Context, Result};
use clap::Parser;
use gstreamer as gst;
use gstreamer::prelude::*;
use multiviewer_pipeline::{FractionConfig, PipelineConfig};
use tracing::{info, warn};

#[derive(Debug, Parser)]
#[command(version = env!("CARGO_PKG_VERSION"), author = env!("CARGO_PKG_AUTHORS"))]
struct Cli {
    /// Path to JSON configuration file.
    #[arg(long, default_value = "config/multiviewer.json")]
    config: String,

    /// Path to the shmem directory where the mxl domain is mapped.
    #[arg(long)]
    mxl_domain: Option<String>,

    /// Four input flow IDs (repeat flag 4 times).
    #[arg(long)]
    input_flow_id: Vec<String>,

    /// Output flow ID for the multiview.
    #[arg(long)]
    output_flow_id: Option<String>,

    /// Output width in pixels.
    #[arg(long)]
    output_width: Option<i32>,

    /// Output height in pixels.
    #[arg(long)]
    output_height: Option<i32>,

    /// Output framerate numerator.
    #[arg(long)]
    framerate_numerator: Option<i32>,

    /// Output framerate denominator.
    #[arg(long)]
    framerate_denominator: Option<i32>,

    /// Interlace mode (e.g. progressive).
    #[arg(long)]
    interlace_mode: Option<String>,

    /// Colorimetry (e.g. bt709).
    #[arg(long)]
    colorimetry: Option<String>,
}

fn main() -> Result<()> {
    setup_logging();
    let cli = Cli::parse();

    let file_config = config::load_config(&cli.config)?;
    let resolved = resolve_config(file_config, &cli)?;

    gst::init().context("Failed to initialize GStreamer")?;
    let pipeline = multiviewer_pipeline::build_pipeline(&resolved)?;

    run_pipeline(pipeline)?;
    Ok(())
}

fn setup_logging() {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::builder()
                .with_default_directive(tracing::level_filters::LevelFilter::INFO.into())
                .from_env_lossy(),
        )
        .init();
}

fn resolve_config(file: config::FileConfig, cli: &Cli) -> Result<PipelineConfig> {
    let mxl_domain = cli
        .mxl_domain
        .clone()
        .unwrap_or_else(|| file.mxl_domain.clone());

    let input_flow_ids = if !cli.input_flow_id.is_empty() {
        cli.input_flow_id.clone()
    } else {
        file.input_flow_ids.clone()
    };
    if input_flow_ids.len() != 4 {
        anyhow::bail!(
            "Expected exactly 4 input flow IDs, got {}.",
            input_flow_ids.len()
        );
    }

    let output_flow_id = cli
        .output_flow_id
        .clone()
        .unwrap_or_else(|| file.output_flow_id.clone());
    let output_width = cli.output_width.unwrap_or(file.output_width);
    let output_height = cli.output_height.unwrap_or(file.output_height);
    let framerate = FractionConfig {
        numerator: cli
            .framerate_numerator
            .unwrap_or(file.framerate.numerator),
        denominator: cli
            .framerate_denominator
            .unwrap_or(file.framerate.denominator),
    };
    let interlace_mode = cli
        .interlace_mode
        .clone()
        .unwrap_or_else(|| file.interlace_mode.clone());
    let colorimetry = cli
        .colorimetry
        .clone()
        .unwrap_or_else(|| file.colorimetry.clone());

    let input_flow_ids: [String; 4] = input_flow_ids
        .try_into()
        .map_err(|_| anyhow::anyhow!("Expected exactly 4 input flow IDs"))?;

    Ok(PipelineConfig {
        mxl_domain,
        input_flow_ids,
        output_flow_id,
        output_width,
        output_height,
        framerate,
        interlace_mode,
        colorimetry,
    })
}

fn run_pipeline(pipeline: gst::Pipeline) -> Result<()> {
    pipeline
        .set_state(gst::State::Playing)
        .context("Failed to set pipeline to Playing")?;
    info!("Pipeline running. Press Ctrl-C to stop.");

    let shutdown = Arc::new(AtomicBool::new(false));
    ctrlc::set_handler({
        let shutdown = Arc::clone(&shutdown);
        move || {
            shutdown.store(true, Ordering::SeqCst);
        }
    })
    .context("Failed to install Ctrl-C handler")?;

    let bus = pipeline.bus().context("Failed to get pipeline bus")?;
    loop {
        if shutdown.load(Ordering::SeqCst) {
            warn!("Shutdown requested.");
            break;
        }
        if let Some(msg) = bus.timed_pop(gst::ClockTime::from_mseconds(200)) {
            use gst::MessageView;
            match msg.view() {
                MessageView::Eos(..) => {
                    info!("Received EOS.");
                    break;
                }
                MessageView::Error(err) => {
                    anyhow::bail!(
                        "GStreamer error from {:?}: {} ({:?})",
                        err.src().map(|s| s.path_string()),
                        err.error(),
                        err.debug()
                    );
                }
                _ => {}
            }
        }
    }

    pipeline
        .set_state(gst::State::Null)
        .context("Failed to set pipeline to Null")?;
    Ok(())
}
