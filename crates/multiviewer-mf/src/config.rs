// SPDX-FileCopyrightText: 2026 Contributors to the MXL Multiviewer project.
// SPDX-License-Identifier: Apache-2.0

use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FileConfig {
    pub mxl_domain: String,
    pub input_flow_ids: Vec<String>,
    pub output_flow_id: String,
    pub output_width: i32,
    pub output_height: i32,
    pub framerate: FractionConfig,
    pub interlace_mode: String,
    pub colorimetry: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FractionConfig {
    pub numerator: i32,
    pub denominator: i32,
}

pub fn load_config(path: &str) -> Result<FileConfig> {
    let raw = std::fs::read_to_string(path)
        .with_context(|| format!("Failed to read config file: {path}"))?;
    let parsed: FileConfig =
        serde_json::from_str(&raw).context("Failed to parse multiviewer config JSON")?;
    Ok(parsed)
}
