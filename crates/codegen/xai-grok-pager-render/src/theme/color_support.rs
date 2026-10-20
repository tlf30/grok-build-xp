//! Detects the terminal's color capabilities (truecolor / 256 / 16 / none).
//! [`quantize_color`] downgrades a [`ratatui::style::Color`] to the highest level the terminal supports.
//!
//! The detected level is cached in a global [`OnceLock`]; call [`detect`] once at startup, then use [`get`] everywhere else.

use std::sync::OnceLock;

use ratatui::style::Color;

use crate::render::color::{indexed_to_rgb, nearest_indexed};
use crate::terminal::{TerminalName, terminal_context};

/// Terminal color support level (ordered low to high).
#[derive(
    Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, strum::AsRefStr, strum::IntoStaticStr,
)]
pub enum ColorLevel {
    /// No color support (monochrome).
    #[strum(serialize = "none")]
    None,
    /// Basic 16-color ANSI (SGR 30–37 / 90–97).
    #[strum(serialize = "basic")]
    Basic,
    /// 256-color indexed palette (SGR 38;5;N).
    #[strum(serialize = "256")]
    Ansi256,
    /// 24-bit truecolor RGB (SGR 38;2;R;G;B).
    #[strum(serialize = "truecolor")]
    TrueColor,
}

impl ColorLevel {
    pub fn has_color(self) -> bool {
        self >= Self::Basic
    }

    pub fn has_256(self) -> bool {
        self >= Self::Ansi256
    }

    pub fn has_truecolor(self) -> bool {
        self >= Self::TrueColor
    }
}

impl std::fmt::Display for ColorLevel {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.as_ref())
    }
}

// ── Global singleton ─────────────────────────────────────────────────────

static COLOR_LEVEL: OnceLock<ColorLevel> = OnceLock::new();

/// Test override before the write-once `OnceLock`. Ambient `NO_COLOR` would otherwise win by scheduling luck. `u8::MAX` means unset.
#[cfg(any(test, feature = "test-support"))]
static TEST_LEVEL_OVERRIDE: std::sync::atomic::AtomicU8 = std::sync::atomic::AtomicU8::new(u8::MAX);

/// Pin the detected color level for the test process (see
/// [`TEST_LEVEL_OVERRIDE`]). The terminal-native lock cap still applies on
/// top, so minimal-mode tests keep their Basic cap.
#[cfg(any(test, feature = "test-support"))]
pub fn set_level_for_test(level: ColorLevel) {
    TEST_LEVEL_OVERRIDE.store(level as u8, std::sync::atomic::Ordering::Relaxed);
}

#[cfg(any(test, feature = "test-support"))]
fn test_level_override() -> Option<ColorLevel> {
    let v = TEST_LEVEL_OVERRIDE.load(std::sync::atomic::Ordering::Relaxed);
    [
        ColorLevel::None,
        ColorLevel::Basic,
        ColorLevel::Ansi256,
        ColorLevel::TrueColor,
    ]
    .into_iter()
    .find(|l| *l as u8 == v)
}

/// `NO_COLOR` forces [`ColorLevel::None`]. Non-TTY without it defaults to TrueColor (a TUI always runs in a terminal).
/// Capped at [`ColorLevel::Basic`] while the terminal-native lock is engaged.
pub fn detect() -> ColorLevel {
    let raw = detect_raw();
    if crate::theme::cache::terminal_native_locked() {
        return raw.min(ColorLevel::Basic);
    }
    raw
}

