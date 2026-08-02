use std::env;
use std::ffi::OsString;
use std::path::{Path, PathBuf};
use std::process::Command;

const C_SOURCES: &[&str] = &[
    "src/common.c",
    "src/port.c",
    "src/registry.c",
    "src/router.c",
    "src/ring.c",
    "src/scheduler.c",
    "src/signal.c",
];
const SWITCH_SOURCES: &[&str] = &["src/config.c", "src/switch.c"];
const HEADERS: &[&str] = &[
    "Cargo.toml",
    "include/datarouter.h",
    "include/datarouter_frontend.h",
    "include/datarouter_protocol.h",
    "src/frontend.rs",
    "src/frontend/ffi.rs",
    "src/frontend/inbound.rs",
    "src/frontend/options.rs",
    "src/frontend/outbound.rs",
    "src/frontend/port_frontend.rs",
    "src/frontend/queue.rs",
    "src/protocol.rs",
    "src/protocol_ffi.rs",
    "src/datarouter_internal.h",
    "src/datarouter_registry.h",
    "src/datarouter_router.h",
    "src/datarouter_scheduler.h",
    "src/datarouter_signal.h",
    "src/config.h",
    "src/config.c",
    "src/switch.c",
    "src/frontend/switch_frontend.rs",
];

fn main() {
    for path in C_SOURCES.iter().chain(HEADERS.iter()) {
        println!("cargo:rerun-if-changed={path}");
    }
    println!("cargo:rerun-if-changed=src/platform_linux.c");
    println!("cargo:rerun-if-changed=src/platform_windows.c");
    println!("cargo:rerun-if-env-changed=DATAROUTER_CHANNEL_SIZE");

    let channel_size = datarouter_channel_size();

    let target = env::var("TARGET").expect("Cargo did not set TARGET");
    let out_dir = PathBuf::from(env::var_os("OUT_DIR").expect("Cargo did not set OUT_DIR"));
    let generated = out_dir.join("generated_constants.rs");
    std::fs::write(
        &generated,
        format!("pub const DATAROUTER_CHANNEL_SIZE: u32 = {channel_size}u32;\n"),
    )
    .expect("write generated datarouter constants");

    let is_windows = target.contains("windows");
    let is_msvc = target.contains("msvc");

    let platform_source = if is_windows {
        "src/platform_windows.c"
    } else {
        "src/platform_linux.c"
    };

    let mut sources: Vec<&str> = C_SOURCES.to_vec();
    sources.push(platform_source);

    let objects: Vec<PathBuf> = sources
        .iter()
        .map(|source| compile_c(source, &out_dir, is_windows, is_msvc, channel_size))
        .collect();

    archive_static_library(&objects, &out_dir, is_msvc);

    let switch_bin = build_switch_binary(
        &objects,
        &out_dir,
        is_windows,
        is_msvc,
        channel_size,
        platform_source,
    );
    let generated_switch = out_dir.join("generated_switch_bin.rs");
    let generated_switch_body = match &switch_bin {
        Some(path) => {
            // Embed the switch binary directly into the consuming executable
            // at compile time: `SwitchHandle::start` spawns the switch from
            // these bytes without depending on any resource file on disk.
            let literal = path.display().to_string().replace('\\', "/");
            format!("pub const DATAROUTER_SWITCH_BYTES: &[u8] = include_bytes!(\"{literal}\");\n")
        }
        None => "pub const DATAROUTER_SWITCH_BYTES: &[u8] = &[];\n".to_owned(),
    };
    std::fs::write(&generated_switch, generated_switch_body)
        .expect("write generated switch binary constant");

    println!("cargo:rustc-link-search=native={}", out_dir.display());
    println!("cargo:rustc-link-lib=static=datarouter");
    if is_windows {
        println!("cargo:rustc-link-lib=dylib=kernel32");
    } else {
        println!("cargo:rustc-link-lib=dylib=rt");
    }
}

fn datarouter_channel_size() -> u32 {
    match env::var("DATAROUTER_CHANNEL_SIZE") {
        Ok(raw) => raw
            .parse::<u32>()
            .unwrap_or_else(|err| panic!("invalid DATAROUTER_CHANNEL_SIZE={raw}: {err}")),
        Err(_) => 524288,
    }
}

