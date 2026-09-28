use rars::{
    Archive, ArchiveMember, ArchiveReadOptions, ArchiveReader, AttrSource, ErrorKind,
    ExtractionDecision, ReadCancellation,
};
use std::ffi::{c_char, c_void};
use std::io::{self, Write};
#[cfg(unix)]
use std::os::unix::ffi::OsStrExt;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::Path;
use std::ptr;
use std::slice;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use zeroize::Zeroize;

const OK: u32 = 0;
const PASSWORD_REQUIRED: u32 = 1;
const INVALID_PASSWORD: u32 = 2;
const CANCELLED: u32 = 3;
const UNSUPPORTED: u32 = 4;
const LIMIT_EXCEEDED: u32 = 5;
const INVALID_ARCHIVE: u32 = 6;
const IO_ERROR: u32 = 7;
const OUTPUT_ERROR: u32 = 8;
const INTERNAL_ERROR: u32 = 9;
const BUFFER_TOO_SMALL: u32 = 10;
const BAD_ARGUMENT: u32 = 11;
const MAXIMUM_ENTRIES: usize = 100_000;
const MAXIMUM_HEADER_BYTES: u64 = 64 * 1024 * 1024;

type ContinueCallback = unsafe extern "C" fn(*mut c_void) -> i32;
type WriteCallback = unsafe extern "C" fn(*const u8, u64, *mut c_void) -> i64;

#[derive(Clone)]
struct Entry {
    name: Vec<u8>,
    size: u64,
    modified: i64,
    encrypted: bool,
    directory: bool,
    special: bool,
    split: bool,
}

struct ArchiveHandle {
    archive: Archive,
    entries: Vec<Entry>,
    header_encrypted: bool,
    contains_encrypted: bool,
}

struct Secret(Option<Vec<u8>>);

impl Secret {
    fn as_deref(&self) -> Option<&[u8]> {
        self.0.as_deref()
    }
}

impl Drop for Secret {
    fn drop(&mut self) {
        if let Some(password) = &mut self.0 {
            password.zeroize();
        }
    }
}

struct CancellationScope {
    token: ReadCancellation,
    finished: Arc<AtomicBool>,
    monitor: Option<JoinHandle<()>>,
}

impl CancellationScope {
    fn new(callback: Option<ContinueCallback>, context: *mut c_void) -> Self {
        let token = ReadCancellation::new();
        let finished = Arc::new(AtomicBool::new(false));
        let monitor = callback.map(|callback| {
            let token = token.clone();
            let finished = Arc::clone(&finished);
            let context_address = context as usize;
            thread::spawn(move || {
                while !finished.load(Ordering::Acquire) {
                    let keep_going = unsafe { callback(context_address as *mut c_void) } != 0;
                    if !keep_going {
                        token.cancel();
                        return;
                    }
                    thread::sleep(Duration::from_millis(5));
                }
            })
        });
        Self {
            token,
            finished,
            monitor,
        }
    }
}

impl Drop for CancellationScope {
    fn drop(&mut self) {
        self.finished.store(true, Ordering::Release);
        if let Some(monitor) = self.monitor.take() {
            let _ = monitor.join();
        }
    }
}

fn write_error(buffer: *mut c_char, capacity: u64, message: &str) {
    if buffer.is_null() || capacity == 0 {
        return;
    }
    let Ok(capacity) = usize::try_from(capacity) else {
        return;
    };
    if capacity == 0 {
        return;
    }
    let count = message.len().min(capacity - 1);
    unsafe {
        ptr::copy_nonoverlapping(message.as_ptr(), buffer.cast::<u8>(), count);
        *buffer.add(count) = 0;
    }
}

fn ffi_guard<F>(error: *mut c_char, error_capacity: u64, operation: F) -> u32
where
    F: FnOnce() -> u32,
{
    match catch_unwind(AssertUnwindSafe(operation)) {
        Ok(status) => status,
        Err(_) => {
            write_error(
                error,
                error_capacity,
                "RAR backend encountered an internal panic",
            );
            INTERNAL_ERROR
        }
    }
}