/// The raw cached detection, without the terminal-native lock cap.
fn detect_raw() -> ColorLevel {
    #[cfg(any(test, feature = "test-support"))]
    if let Some(level) = test_level_override() {
        return level;
    }
    *COLOR_LEVEL.get_or_init(|| {
        // Explicit opt-out via NO_COLOR takes priority.
        if std::env::var_os("NO_COLOR").is_some() {
            return ColorLevel::None;
        }

        let mut level = match supports_color::on(supports_color::Stream::Stdout) {
            Some(level) => {
                if level.has_16m {
                    ColorLevel::TrueColor
                } else if level.has_256 {
                    ColorLevel::Ansi256
                } else if level.has_basic {
                    ColorLevel::Basic
                } else {
                    ColorLevel::None
                }
            }
            // `None` means stdout is not a TTY (tests, piped)
            None => ColorLevel::TrueColor,
        };

        // The `supports-color` crate relies on COLORTERM=truecolor, but tmux/SSH/mosh often strip that variable
        // When the crate reports only 256-color support, upgrade to TrueColor if we can identify the emulator and know it handles 24-bit RGB
        if level < ColorLevel::TrueColor && terminal_supports_truecolor() {
            level = ColorLevel::TrueColor;
        }

        // XP ConHost rejects ENABLE_VIRTUAL_TERMINAL_PROCESSING. Crossterm then
        // uses Win32 attributes, and unquantized RGB paints as black on black.
        // Basic keeps the theme's ANSI-16 chrome overrides. Pipes and tests
        // have no console, so they keep the level chosen above.
        cap_level_if_vt_rejected(level, windows_console_rejects_vt())
    })
}

/// This never consults stdout, because `grok doctor --json` is commonly piped.
/// Stderr or an independently opened controlling terminal is sufficient evidence that the process is diagnosing that terminal.
/// A fully headless invocation is honest about having no color evidence.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum StandaloneColorEvidence {
    Available(ColorLevel),
    Unavailable,
}

pub fn standalone(terminal: TerminalName) -> StandaloneColorEvidence {
    use std::io::IsTerminal;

    standalone_from_env(
        &crate::host::collect_unicode_env(),
        std::io::stderr().is_terminal(),
        controlling_terminal_available(),
        terminal,
    )
}

fn controlling_terminal_available() -> bool {
    #[cfg(unix)]
    {
        std::fs::OpenOptions::new()
            .read(true)
            .write(true)
            .open("/dev/tty")
            .is_ok()
    }
    #[cfg(windows)]
    {
        std::fs::OpenOptions::new()
            .read(true)
            .write(true)
            .open("CONIN$")
            .is_ok()
    }
    #[cfg(not(any(unix, windows)))]
    {
        false
    }
}

fn standalone_from_env(
    env: &std::collections::HashMap<String, String>,
    stderr_is_terminal: bool,
    controlling_terminal: bool,
    terminal: TerminalName,
) -> StandaloneColorEvidence {
    if env.contains_key("NO_COLOR") {
        return StandaloneColorEvidence::Available(ColorLevel::None);
    }
    if !stderr_is_terminal && !controlling_terminal {
        return StandaloneColorEvidence::Unavailable;
    }
    let colorterm = env.get("COLORTERM").map(|value| value.to_ascii_lowercase());
    if colorterm
        .as_deref()
        .is_some_and(|value| value == "truecolor" || value == "24bit")
        || terminal_supports_truecolor_brand(terminal)
    {
        return StandaloneColorEvidence::Available(ColorLevel::TrueColor);
    }
    let term = env
        .get("TERM")
        .map(|value| value.to_ascii_lowercase())
        .unwrap_or_default();
    if term.contains("256color") {
        StandaloneColorEvidence::Available(ColorLevel::Ansi256)
    } else if term.is_empty() || term == "dumb" {
        StandaloneColorEvidence::Unavailable
    } else {
        StandaloneColorEvidence::Available(ColorLevel::Basic)
    }
}

/// Return the cached color level (calls [`detect`] if not yet initialized).
pub fn get() -> ColorLevel {
    detect()
}

/// Override the color level (useful for tests or `--color` flags).
///
/// Returns `Err` if already set.
pub fn set(level: ColorLevel) -> Result<(), ColorLevel> {
    COLOR_LEVEL.set(level)
}

// ── Color quantization ──────────────────────────────────────────────────

/// Downgrade to what the terminal can show: Ansi256 nearest-index, Basic nearest ANSI16, None to `Reset`.
pub fn quantize_color(color: Color, level: ColorLevel) -> Color {
    match level {
        ColorLevel::TrueColor => color,
        ColorLevel::Ansi256 => match color {
            Color::Rgb(r, g, b) => Color::Indexed(nearest_indexed(r, g, b)),
            other => other,
        },
        ColorLevel::Basic => match color {
            Color::Rgb(r, g, b) => indexed_to_ansi16(nearest_indexed(r, g, b)),
            Color::Indexed(n) => indexed_to_ansi16(n),
            other => other,
        },
        ColorLevel::None => Color::Reset,
    }
}