fn compile_c(
    source: &str,
    out_dir: &Path,
    is_windows: bool,
    is_msvc: bool,
    channel_size: u32,
) -> PathBuf {
    let source_path = Path::new(source);
    let stem = source_path
        .file_stem()
        .expect("source file has no stem")
        .to_string_lossy();
    let object = out_dir.join(if is_msvc {
        format!("{stem}.obj")
    } else {
        format!("{stem}.o")
    });

    if is_msvc {
        let compiler = env::var_os("CC").unwrap_or_else(|| OsString::from("cl"));
        let mut cmd = Command::new(compiler);
        cmd.arg("/nologo")
            .arg("/c")
            .arg("/std:c11")
            .arg("/D_CRT_SECURE_NO_WARNINGS")
            .arg(format!("/DDATAROUTER_CHANNEL_SIZE={channel_size}U"))
            .arg("/Iinclude")
            .arg("/Isrc")
            .arg(format!("/Fo{}", object.display()))
            .arg(source);
        run(cmd, source);
    } else {
        let compiler = env::var_os("CC").unwrap_or_else(|| OsString::from("cc"));
        let mut cmd = Command::new(compiler);
        cmd.arg("-std=c11")
            .arg("-Wall")
            .arg("-Wextra")
            .arg("-Wpedantic")
            .arg(format!("-DDATAROUTER_CHANNEL_SIZE={channel_size}U"))
            .arg("-Iinclude")
            .arg("-Isrc")
            .arg("-c")
            .arg(source)
            .arg("-o")
            .arg(&object);
        if !is_windows {
            cmd.arg("-D_POSIX_C_SOURCE=200809L").arg("-fPIC");
        }
        run(cmd, source);
    }

    object
}

fn build_switch_binary(
    objects: &[PathBuf],
    out_dir: &Path,
    is_windows: bool,
    is_msvc: bool,
    channel_size: u32,
    platform_source: &str,
) -> Option<PathBuf> {
    let mut switch_objects: Vec<PathBuf> = objects.to_vec();
    for source in SWITCH_SOURCES {
        switch_objects.push(compile_c(
            source,
            out_dir,
            is_windows,
            is_msvc,
            channel_size,
        ));
    }
    let binary = out_dir.join(if is_windows {
        "datarouter-switch.exe"
    } else {
        "datarouter-switch"
    });
    let _ = platform_source;
    if is_msvc {
        let linker = env::var_os("LD").unwrap_or_else(|| OsString::from("link"));
        let mut cmd = Command::new(linker);
        cmd.arg("/nologo")
            .arg(format!("/OUT:{}", binary.display()))
            .arg("/SUBSYSTEM:CONSOLE");
        cmd.args(&switch_objects);
        cmd.arg("kernel32.lib").arg("user32.lib");
        if run_optional(cmd, "link datarouter-switch") {
            return Some(binary);
        }
        eprintln!("cargo:warning=datarouter switch binary unavailable on this MSVC toolchain");
        return None;
    }
    let compiler = env::var_os("CC").unwrap_or_else(|| OsString::from("cc"));
    let mut cmd = Command::new(compiler);
    cmd.arg("-std=c11")
        .args(&switch_objects)
        .arg("-o")
        .arg(&binary);
    if is_windows {
        cmd.arg("-lws2_32");
    } else {
        cmd.arg("-pthread");
    }
    if run_optional(cmd, "link datarouter-switch") {
        return Some(binary);
    }
    eprintln!("cargo:warning=datarouter switch binary unavailable on this toolchain");
    None
}

fn run_optional(mut cmd: Command, label: &str) -> bool {
    match cmd.status() {
        Ok(status) if status.success() => true,
        Ok(status) => {
            eprintln!("cargo:warning=linker step failed for {label}: {status}");
            false
        }
        Err(err) => {
            eprintln!("cargo:warning=linker unavailable for {label}: {err}");
            false
        }
    }
}

fn archive_static_library(objects: &[PathBuf], out_dir: &Path, is_msvc: bool) {
    if is_msvc {
        let lib = env::var_os("AR").unwrap_or_else(|| OsString::from("lib"));
        let output = out_dir.join("datarouter.lib");
        let mut cmd = Command::new(lib);
        cmd.arg("/nologo").arg(format!("/OUT:{}", output.display()));
        cmd.args(objects);
        run(cmd, "archive datarouter.lib");
    } else {
        let ar = env::var_os("AR").unwrap_or_else(|| OsString::from("ar"));
        let output = out_dir.join("libdatarouter.a");
        let mut cmd = Command::new(ar);
        cmd.arg("crs").arg(output);
        cmd.args(objects);
        run(cmd, "archive libdatarouter.a");
    }
}

fn run(mut cmd: Command, label: &str) {
    let status = cmd
        .status()
        .unwrap_or_else(|err| panic!("failed to run compiler step for {label}: {err}"));
    if !status.success() {
        panic!("compiler step failed for {label}: {status}");
    }
}
