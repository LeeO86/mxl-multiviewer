// SPDX-FileCopyrightText: 2026 Contributors to the MXL Multiviewer project.
// SPDX-License-Identifier: Apache-2.0

use anyhow::{Context, Result};
use gstreamer as gst;
use gst::prelude::*;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FractionConfig {
    pub numerator: i32,
    pub denominator: i32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PipelineConfig {
    pub mxl_domain: String,
    pub input_flow_ids: [String; 4],
    pub output_flow_id: String,
    pub output_width: i32,
    pub output_height: i32,
    pub framerate: FractionConfig,
    pub interlace_mode: String,
    pub colorimetry: String,
}

impl PipelineConfig {
    pub fn tile_dimensions(&self) -> Result<(i32, i32)> {
        if self.output_width % 2 != 0 || self.output_height % 2 != 0 {
            anyhow::bail!("Output width/height must be even for a 2x2 layout.");
        }
        Ok((self.output_width / 2, self.output_height / 2))
    }
}

pub fn build_pipeline(config: &PipelineConfig) -> Result<gst::Pipeline> {
    ensure_mxl_elements_available()?;

    let pipeline = gst::Pipeline::new();

    let compositor = gst::ElementFactory::make("compositor")
        .build()
        .context("Failed to create compositor element")?;

    pipeline
        .add(&compositor)
        .context("Failed to add compositor to pipeline")?;

    let (tile_width, tile_height) = config.tile_dimensions()?;
    let positions = [
        (0, 0),
        (tile_width, 0),
        (0, tile_height),
        (tile_width, tile_height),
    ];

    for (index, flow_id) in config.input_flow_ids.iter().enumerate() {
        let (xpos, ypos) = positions[index];
        add_input_branch(
            &pipeline,
            &compositor,
            &config.mxl_domain,
            flow_id,
            tile_width,
            tile_height,
            xpos,
            ypos,
        )?;
    }

    let output_convert = gst::ElementFactory::make("videoconvert")
        .build()
        .context("Failed to create videoconvert (output)")?;
    let output_capsfilter = gst::ElementFactory::make("capsfilter")
        .build()
        .context("Failed to create capsfilter (output)")?;
    let output_sink = gst::ElementFactory::make("mxlsink")
        .build()
        .context("Failed to create mxlsink")?;

    output_sink.set_property("flow-id", &config.output_flow_id);
    output_sink.set_property("domain", &config.mxl_domain);

    let output_caps = gst::Caps::builder("video/x-raw")
        .field("format", "v210")
        .field("width", config.output_width)
        .field("height", config.output_height)
        .field(
            "framerate",
            gst::Fraction::new(config.framerate.numerator, config.framerate.denominator),
        )
        .field("interlace-mode", config.interlace_mode.as_str())
        .field("colorimetry", config.colorimetry.as_str())
        .build();
    output_capsfilter.set_property("caps", output_caps);

    pipeline
        .add_many([&output_convert, &output_capsfilter, &output_sink])
        .context("Failed to add output elements")?;

    compositor
        .link(&output_convert)
        .context("Failed to link compositor to videoconvert")?;
    gst::Element::link_many([&output_convert, &output_capsfilter, &output_sink])
        .context("Failed to link output elements")?;

    Ok(pipeline)
}

fn ensure_mxl_elements_available() -> Result<()> {
    if gst::ElementFactory::find("mxlsrc").is_none() {
        anyhow::bail!("GStreamer element 'mxlsrc' not found (check GST_PLUGIN_PATH).");
    }
    if gst::ElementFactory::find("mxlsink").is_none() {
        anyhow::bail!("GStreamer element 'mxlsink' not found (check GST_PLUGIN_PATH).");
    }
    Ok(())
}

fn add_input_branch(
    pipeline: &gst::Pipeline,
    compositor: &gst::Element,
    domain: &str,
    flow_id: &str,
    tile_width: i32,
    tile_height: i32,
    xpos: i32,
    ypos: i32,
) -> Result<()> {
    let src = gst::ElementFactory::make("mxlsrc")
        .build()
        .context("Failed to create mxlsrc")?;
    let queue = gst::ElementFactory::make("queue")
        .build()
        .context("Failed to create queue")?;
    let convert = gst::ElementFactory::make("videoconvert")
        .build()
        .context("Failed to create videoconvert")?;
    let scale = gst::ElementFactory::make("videoscale")
        .build()
        .context("Failed to create videoscale")?;
    let capsfilter = gst::ElementFactory::make("capsfilter")
        .build()
        .context("Failed to create capsfilter")?;

    src.set_property("video-flow-id", flow_id);
    src.set_property("domain", domain);
    queue.set_property_from_str("leaky", "downstream");
    queue.set_property("max-size-buffers", 2u32);
    queue.set_property("max-size-time", 0u64);
    queue.set_property("max-size-bytes", 0u32);

    let caps = gst::Caps::builder("video/x-raw")
        .field("format", "I420")
        .field("width", tile_width)
        .field("height", tile_height)
        .build();
    capsfilter.set_property("caps", caps);

    pipeline
        .add_many([&src, &queue, &convert, &scale, &capsfilter])
        .context("Failed to add input elements")?;
    gst::Element::link_many([&src, &queue, &convert, &scale, &capsfilter])
        .context("Failed to link input elements")?;

    let src_pad = capsfilter
        .static_pad("src")
        .context("Failed to get capsfilter src pad")?;
    let sink_pad = compositor
        .request_pad_simple("sink_%u")
        .context("Failed to request compositor sink pad")?;
    sink_pad.set_property("xpos", xpos);
    sink_pad.set_property("ypos", ypos);

    src_pad
        .link(&sink_pad)
        .context("Failed to link branch into compositor")?;

    Ok(())
}
