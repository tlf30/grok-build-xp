use std::convert::TryFrom;
use std::sync::atomic::{AtomicU32, Ordering};

use crossterm_winapi::{Console, Handle, HandleType, ScreenBuffer};
use winapi::um::wincon;

use super::super::{Color, Colored};

const FG_GREEN: u16 = wincon::FOREGROUND_GREEN;
const FG_RED: u16 = wincon::FOREGROUND_RED;
const FG_BLUE: u16 = wincon::FOREGROUND_BLUE;
const FG_INTENSITY: u16 = wincon::FOREGROUND_INTENSITY;

const BG_GREEN: u16 = wincon::BACKGROUND_GREEN;
const BG_RED: u16 = wincon::BACKGROUND_RED;
const BG_BLUE: u16 = wincon::BACKGROUND_BLUE;
const BG_INTENSITY: u16 = wincon::BACKGROUND_INTENSITY;

pub(crate) fn set_foreground_color(fg_color: Color) -> std::io::Result<()> {
    init_console_color()?;

    let color_value: u16 = Colored::ForegroundColor(fg_color).into();

    let screen_buffer = ScreenBuffer::current()?;
    let csbi = screen_buffer.info()?;

    // Notice that the color values are stored in wAttribute.
    // So we need to use bitwise operators to check if the values exists or to get current console colors.
    let attrs = csbi.attributes();
    let bg_color = attrs & 0x0070;
    let mut color = color_value | bg_color;

    // background intensity is a separate value in attrs,
    // we need to check if this was applied to the current bg color.
    if (attrs & wincon::BACKGROUND_INTENSITY) != 0 {
        color |= wincon::BACKGROUND_INTENSITY;
    }

    Console::from(screen_buffer.handle().clone()).set_text_attribute(color)?;
    Ok(())
}

pub(crate) fn set_background_color(bg_color: Color) -> std::io::Result<()> {
    init_console_color()?;

    let color_value: u16 = Colored::BackgroundColor(bg_color).into();

    let screen_buffer = ScreenBuffer::current()?;
    let csbi = screen_buffer.info()?;

    // Notice that the color values are stored in wAttribute.
    // So we need to use bitwise operators to check if the values exists or to get current console colors.
    let attrs = csbi.attributes();
    let fg_color = attrs & 0x0007;
    let mut color = fg_color | color_value;

    // Foreground intensity is a separate value in attrs,
    // So we need to check if this was applied to the current fg color.
    if (attrs & wincon::FOREGROUND_INTENSITY) != 0 {
        color |= wincon::FOREGROUND_INTENSITY;
    }

    Console::from(screen_buffer.handle().clone()).set_text_attribute(color)?;
    Ok(())
}

/// Write `text` at the current cursor with WriteConsoleW.
/// Used when the console cannot process ANSI, including Windows XP.
pub(crate) fn print(text: &str) -> std::io::Result<()> {
    if text.is_empty() {
        return Ok(());
    }
    let handle = Handle::new(HandleType::CurrentOutputHandle)?;
    Console::from(handle).write_char_buffer(text.as_bytes())?;
    Ok(())
}

pub(crate) fn reset() -> std::io::Result<()> {
    if let Ok(original_color) = u16::try_from(ORIGINAL_CONSOLE_COLOR.load(Ordering::Relaxed)) {
        Console::from(Handle::new(HandleType::CurrentOutputHandle)?)
            .set_text_attribute(original_color)?;
    }

    Ok(())
}

/// Initializes the default console color. It will will be skipped if it has already been initialized.
pub(crate) fn init_console_color() -> std::io::Result<()> {
    if ORIGINAL_CONSOLE_COLOR.load(Ordering::Relaxed) == u32::MAX {
        let screen_buffer = ScreenBuffer::current()?;
        let attr = screen_buffer.info()?.attributes();
        ORIGINAL_CONSOLE_COLOR.store(u32::from(attr), Ordering::Relaxed);
    }

    Ok(())
}

/// Xterm ANSI-16 palette, then the Win32 attribute for that slot (foreground bits).
/// Background uses the same bits shifted by 4.
const ANSI16_RGB: [(u8, u8, u8); 16] = [
    (0, 0, 0),
    (128, 0, 0),
    (0, 128, 0),
    (128, 128, 0),
    (0, 0, 128),
    (128, 0, 128),
    (0, 128, 128),
    (192, 192, 192),
    (128, 128, 128),
    (255, 0, 0),
    (0, 255, 0),
    (255, 255, 0),
    (0, 0, 255),
    (255, 0, 255),
    (0, 255, 255),
    (255, 255, 255),
];