/// Quantize a color using the globally-detected level.
pub fn quantize(color: Color) -> Color {
    quantize_color(color, get())
}

// ── Terminal-based truecolor inference ──────────────────────────────────

/// Keep [`ColorLevel::None`] when `rejected` is set (`None` < `Basic`).
fn cap_level_if_vt_rejected(level: ColorLevel, rejected: bool) -> ColorLevel {
    if rejected {
        level.min(ColorLevel::Basic)
    } else {
        level
    }
}

/// `true` only when a live console handle refuses `ENABLE_VIRTUAL_TERMINAL_PROCESSING`.
/// Redirected stdout/stderr is not a refusal: detection stays as it was.
fn windows_console_rejects_vt() -> bool {
    #[cfg(windows)]
    {
        // stderr is the pager's render handle. stdout covers a console that only bound that.
        const STD_ERROR_HANDLE: u32 = 0xFFFF_FFF4;
        const STD_OUTPUT_HANDLE: u32 = 0xFFFF_FFF5;
        console_handle_rejects_vt(STD_ERROR_HANDLE)
            .or_else(|| console_handle_rejects_vt(STD_OUTPUT_HANDLE))
            .unwrap_or(false)
    }
    #[cfg(not(windows))]
    {
        false
    }
}

/// `Some(true)` when this handle is a console and the VT mode bit is rejected.
/// `Some(false)` when VT is already on or the host accepts it. `None` when it is not a console.
#[cfg(windows)]
fn console_handle_rejects_vt(kind: u32) -> Option<bool> {
    const ENABLE_VIRTUAL_TERMINAL_PROCESSING: u32 = 0x0004;
    unsafe extern "system" {
        fn GetStdHandle(nStdHandle: u32) -> *mut core::ffi::c_void;
        fn GetConsoleMode(hConsoleHandle: *mut core::ffi::c_void, lpMode: *mut u32) -> i32;
        fn SetConsoleMode(hConsoleHandle: *mut core::ffi::c_void, dwMode: u32) -> i32;
    }

    // SAFETY: `kind` is a standard-handle id. The mode pointer is a live local.
    // On success the original mode is written back before returning.
    unsafe {
        let handle = GetStdHandle(kind);
        if handle.is_null() || handle == -1isize as *mut core::ffi::c_void {
            return None;
        }
        let mut mode = 0u32;
        if GetConsoleMode(handle, &mut mode) == 0 {
            return None;
        }
        if mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING != 0 {
            return Some(false);
        }
        if SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0 {
            // Detection must not leave the console in a different mode.
            let _ = SetConsoleMode(handle, mode);
            return Some(false);
        }
        Some(true)
    }
}

/// Used as a fallback when `COLORTERM` is missing: inside tmux, SSH, or a bare `cmd.exe` / `powershell.exe` ConHost window.
/// ConHost has supported VT-encoded 24-bit color since Windows 10 1709 (Fall Creators Update) but doesn't advertise it via COLORTERM.
/// Without the fallback, themes there get quantized to the 16-color ANSI palette and the subtle bg/border/muted gradations collapse onto each other.
/// [`detect_raw`] still caps this at Basic when the console rejects virtual-terminal processing, which is Windows XP.
fn terminal_supports_truecolor() -> bool {
    terminal_supports_truecolor_brand(terminal_context().brand)
}

fn terminal_supports_truecolor_brand(terminal: TerminalName) -> bool {
    if matches!(
        terminal,
        TerminalName::Iterm2
            | TerminalName::Ghostty
            | TerminalName::Kitty
            | TerminalName::WezTerm
            | TerminalName::Alacritty
            | TerminalName::Rio
            | TerminalName::WarpTerminal
            | TerminalName::VsCode
            | TerminalName::WindowsTerminal
            | TerminalName::Foot
    ) {
        return true;
    }
    // Win10 ConHost has VT truecolor and does not set COLORTERM.
    // detect_raw caps this when the console rejects virtual-terminal mode (Windows XP).
    cfg!(target_os = "windows")
}

