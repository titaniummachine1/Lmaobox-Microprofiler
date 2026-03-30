use std::collections::hash_map::DefaultHasher;
use std::collections::HashMap;
use std::env;
use std::ffi::OsStr;
use std::fs::File;
use std::hash::{Hash, Hasher};
use std::io::{self, ErrorKind, Read};
use std::mem::size_of;
use std::os::windows::ffi::OsStrExt;
use std::os::windows::io::FromRawHandle;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use byteorder::{LittleEndian, ReadBytesExt};
use winapi::shared::winerror::ERROR_PIPE_CONNECTED;
use winapi::um::errhandlingapi::GetLastError;
use winapi::um::handleapi::{CloseHandle, INVALID_HANDLE_VALUE};
use winapi::um::namedpipeapi::{ConnectNamedPipe, CreateNamedPipeW};
use winapi::um::winbase::{PIPE_ACCESS_INBOUND, PIPE_READMODE_BYTE, PIPE_TYPE_BYTE, PIPE_WAIT};

const PIPE_NAME: &str = r"\\.\pipe\LboxProfiler";
const SESSION_MAGIC: u32 = 0x4C423058;
const SESSION_VERSION: u32 = 1;

#[repr(C, packed)]
struct SessionHeader {
    magic: u32,
    version: u32,
    tsc_freq: u64,
    build_tag: [u8; 32],
}

#[repr(C, packed)]
struct ProfileEvent {
    timestamp: u64,
    event_type: u8,
    func_name: [u8; 63],
}

#[derive(Default)]
struct RunStats {
    orphan_returns: u64,
    hash_collisions: u64,
    unknown_events: u64,
}

struct FlameEntry {
    path_ids: Vec<u32>,
    total_duration_tsc: u64,
}

struct ProfilerState {
    string_to_id: HashMap<String, u32>,
    id_to_string: HashMap<u32, String>,
    next_id: u32,
    shadow_stack: Vec<(u32, u64)>,
    flame_data: HashMap<u64, FlameEntry>,
    stats: RunStats,
}

impl ProfilerState {
    fn new() -> Self {
        Self {
            string_to_id: HashMap::new(),
            id_to_string: HashMap::new(),
            next_id: 0,
            shadow_stack: Vec::with_capacity(1024),
            flame_data: HashMap::new(),
            stats: RunStats::default(),
        }
    }

    fn get_id(&mut self, name: &str) -> u32 {
        if let Some(&id) = self.string_to_id.get(name) {
            return id;
        }

        let id = self.next_id;
        self.next_id = self.next_id.saturating_add(1);
        self.string_to_id.insert(name.to_string(), id);
        self.id_to_string.insert(id, name.to_string());
        id
    }

    fn record_completed_frame(&mut self, leaf_id: u32, duration_tsc: u64) {
        let mut hasher = DefaultHasher::new();
        for &(id, _) in &self.shadow_stack {
            id.hash(&mut hasher);
        }
        leaf_id.hash(&mut hasher);
        let path_hash = hasher.finish();

        if let Some(entry) = self.flame_data.get_mut(&path_hash) {
            let same_path = entry.path_ids.len() == self.shadow_stack.len() + 1
                && entry
                    .path_ids
                    .iter()
                    .zip(
                        self.shadow_stack
                            .iter()
                            .map(|(id, _)| id)
                            .chain(std::iter::once(&leaf_id)),
                    )
                    .all(|(a, b)| a == b);

            if same_path {
                entry.total_duration_tsc = entry.total_duration_tsc.saturating_add(duration_tsc);
            } else {
                self.stats.hash_collisions = self.stats.hash_collisions.saturating_add(1);
            }
            return;
        }

        let path_ids = self
            .shadow_stack
            .iter()
            .map(|(id, _)| *id)
            .chain(std::iter::once(leaf_id))
            .collect();
        self.flame_data.insert(
            path_hash,
            FlameEntry {
                path_ids,
                total_duration_tsc: duration_tsc,
            },
        );
    }
}

fn build_tag(bytes: &[u8]) -> String {
    let end = bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len());
    String::from_utf8_lossy(&bytes[..end]).trim().to_string()
}

fn connect_pipe() -> io::Result<File> {
    let pipe_name: Vec<u16> = OsStr::new(PIPE_NAME)
        .encode_wide()
        .chain(std::iter::once(0))
        .collect();
    let handle = unsafe {
        CreateNamedPipeW(
            pipe_name.as_ptr(),
            PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,
            65_536,
            65_536,
            0,
            std::ptr::null_mut(),
        )
    };

    if handle == INVALID_HANDLE_VALUE {
        return Err(io::Error::last_os_error());
    }

    let connected = unsafe { ConnectNamedPipe(handle, std::ptr::null_mut()) };
    if connected == 0 {
        let last_error = unsafe { GetLastError() };
        if last_error != ERROR_PIPE_CONNECTED {
            unsafe {
                CloseHandle(handle);
            }
            return Err(io::Error::from_raw_os_error(last_error as i32));
        }
    }

    Ok(unsafe { File::from_raw_handle(handle as _) })
}

fn read_header(pipe: &mut File) -> io::Result<(u64, String)> {
    let mut header_buf = [0u8; size_of::<SessionHeader>()];
    pipe.read_exact(&mut header_buf)?;

    let mut reader = &header_buf[..];
    let magic = reader.read_u32::<LittleEndian>()?;
    let version = reader.read_u32::<LittleEndian>()?;
    let tsc_freq = reader.read_u64::<LittleEndian>()?;
    let mut build_tag_buf = [0u8; 32];
    reader.read_exact(&mut build_tag_buf)?;

    if magic != SESSION_MAGIC {
        return Err(io::Error::new(
            ErrorKind::InvalidData,
            format!("invalid session magic: 0x{magic:08X}"),
        ));
    }

    if version != SESSION_VERSION {
        return Err(io::Error::new(
            ErrorKind::InvalidData,
            format!("unsupported session version: {version}"),
        ));
    }

    Ok((tsc_freq, build_tag(&build_tag_buf)))
}