unsafe fn input_bytes<'a>(data: *const u8, length: u64) -> Result<&'a [u8], u32> {
    let length = usize::try_from(length).map_err(|_| BAD_ARGUMENT)?;
    if length == 0 {
        return Ok(&[]);
    }
    if data.is_null() || length > isize::MAX as usize {
        return Err(BAD_ARGUMENT);
    }
    Ok(slice::from_raw_parts(data, length))
}

fn map_error(error: &rars::Error, has_password: bool) -> u32 {
    match error.kind() {
        ErrorKind::PasswordRequired => PASSWORD_REQUIRED,
        ErrorKind::BadPassword if has_password => INVALID_PASSWORD,
        ErrorKind::BadPassword => PASSWORD_REQUIRED,
        // Legacy encrypted headers have no independent password-check field. A
        // wrong AES key therefore surfaces as an invalid decrypted header.
        ErrorKind::InvalidArchive if has_password => INVALID_PASSWORD,
        ErrorKind::Cancelled => CANCELLED,
        ErrorKind::UnsupportedFeature | ErrorKind::UnsupportedFormat => UNSUPPORTED,
        ErrorKind::ResourceLimit => LIMIT_EXCEEDED,
        ErrorKind::Io => IO_ERROR,
        _ => INVALID_ARCHIVE,
    }
}

fn header_encrypted(archive: &Archive) -> bool {
    archive
        .as_rar15_40()
        .map(|value| value.main.has_encrypted_headers())
        .or_else(|| archive.as_rar50().map(|value| value.main.encrypted_headers))
        .unwrap_or(false)
}

fn timestamp_seconds(time: Option<SystemTime>) -> i64 {
    match time {
        None => 0,
        Some(time) => match time.duration_since(UNIX_EPOCH) {
            Ok(duration) => duration.as_secs().min(i64::MAX as u64) as i64,
            Err(error) => -(error.duration().as_secs().min(i64::MAX as u64) as i64),
        },
    }
}

fn is_special(member: &ArchiveMember) -> bool {
    let metadata = &member.meta;
    let kind = metadata.file_attr & 0o170000;
    metadata.is_redirection
        || (metadata.is_directory && metadata.unpacked_size != 0)
        || (metadata.attr_source() == AttrSource::Unix
            && !matches!(kind, 0 | 0o100000)
            && !(metadata.is_directory && kind == 0o040000))
        || (metadata.attr_source() == AttrSource::Dos && metadata.file_attr & 0x400 != 0)
}

fn split_member(member: &ArchiveMember) -> bool {
    member.meta.is_split_before || member.meta.is_split_after
}

fn collect_entries(archive: &Archive) -> Result<Vec<Entry>, u32> {
    let mut entries = Vec::new();
    for member in archive.members() {
        if entries.len() >= MAXIMUM_ENTRIES {
            return Err(LIMIT_EXCEEDED);
        }
        let metadata = &member.meta;
        entries.push(Entry {
            name: metadata.name.clone(),
            size: metadata.unpacked_size,
            modified: timestamp_seconds(metadata.modification_time()),
            encrypted: metadata.is_encrypted,
            directory: metadata.is_directory,
            special: is_special(&member),
            split: split_member(&member),
        });
    }
    Ok(entries)
}

fn read_archive(
    path: &[u8],
    password: Option<&[u8]>,
    cancellation: &ReadCancellation,
) -> Result<ArchiveHandle, (u32, String, bool)> {
    #[cfg(unix)]
    let path = Path::new(std::ffi::OsStr::from_bytes(path));
    #[cfg(not(unix))]
    let path = Path::new(std::str::from_utf8(path).map_err(|_| {
        (
            BAD_ARGUMENT,
            "archive path is not valid UTF-8".to_string(),
            false,
        )
    })?);

    let mut options = ArchiveReadOptions::new();
    options.password = password;
    options.cancellation = Some(cancellation);
    options.max_header_count = Some((MAXIMUM_ENTRIES as u64) * 2 + 1);
    options.max_header_bytes = Some(MAXIMUM_HEADER_BYTES);
    let archive = ArchiveReader::read_path_with_options(path, options).map_err(|error| {
        let status = map_error(&error, password.is_some());
        let hidden = status == PASSWORD_REQUIRED && password.is_none();
        (status, error.to_string(), hidden)
    })?;
    let header_encrypted = header_encrypted(&archive);
    let entries = collect_entries(&archive).map_err(|status| {
        (
            status,
            "RAR archive exceeds the 100000-entry limit".to_string(),
            false,
        )
    })?;
    let contains_encrypted = header_encrypted || entries.iter().any(|entry| entry.encrypted);
    Ok(ArchiveHandle {
        archive,
        entries,
        header_encrypted,
        contains_encrypted,
    })
}