fn ansi16_fg(index: u8) -> u16 {
    const FG: [u16; 16] = [
        0,
        FG_RED,
        FG_GREEN,
        FG_RED | FG_GREEN,
        FG_BLUE,
        FG_RED | FG_BLUE,
        FG_GREEN | FG_BLUE,
        FG_RED | FG_GREEN | FG_BLUE,
        FG_INTENSITY,
        FG_INTENSITY | FG_RED,
        FG_INTENSITY | FG_GREEN,
        FG_INTENSITY | FG_RED | FG_GREEN,
        FG_INTENSITY | FG_BLUE,
        FG_INTENSITY | FG_RED | FG_BLUE,
        FG_INTENSITY | FG_GREEN | FG_BLUE,
        FG_INTENSITY | FG_RED | FG_GREEN | FG_BLUE,
    ];
    FG[(index & 15) as usize]
}

fn attr_from_index(index: u8, background: bool) -> u16 {
    let fg = ansi16_fg(index);
    if background { fg << 4 } else { fg }
}

fn nearest_ansi16(r: u8, g: u8, b: u8) -> u8 {
    let mut best_i = 0u8;
    let mut best_dist = u32::MAX;
    for (i, &(pr, pg, pb)) in ANSI16_RGB.iter().enumerate() {
        let dr = r as i32 - pr as i32;
        let dg = g as i32 - pg as i32;
        let db = b as i32 - pb as i32;
        let dist = (dr * dr + dg * dg + db * db) as u32;
        if dist < best_dist {
            best_dist = dist;
            best_i = i as u8;
        }
    }
    best_i
}

/// Nearest of the 16 console colors. XP Win32 attributes have no RGB field;
/// leaving these as 0 paints the theme as black on black.
fn nearest_console_attr(r: u8, g: u8, b: u8, background: bool) -> u16 {
    attr_from_index(nearest_ansi16(r, g, b), background)
}

fn ansi256_to_rgb(n: u8) -> (u8, u8, u8) {
    match n {
        0..=15 => ANSI16_RGB[n as usize],
        16..=231 => {
            let i = n - 16;
            let scale = |c: u8| if c == 0 { 0 } else { 55 + c * 40 };
            (scale(i / 36), scale((i / 6) % 6), scale(i % 6))
        }
        gray => {
            let v = 8 + (gray - 232) * 10;
            (v, v, v)
        }
    }
}

fn ansi_to_console_attr(n: u8, background: bool) -> u16 {
    if n < 16 {
        return attr_from_index(n, background);
    }
    let (r, g, b) = ansi256_to_rgb(n);
    nearest_console_attr(r, g, b, background)
}

/// Returns the original console color, make sure to call `init_console_color` before calling this function. Otherwise this function will panic.
pub(crate) fn original_console_color() -> u16 {
    u16::try_from(ORIGINAL_CONSOLE_COLOR.load(Ordering::Relaxed))
        // safe unwrap, initial console color was set with `init_console_color` in `WinApiColor::new()`
        .expect("Initial console color not set")
}

// This is either a valid u16 in which case it stores the original console color or it is u32::MAX
// in which case it is uninitialized.
static ORIGINAL_CONSOLE_COLOR: AtomicU32 = AtomicU32::new(u32::MAX);