fn read_event(
    pipe: &mut File,
    event_buf: &mut [u8; size_of::<ProfileEvent>()],
) -> io::Result<Option<(u64, u8, String)>> {
    match pipe.read_exact(event_buf) {
        Ok(()) => {
            let mut reader = &event_buf[..];
            let timestamp = reader.read_u64::<LittleEndian>()?;
            let event_type = reader.read_u8()?;

            let mut name_bytes = [0u8; 63];
            reader.read_exact(&mut name_bytes)?;
            let end = name_bytes
                .iter()
                .position(|&b| b == 0)
                .unwrap_or(name_bytes.len());
            let func_name = String::from_utf8_lossy(&name_bytes[..end]).to_string();

            Ok(Some((timestamp, event_type, func_name)))
        }
        Err(err) if err.kind() == ErrorKind::UnexpectedEof => Ok(None),
        Err(err) => Err(err),
    }
}

fn serialize_flamegraph(state: ProfilerState, tsc_freq: u64) -> io::Result<()> {
    let ProfilerState {
        id_to_string,
        flame_data,
        stats,
        ..
    } = state;

    let mut rows: Vec<(String, u64)> = flame_data
        .into_values()
        .map(|entry| {
            let path = entry
                .path_ids
                .iter()
                .filter_map(|id| id_to_string.get(id))
                .map(String::as_str)
                .collect::<Vec<_>>()
                .join(";");

            let micros = ((entry.total_duration_tsc as f64) / (tsc_freq as f64 / 1_000_000.0)) as u64;
            (path, micros)
        })
        .collect();

    rows.sort_by(|a, b| b.1.cmp(&a.1).then_with(|| a.0.cmp(&b.0)));

    let mut output = String::new();
    for (path, micros) in rows {
        output.push_str(&path);
        output.push(' ');
        output.push_str(&micros.to_string());
        output.push('\n');
    }

    std::fs::write("lbox_flamegraph_data.txt", output)?;

    if stats.orphan_returns > 0
        || stats.hash_collisions > 0
        || stats.unknown_events > 0
    {
        eprintln!(
            "Warnings: orphan_returns={}, hash_collisions={}, unknown_events={}",
            stats.orphan_returns,
            stats.hash_collisions,
            stats.unknown_events
        );
    }

    Ok(())
}

fn inferno_candidates() -> Vec<PathBuf> {
    let mut out = Vec::new();
    out.push(PathBuf::from("inferno-flamegraph.exe"));
    out.push(PathBuf::from("inferno-flamegraph"));

    if let Ok(user_profile) = env::var("USERPROFILE") {
        out.push(Path::new(&user_profile).join(".cargo\\bin\\inferno-flamegraph.exe"));
    }

    out
}

fn render_svg_if_possible(input_path: &Path, output_path: &Path) -> io::Result<bool> {
    for candidate in inferno_candidates() {
        let output_file = match File::create(output_path) {
            Ok(file) => file,
            Err(err) => return Err(err),
        };

        let status = Command::new(&candidate)
            .arg(input_path)
            .stdout(Stdio::from(output_file))
            .stderr(Stdio::inherit())
            .status();

        match status {
            Ok(s) if s.success() => return Ok(true),
            Ok(_) => continue,
            Err(_) => continue,
        }
    }

    Ok(false)
}

fn main() -> io::Result<()> {
    println!("Starting Lbox MicroProfiler Server...");
    println!("Waiting for profiler DLL to connect on {PIPE_NAME}...");

    let mut pipe = connect_pipe()?;
    let (tsc_freq, build_tag) = read_header(&mut pipe)?;
    println!("Connected. Build tag: {build_tag}, TSC frequency: {tsc_freq} Hz");

    let mut state = ProfilerState::new();
    let mut event_buf = [0u8; size_of::<ProfileEvent>()];

    while let Some((timestamp, event_type, func_name)) = read_event(&mut pipe, &mut event_buf)? {
        match event_type {
            0 => {
                let func_name = if func_name.is_empty() {
                    "?".to_string()
                } else {
                    func_name
                };
                let func_id = state.get_id(&func_name);
                state.shadow_stack.push((func_id, timestamp));
            }
            1 => {
                if let Some((popped_id, start_tsc)) = state.shadow_stack.pop() {
                    let duration = timestamp.saturating_sub(start_tsc);
                    state.record_completed_frame(popped_id, duration);
                } else {
                    state.stats.orphan_returns = state.stats.orphan_returns.saturating_add(1);
                }
            }
            _ => {
                state.stats.unknown_events = state.stats.unknown_events.saturating_add(1);
            }
        }
    }

    println!("Stream ended. Writing lbox_flamegraph_data.txt...");
    serialize_flamegraph(state, tsc_freq)?;
    println!("Saved flamegraph input.");

    let folded = Path::new("lbox_flamegraph_data.txt");
    let svg = Path::new("final_profile.svg");
    match render_svg_if_possible(folded, svg)? {
        true => println!("Rendered final_profile.svg automatically."),
        false => println!(
            "Inferno not found. Run `inferno-flamegraph < lbox_flamegraph_data.txt > final_profile.svg`."
        ),
    }

    Ok(())
}
