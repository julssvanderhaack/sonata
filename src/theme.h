// Colours from the active Omarchy theme (colors.toml), reloaded live when the
// theme changes.
#pragma once
#include <QColor>
#include <QString>

struct Theme {
    QColor bg, bgDark, bgLight, fg, fgDim, fgLight, accent, selection, muted;
    bool dark = true;
    QString iconFont;

    static Theme &get();
    static QString colorsPath();
    void load();
    void apply();                  // palette + stylesheet on qApp
    QString styleSheet() const;
};