// ── 256 → 16 mapping ────────────────────────────────────────────────────

/// Map a 256-color index to the nearest basic ANSI 16 color.
fn indexed_to_ansi16(n: u8) -> Color {
    match n {
        // First 16 indices already *are* the ANSI 16 colors.
        0 => Color::Black,
        1 => Color::Red,
        2 => Color::Green,
        3 => Color::Yellow,
        4 => Color::Blue,
        5 => Color::Magenta,
        6 => Color::Cyan,
        7 => Color::White, // actually "silver" in most terminals
        8 => Color::DarkGray,
        9 => Color::LightRed,
        10 => Color::LightGreen,
        11 => Color::LightYellow,
        12 => Color::LightBlue,
        13 => Color::LightMagenta,
        14 => Color::LightCyan,
        15 => Color::White,
        // For 16–255, convert to RGB and find nearest ANSI 16 color.
        _ => {
            let (r, g, b) = indexed_to_rgb(n);
            rgb_to_ansi16(r, g, b)
        }
    }
}

/// Nearest xterm ANSI 16 by squared-Euclidean distance. Fallback only; 16-color terminals are rare.
fn rgb_to_ansi16(r: u8, g: u8, b: u8) -> Color {
    // Standard xterm ANSI 16 palette (same values used by indexed_to_rgb for 0–15).
    const PALETTE: [(u8, u8, u8, Color); 16] = [
        (0, 0, 0, Color::Black),
        (128, 0, 0, Color::Red),
        (0, 128, 0, Color::Green),
        (128, 128, 0, Color::Yellow),
        (0, 0, 128, Color::Blue),
        (128, 0, 128, Color::Magenta),
        (0, 128, 128, Color::Cyan),
        (192, 192, 192, Color::White),
        (128, 128, 128, Color::DarkGray),
        (255, 0, 0, Color::LightRed),
        (0, 255, 0, Color::LightGreen),
        (255, 255, 0, Color::LightYellow),
        (0, 0, 255, Color::LightBlue),
        (255, 0, 255, Color::LightMagenta),
        (0, 255, 255, Color::LightCyan),
        (255, 255, 255, Color::White), // index 15 = bright white
    ];

    let mut best = Color::White;
    let mut best_dist = u32::MAX;
    for &(pr, pg, pb, color) in &PALETTE {
        let dr = r as i32 - pr as i32;
        let dg = g as i32 - pg as i32;
        let db = b as i32 - pb as i32;
        let dist = (dr * dr + dg * dg + db * db) as u32;
        if dist < best_dist {
            best_dist = dist;
            best = color;
        }
    }
    best
}

#[cfg(test)]
mod tests {
    use super::*;

    fn env(pairs: &[(&str, &str)]) -> std::collections::HashMap<String, String> {
        pairs
            .iter()
            .map(|(key, value)| ((*key).to_owned(), (*value).to_owned()))
            .collect()
    }

    #[test]
    fn standalone_color_uses_stderr_terminal_not_stdout() {
        assert_eq!(
            standalone_from_env(
                &env(&[("COLORTERM", "truecolor")]),
                true,
                false,
                TerminalName::Unknown,
            ),
            StandaloneColorEvidence::Available(ColorLevel::TrueColor)
        );
        assert_eq!(
            standalone_from_env(
                &env(&[("TERM", "xterm-256color")]),
                true,
                false,
                TerminalName::Unknown,
            ),
            StandaloneColorEvidence::Available(ColorLevel::Ansi256)
        );
        assert_eq!(
            standalone_from_env(
                &env(&[("COLORTERM", "truecolor")]),
                false,
                false,
                TerminalName::Ghostty,
            ),
            StandaloneColorEvidence::Unavailable
        );
        assert_eq!(
            standalone_from_env(
                &env(&[("NO_COLOR", "1")]),
                false,
                false,
                TerminalName::Ghostty,
            ),
            StandaloneColorEvidence::Available(ColorLevel::None)
        );
    }