impl From<Colored> for u16 {
    /// Returns the WinAPI color value (u16) from the `Colored` struct.
    fn from(colored: Colored) -> Self {
        match colored {
            Colored::ForegroundColor(color) => {
                match color {
                    Color::Black => 0,
                    Color::DarkGrey => FG_INTENSITY,
                    Color::Red => FG_INTENSITY | FG_RED,
                    Color::DarkRed => FG_RED,
                    Color::Green => FG_INTENSITY | FG_GREEN,
                    Color::DarkGreen => FG_GREEN,
                    Color::Yellow => FG_INTENSITY | FG_GREEN | FG_RED,
                    Color::DarkYellow => FG_GREEN | FG_RED,
                    Color::Blue => FG_INTENSITY | FG_BLUE,
                    Color::DarkBlue => FG_BLUE,
                    Color::Magenta => FG_INTENSITY | FG_RED | FG_BLUE,
                    Color::DarkMagenta => FG_RED | FG_BLUE,
                    Color::Cyan => FG_INTENSITY | FG_GREEN | FG_BLUE,
                    Color::DarkCyan => FG_GREEN | FG_BLUE,
                    Color::White => FG_INTENSITY | FG_RED | FG_GREEN | FG_BLUE,
                    Color::Grey => FG_RED | FG_GREEN | FG_BLUE,

                    Color::Reset => {
                        // safe unwrap, initial console color was set with `init_console_color`.
                        let original_color = original_console_color();

                        const REMOVE_BG_MASK: u16 = BG_INTENSITY | BG_RED | BG_GREEN | BG_BLUE;
                        // remove all background values from the original color, we don't want to reset those.

                        original_color & !REMOVE_BG_MASK
                    }

                    // XP has no VT processing. Map RGB and the 256-color cube onto
                    // the 16 Win32 attributes so light text does not become black.
                    Color::Rgb { r, g, b } => nearest_console_attr(r, g, b, false),
                    Color::AnsiValue(val) => ansi_to_console_attr(val, false),
                }
            }
            Colored::BackgroundColor(color) => {
                match color {
                    Color::Black => 0,
                    Color::DarkGrey => BG_INTENSITY,
                    Color::Red => BG_INTENSITY | BG_RED,
                    Color::DarkRed => BG_RED,
                    Color::Green => BG_INTENSITY | BG_GREEN,
                    Color::DarkGreen => BG_GREEN,
                    Color::Yellow => BG_INTENSITY | BG_GREEN | BG_RED,
                    Color::DarkYellow => BG_GREEN | BG_RED,
                    Color::Blue => BG_INTENSITY | BG_BLUE,
                    Color::DarkBlue => BG_BLUE,
                    Color::Magenta => BG_INTENSITY | BG_RED | BG_BLUE,
                    Color::DarkMagenta => BG_RED | BG_BLUE,
                    Color::Cyan => BG_INTENSITY | BG_GREEN | BG_BLUE,
                    Color::DarkCyan => BG_GREEN | BG_BLUE,
                    Color::White => BG_INTENSITY | BG_RED | BG_GREEN | BG_BLUE,
                    Color::Grey => BG_RED | BG_GREEN | BG_BLUE,

                    Color::Reset => {
                        let original_color = original_console_color();

                        const REMOVE_FG_MASK: u16 = FG_INTENSITY | FG_RED | FG_GREEN | FG_BLUE;
                        // remove all foreground values from the original color, we don't want to reset those.

                        original_color & !REMOVE_FG_MASK
                    }
                    Color::Rgb { r, g, b } => nearest_console_attr(r, g, b, true),
                    Color::AnsiValue(val) => ansi_to_console_attr(val, true),
                }
            }
            Colored::UnderlineColor(_) => 0,
        }
    }
}

#[cfg(test)]
mod tests {
    use std::sync::atomic::Ordering;

    use crate::style::sys::windows::set_foreground_color;

    use super::{
        Color, Colored, BG_INTENSITY, BG_RED, FG_BLUE, FG_GREEN, FG_INTENSITY, FG_RED,
        ORIGINAL_CONSOLE_COLOR,
    };

    #[test]
    fn test_parse_fg_color() {
        let colored = Colored::ForegroundColor(Color::Red);
        assert_eq!(Into::<u16>::into(colored), FG_INTENSITY | FG_RED);
    }

    #[test]
    fn test_parse_bg_color() {
        let colored = Colored::BackgroundColor(Color::Red);
        assert_eq!(Into::<u16>::into(colored), BG_INTENSITY | BG_RED);
    }

    #[test]
    fn test_original_console_color_is_set() {
        assert_eq!(ORIGINAL_CONSOLE_COLOR.load(Ordering::Relaxed), u32::MAX);

        // will call `init_console_color`
        set_foreground_color(Color::Blue).unwrap();

        assert_ne!(ORIGINAL_CONSOLE_COLOR.load(Ordering::Relaxed), u32::MAX);
    }

    #[test]
    fn light_gray_rgb_is_bright_white_not_black() {
        let colored = Colored::ForegroundColor(Color::Rgb {
            r: 225,
            g: 225,
            b: 225,
        });
        let attr: u16 = colored.into();
        assert_eq!(attr, FG_INTENSITY | FG_RED | FG_GREEN | FG_BLUE);
    }

    #[test]
    fn near_black_rgb_background_stays_black() {
        let colored = Colored::BackgroundColor(Color::Rgb { r: 20, g: 20, b: 20 });
        let attr: u16 = colored.into();
        assert_eq!(attr, 0);
    }

    #[test]
    fn ansi_bright_white_is_not_black() {
        let colored = Colored::ForegroundColor(Color::AnsiValue(15));
        let attr: u16 = colored.into();
        assert_eq!(attr, FG_INTENSITY | FG_RED | FG_GREEN | FG_BLUE);
    }

    #[test]
    fn ansi_cube_bright_red_is_intense_red() {
        let colored = Colored::ForegroundColor(Color::AnsiValue(196));
        let attr: u16 = colored.into();
        assert_eq!(attr, FG_INTENSITY | FG_RED);
    }
}