fn archive_is_solid(archive: &Archive) -> bool {
    match archive {
        Archive::Rar13(value) => value.main.is_solid(),
        Archive::Rar15To40(value) => {
            value.main.is_solid() || value.files().any(|file| file.is_solid())
        }
        Archive::Rar50Plus(value) => {
            value.main.is_solid() || value.files().any(|file| file.compression_info & 0x40 != 0)
        }
        _ => false,
    }
}

struct CallbackWriter {
    callback: WriteCallback,
    context: *mut c_void,
    cancellation: ReadCancellation,
    maximum: u64,
    written: Arc<AtomicU64>,
    failure: Arc<AtomicU32>,
}

impl Write for CallbackWriter {
    fn write(&mut self, buffer: &[u8]) -> io::Result<usize> {
        if self.cancellation.is_cancelled() {
            return Err(io::Error::new(
                io::ErrorKind::Interrupted,
                "RAR extraction cancelled",
            ));
        }
        let length = u64::try_from(buffer.len()).map_err(|_| {
            io::Error::new(io::ErrorKind::InvalidInput, "RAR output chunk is too large")
        })?;
        let written = self.written.load(Ordering::Relaxed);
        if written > self.maximum || length > self.maximum - written {
            self.failure.store(LIMIT_EXCEEDED, Ordering::Relaxed);
            return Err(io::Error::new(
                io::ErrorKind::Other,
                "RAR member exceeds output limit",
            ));
        }
        let accepted = unsafe { (self.callback)(buffer.as_ptr(), length, self.context) };
        if accepted < 0 || accepted as u64 > length {
            self.failure.store(OUTPUT_ERROR, Ordering::Relaxed);
            return Err(io::Error::new(
                io::ErrorKind::Other,
                "RAR output callback failed",
            ));
        }
        let accepted = accepted as usize;
        if accepted == 0 && !buffer.is_empty() {
            self.failure.store(OUTPUT_ERROR, Ordering::Relaxed);
            return Err(io::Error::new(
                io::ErrorKind::WriteZero,
                "RAR output callback wrote no data",
            ));
        }
        self.written.fetch_add(accepted as u64, Ordering::Relaxed);
        Ok(accepted)
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

fn copy_password(password: *const u8, length: u64, present: u8) -> Result<Secret, u32> {
    if present == 0 {
        return Ok(Secret(None));
    }
    let bytes = unsafe { input_bytes(password, length)? };
    Ok(Secret(Some(bytes.to_vec())))
}

#[no_mangle]
pub unsafe extern "C" fn jv_rar_open(
    path: *const u8,
    path_length: u64,
    password: *const u8,
    password_length: u64,
    password_present: u8,
    should_continue: Option<ContinueCallback>,
    continue_context: *mut c_void,
    out_handle: *mut *mut c_void,
    out_header_encrypted: *mut u8,
    out_contains_encrypted: *mut u8,
    error: *mut c_char,
    error_capacity: u64,
) -> u32 {
    ffi_guard(error, error_capacity, || {
        write_error(error, error_capacity, "");
        if out_handle.is_null()
            || out_header_encrypted.is_null()
            || out_contains_encrypted.is_null()
        {
            write_error(error, error_capacity, "invalid RAR backend output pointer");
            return BAD_ARGUMENT;
        }
        *out_handle = ptr::null_mut();
        *out_header_encrypted = 0;
        *out_contains_encrypted = 0;
        let path = match unsafe { input_bytes(path, path_length) } {
            Ok(value) if !value.is_empty() => value,
            _ => {
                write_error(error, error_capacity, "archive path is empty or invalid");
                return BAD_ARGUMENT;
            }
        };
        let password = match copy_password(password, password_length, password_present) {
            Ok(value) => value,
            Err(status) => {
                write_error(error, error_capacity, "archive password buffer is invalid");
                return status;
            }
        };
        let cancellation = CancellationScope::new(should_continue, continue_context);
        match read_archive(path, password.as_deref(), &cancellation.token) {
            Ok(handle) => {
                *out_header_encrypted = u8::from(handle.header_encrypted);
                *out_contains_encrypted = u8::from(handle.contains_encrypted);
                *out_handle = Box::into_raw(Box::new(handle)).cast::<c_void>();
                OK
            }
            Err((status, message, hidden)) => {
                *out_header_encrypted = u8::from(hidden);
                *out_contains_encrypted = u8::from(hidden);
                write_error(error, error_capacity, &message);
                status
            }
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jv_rar_entry_count(
    handle: *const c_void,
    out_count: *mut u64,
    error: *mut c_char,
    error_capacity: u64,
) -> u32 {
    ffi_guard(error, error_capacity, || {
        if handle.is_null() || out_count.is_null() {
            write_error(error, error_capacity, "invalid RAR archive handle");
            return BAD_ARGUMENT;
        }
        *out_count = (&*handle.cast::<ArchiveHandle>()).entries.len() as u64;
        OK
    })
}

#[no_mangle]
pub unsafe extern "C" fn jv_rar_entry_info(
    handle: *const c_void,
    index: u64,
    name: *mut u8,
    name_capacity: u64,
    out_name_length: *mut u64,
    out_size: *mut u64,
    out_modified: *mut i64,
    out_flags: *mut u32,
    error: *mut c_char,
    error_capacity: u64,
) -> u32 {
    ffi_guard(error, error_capacity, || {
        if handle.is_null()
            || out_name_length.is_null()
            || out_size.is_null()
            || out_modified.is_null()
            || out_flags.is_null()
        {
            write_error(error, error_capacity, "invalid RAR entry output pointer");
            return BAD_ARGUMENT;
        }
        let archive = &*handle.cast::<ArchiveHandle>();
        let Ok(index) = usize::try_from(index) else {
            return BAD_ARGUMENT;
        };
        let Some(entry) = archive.entries.get(index) else {
            write_error(error, error_capacity, "RAR member index is out of range");
            return BAD_ARGUMENT;
        };
        let Ok(name_capacity) = usize::try_from(name_capacity) else {
            return BAD_ARGUMENT;
        };
        *out_name_length = entry.name.len() as u64;
        *out_size = entry.size;
        *out_modified = entry.modified;
        *out_flags = u32::from(entry.directory)
            | (u32::from(entry.encrypted) << 1)
            | (u32::from(entry.special) << 2)
            | (u32::from(entry.split) << 3);
        if entry.name.len() > name_capacity {
            return BUFFER_TOO_SMALL;
        }
        if !entry.name.is_empty() {
            if name.is_null() {
                write_error(error, error_capacity, "RAR member name buffer is null");
                return BAD_ARGUMENT;
            }
            ptr::copy_nonoverlapping(entry.name.as_ptr(), name, entry.name.len());
        }
        OK
    })
}

#[no_mangle]
pub unsafe extern "C" fn jv_rar_extract_member(
    handle: *const c_void,
    index: u64,
    expected_name: *const u8,
    expected_name_length: u64,
    password: *const u8,
    password_length: u64,
    password_present: u8,
    maximum_output: u64,
    should_continue: Option<ContinueCallback>,
    continue_context: *mut c_void,
    writer: Option<WriteCallback>,
    writer_context: *mut c_void,
    out_written: *mut u64,
    error: *mut c_char,
    error_capacity: u64,
) -> u32 {
    ffi_guard(error, error_capacity, || {
        write_error(error, error_capacity, "");
        if handle.is_null() || out_written.is_null() {
            write_error(error, error_capacity, "invalid RAR extraction handle");
            return BAD_ARGUMENT;
        }
        *out_written = 0;
        let Some(writer) = writer else {
            write_error(error, error_capacity, "RAR output callback is missing");
            return BAD_ARGUMENT;
        };
        let expected_name = match unsafe { input_bytes(expected_name, expected_name_length) } {
            Ok(value) => value,
            Err(status) => {
                write_error(error, error_capacity, "RAR member name is invalid");
                return status;
            }
        };
        let password = match copy_password(password, password_length, password_present) {
            Ok(value) => value,
            Err(status) => {
                write_error(error, error_capacity, "archive password buffer is invalid");
                return status;
            }
        };
        let archive = &*handle.cast::<ArchiveHandle>();
        let Ok(index) = usize::try_from(index) else {
            return BAD_ARGUMENT;
        };
        let Some(selected) = archive.entries.get(index) else {
            write_error(error, error_capacity, "RAR member no longer exists");
            return INVALID_ARCHIVE;
        };
        if selected.name != expected_name
            || selected.directory
            || selected.special
            || selected.split
        {
            write_error(
                error,
                error_capacity,
                "RAR member changed since it was listed or is unsupported",
            );
            return INVALID_ARCHIVE;
        }
        if selected.size > maximum_output {
            write_error(
                error,
                error_capacity,
                "RAR member exceeds the configured output limit",
            );
            return LIMIT_EXCEEDED;
        }
        let cancellation = CancellationScope::new(should_continue, continue_context);
        let solid = archive_is_solid(&archive.archive);
        let target_index = index as u64;
        let mut ordinal = 0u64;
        let written = Arc::new(AtomicU64::new(0));
        let failure = Arc::new(AtomicU32::new(OK));
        let target_seen = std::cell::Cell::new(false);
        let mut options = ArchiveReadOptions::new();
        options.password = password.as_deref();
        options.cancellation = Some(&cancellation.token);
        options.max_member_output_bytes = Some(maximum_output);
        match archive.archive.extract_with_control(options, |member| {
            let current = ordinal;
            ordinal = ordinal.saturating_add(1);
            if current < target_index {
                return Ok(if solid {
                    ExtractionDecision::Extract(Box::new(io::sink()))
                } else {
                    ExtractionDecision::Skip
                });
            }
            if current > target_index {
                return Ok(ExtractionDecision::Stop);
            }
            if member.meta.name != selected.name
                || member.meta.unpacked_size != selected.size
                || member.meta.is_directory
                || is_special(member)
                || split_member(member)
            {
                return Err(rars::Error::SourceChanged("RAR member identity changed"));
            }
            target_seen.set(true);
            Ok(ExtractionDecision::Extract(Box::new(CallbackWriter {
                callback: writer,
                context: writer_context,
                cancellation: cancellation.token.clone(),
                maximum: maximum_output,
                written: Arc::clone(&written),
                failure: Arc::clone(&failure),
            })))
        }) {
            Ok(_) if target_seen.get() => {
                *out_written = written.load(Ordering::Relaxed);
                OK
            }
            Ok(_) => {
                write_error(error, error_capacity, "RAR member was not extracted");
                INVALID_ARCHIVE
            }
            Err(cause) => {
                let output_status = failure.load(Ordering::Relaxed);
                let status = if output_status == OK {
                    map_error(&cause, password.as_deref().is_some())
                } else {
                    output_status
                };
                let detail = match output_status {
                    LIMIT_EXCEEDED => "RAR member exceeds the configured output limit".to_string(),
                    OUTPUT_ERROR => "cannot write extracted RAR image".to_string(),
                    _ => cause.to_string(),
                };
                write_error(error, error_capacity, &detail);
                status
            }
        }
    })
}

#[no_mangle]
pub unsafe extern "C" fn jv_rar_free(handle: *mut c_void) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if !handle.is_null() {
            drop(Box::from_raw(handle.cast::<ArchiveHandle>()));
        }
    }));
}