    #[test]
    fn standalone_color_uses_controlling_terminal_when_stderr_is_piped() {
        assert_eq!(
            standalone_from_env(
                &env(&[("TERM", "xterm-256color")]),
                false,
                true,
                TerminalName::Unknown,
            ),
            StandaloneColorEvidence::Available(ColorLevel::Ansi256)
        );
    }

    #[test]
    fn standalone_color_uses_known_terminal_brand_without_colorterm() {
        assert_eq!(
            standalone_from_env(
                &env(&[("TERM", "xterm")]),
                true,
                false,
                TerminalName::WezTerm,
            ),
            StandaloneColorEvidence::Available(ColorLevel::TrueColor)
        );
    }

    #[test]
    fn truecolor_passes_through() {
        let rgb = Color::Rgb(122, 162, 247);
        assert_eq!(quantize_color(rgb, ColorLevel::TrueColor), rgb);

        let idx = Color::Indexed(141);
        assert_eq!(quantize_color(idx, ColorLevel::TrueColor), idx);
    }

    #[test]
    fn ansi256_quantizes_rgb_to_indexed() {
        let rgb = Color::Rgb(122, 162, 247);
        let q = quantize_color(rgb, ColorLevel::Ansi256);
        assert!(matches!(q, Color::Indexed(_)));
    }

    #[test]
    fn ansi256_passes_indexed_through() {
        let idx = Color::Indexed(141);
        assert_eq!(quantize_color(idx, ColorLevel::Ansi256), idx);
    }

    #[test]
    fn basic_quantizes_to_named() {
        let rgb = Color::Rgb(255, 0, 0);
        let q = quantize_color(rgb, ColorLevel::Basic);
        assert!(
            matches!(q, Color::Red | Color::LightRed),
            "expected Red/LightRed, got {q:?}"
        );
    }

    #[test]
    fn basic_quantizes_indexed_to_named() {
        // Indexed(196) is (255,0,0), pure bright red in the cube
        let idx = Color::Indexed(196);
        let q = quantize_color(idx, ColorLevel::Basic);
        assert!(
            matches!(q, Color::Red | Color::LightRed),
            "expected Red/LightRed, got {q:?}"
        );
    }

    #[test]
    fn none_resets_everything() {
        assert_eq!(
            quantize_color(Color::Rgb(100, 200, 50), ColorLevel::None),
            Color::Reset
        );
        assert_eq!(
            quantize_color(Color::Indexed(111), ColorLevel::None),
            Color::Reset
        );
    }

    #[test]
    fn named_colors_pass_through_all_levels() {
        for level in [
            ColorLevel::TrueColor,
            ColorLevel::Ansi256,
            ColorLevel::Basic,
        ] {
            assert_eq!(quantize_color(Color::Red, level), Color::Red);
            assert_eq!(quantize_color(Color::Blue, level), Color::Blue);
        }
    }

    #[test]
    fn vt_rejection_caps_at_basic_and_keeps_no_color() {
        assert_eq!(
            cap_level_if_vt_rejected(ColorLevel::TrueColor, true),
            ColorLevel::Basic
        );
        assert_eq!(
            cap_level_if_vt_rejected(ColorLevel::Ansi256, true),
            ColorLevel::Basic
        );
        assert_eq!(
            cap_level_if_vt_rejected(ColorLevel::TrueColor, false),
            ColorLevel::TrueColor
        );
        assert_eq!(
            cap_level_if_vt_rejected(ColorLevel::None, true),
            ColorLevel::None
        );
    }

    #[test]
    fn level_ordering() {
        assert!(ColorLevel::None < ColorLevel::Basic);
        assert!(ColorLevel::Basic < ColorLevel::Ansi256);
        assert!(ColorLevel::Ansi256 < ColorLevel::TrueColor);
    }

    #[test]
    fn ansi16_roundtrip_first_16() {
        assert_eq!(indexed_to_ansi16(0), Color::Black);
        assert_eq!(indexed_to_ansi16(1), Color::Red);
        assert_eq!(indexed_to_ansi16(4), Color::Blue);
        assert_eq!(indexed_to_ansi16(9), Color::LightRed);
        assert_eq!(indexed_to_ansi16(14), Color::LightCyan);
    }
}
