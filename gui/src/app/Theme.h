#ifndef REVSTUDIO_THEME_H
#define REVSTUDIO_THEME_H

namespace revstudio::Theme {

/** System: leave the platform's native style and palette alone. Light/Dark: force the Fusion style (the usual
 *  choice for a custom palette, since native styles mostly ignore QPalette overrides) with a matching palette. */
enum class Mode { System, Light, Dark };

/** Applies `mode` to the running QApplication. Must be called after a QApplication exists; cheap enough to call
 *  again whenever the user changes it (View > Theme). */
void apply(Mode mode);

} // namespace revstudio::Theme

#endif
