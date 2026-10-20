//! Pipe shell used when Windows has no ConPTY.
//!
//! The child is a normal `CreateProcess` with piped stdio. That runs `cmd`.
//! It is not a console: there is no resize, no TTY, and full-screen console
//! programs do not work. Do not pretend otherwise.
//!
//! The child's handles come from `Stdio::piped()`, which is the overlapped
//! named pipe std already falls back to on XP. `CreatePipe` is synchronous and
//! cannot feed those reads. The session has one reader, so stdout and stderr
//! are copied onto a `CreatePipe` pair that the blocking output loop reads
//! with `ReadFile`.

use std::ffi::OsString;
use std::io::{self, Read, Write};
use std::os::windows::process::CommandExt;
use std::process::{Command, Stdio};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use portable_pty::{Child, CommandBuilder};

pub(crate) struct PipeEnds {
    pub child: Box<dyn Child + Send + Sync>,
    pub reader: Box<dyn Read + Send>,
    pub writer: Box<dyn Write + Send>,
}

/// `CreatePseudoConsole` is Windows 10 1809. XP fails this check. Calling
/// `openpty` anyway panics inside portable-pty instead of returning an error.
pub(crate) fn conpty_available() -> bool {
    unsafe extern "system" {
        fn GetModuleHandleA(lp_module_name: *const u8) -> *mut core::ffi::c_void;
        fn GetProcAddress(
            h_module: *mut core::ffi::c_void,
            lp_proc_name: *const u8,
        ) -> *mut core::ffi::c_void;
    }

    // SAFETY: kernel32 is loaded in every Windows process. Both names are
    // NUL-terminated. A null module or proc means the export is absent.
    unsafe {
        let module = GetModuleHandleA(b"kernel32.dll\0".as_ptr());
        if module.is_null() {
            return false;
        }
        !GetProcAddress(module, b"CreatePseudoConsole\0".as_ptr()).is_null()
    }
}

pub(crate) fn spawn_shell(cmd: CommandBuilder) -> io::Result<PipeEnds> {
    let argv = cmd.get_argv();
    let Some((program, args)) = argv.split_first() else {
        return Err(io::Error::other("pipe shell has no program"));
    };

    let mut command = Command::new(program);
    command.args(args);
    if let Some(dir) = cmd.get_cwd() {
        command.current_dir(dir);
    }
    // Match the ConPTY spawn, which passes the builder's environment block
    // rather than only the extras. The process env is the base of that block.
    command.env_clear();
    for (key, value) in cmd.iter_full_env_as_str() {
        command.env(key, value);
    }
    command
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    // A new group keeps a console Ctrl+C from reaching the pager. It does not
    // create a console.
    command.creation_flags(windows::Win32::System::Threading::CREATE_NEW_PROCESS_GROUP.0);

    // The caller enrolls the pid. The probe enrolls and waits.
    #[allow(clippy::disallowed_methods)]
    let mut child = command.spawn()?;
    let stdin = child
        .stdin
        .take()
        .ok_or_else(|| io::Error::other("pipe shell stdin missing"))?;
    let stdout = child
        .stdout
        .take()
        .ok_or_else(|| io::Error::other("pipe shell stdout missing"))?;
    let stderr = child
        .stderr
        .take()
        .ok_or_else(|| io::Error::other("pipe shell stderr missing"))?;

    let (merged_read, merged_write) = match io::pipe() {
        Ok(ends) => ends,
        Err(error) => {
            let _ = child.kill();
            let _ = child.wait();
            return Err(error);
        }
    };
    let shared = Arc::new(Mutex::new(merged_write));
    if let Err(error) = start_pump(stdout, Arc::clone(&shared)) {
        let _ = child.kill();
        let _ = child.wait();
        return Err(error);
    }
    if let Err(error) = start_pump(stderr, shared) {
        let _ = child.kill();
        let _ = child.wait();
        return Err(error);
    }

    Ok(PipeEnds {
        child: Box::new(child),
        reader: Box::new(merged_read),
        writer: Box::new(stdin),
    })
}

fn start_pump<R>(reader: R, writer: Arc<Mutex<io::PipeWriter>>) -> io::Result<()>
where
    R: Read + Send + 'static,
{
    std::thread::Builder::new()
        .name("pipe-pty".into())
        .spawn(move || pump(reader, writer))
        .map(|_| ())
        .map_err(|error| io::Error::other(format!("pipe shell reader: {error}")))
}

fn pump(mut reader: impl Read, writer: Arc<Mutex<io::PipeWriter>>) {
    let mut buf = [0u8; 4096];
    loop {
        let n = match reader.read(&mut buf) {
            Ok(0) | Err(_) => break,
            Ok(n) => n,
        };
        let Some(chunk) = buf.get(..n) else {
            break;
        };
        let mut guard = match writer.lock() {
            Ok(guard) => guard,
            Err(poisoned) => poisoned.into_inner(),
        };
        // `flush` on a pipe can fail with ERROR_INVALID_FUNCTION. The write
        // itself is already in the pipe buffer.
        if guard.write_all(chunk).is_err() {
            break;
        }
    }
}

/// Interactive `cmd` on the pipe: write `echo xp-shell`, then `exit`.
///
/// Enrollment happens while `cmd` is blocked on stdin, so a fast `/c` command
/// cannot exit before the job object is assigned.
pub(crate) fn run_probe() -> io::Result<Vec<u8>> {
    let program = std::env::var_os("ComSpec").unwrap_or_else(|| OsString::from("cmd.exe"));
    let cmd = CommandBuilder::new(program);
    let mut ends = spawn_shell(cmd)?;
    let Some(pid) = ends.child.process_id() else {
        let _ = ends.child.kill();
        let _ = ends.child.wait();
        return Err(io::Error::other("pipe shell pid missing"));
    };
    let group = match xai_tty_utils::global_process_scope().enroll_terminal_pid(pid) {
        Ok(group) => group,
        Err(error) => {
            let _ = ends.child.kill();
            let _ = ends.child.wait();
            return Err(io::Error::other(format!("enroll shell: {error}")));
        }
    };

    let mut reader = ends.reader;
    let (tx, rx) = std::sync::mpsc::channel();
    std::thread::Builder::new()
        .name("pipe-pty-probe".into())
        .spawn(move || {
            let mut buf = Vec::new();
            let _ = reader.read_to_end(&mut buf);
            let _ = tx.send(buf);
        })
        .map_err(|error| io::Error::other(format!("probe reader: {error}")))?;

    ends.writer.write_all(b"echo xp-shell\r\nexit\r\n")?;
    let _ = ends.writer.flush();
    drop(ends.writer);

    let output = match rx.recv_timeout(Duration::from_secs(8)) {
        Ok(output) => output,
        Err(_) => {
            let _ = group.kill();
            let _ = ends.child.kill();
            let _ = ends.child.wait();
            return Err(io::Error::other("pipe shell probe timed out"));
        }
    };

    if ends.child.try_wait()?.is_none() {
        let _ = group.kill();
    }
    let _ = ends.child.wait();
    drop(group);

    if !output.windows(b"xp-shell".len()).any(|window| window == b"xp-shell") {
        return Err(io::Error::other(format!(
            "output missing xp-shell: {}",
            String::from_utf8_lossy(&output)
        )));
    }
    Ok(output)
}
