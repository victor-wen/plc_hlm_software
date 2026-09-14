#pragma once

class QApplication;

namespace hlm {

// Applies the fixed light industrial application theme.
//
// The HMI must look the same regardless of the operating system's light/dark
// color scheme: the theme.qss palette assumes light surfaces. This forces the
// Fusion style and the light palette (and, on Qt >= 6.5, the Light color
// scheme) before the shell is built. Call once, right after QApplication is
// constructed.
void applyAppTheme(QApplication &app);

} // namespace hlm
