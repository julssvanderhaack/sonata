#!/bin/sh
# Instala Sonata desde el zip de una versión.
#   ./install.sh                 en ~/.local (solo para ti, sin sudo)
#   sudo ./install.sh --system   en /usr/local (para todos)
#   añade --uninstall para quitarlo
set -e
here=$(cd "$(dirname "$0")" && pwd)
prefix="$HOME/.local"
uninstall=0
for a in "$@"; do
  case "$a" in
    --system) prefix=/usr/local ;;
    --uninstall) uninstall=1 ;;
    *) echo "Opción desconocida: $a" >&2; exit 2 ;;
  esac
done

bin="$prefix/bin/sonata"
desktop="$prefix/share/applications/sonata.desktop"
icon="$prefix/share/icons/hicolor/scalable/apps/sonata.svg"

refresh() {
  command -v update-desktop-database >/dev/null && update-desktop-database -q "$prefix/share/applications" 2>/dev/null || true
  command -v gtk-update-icon-cache >/dev/null && gtk-update-icon-cache -q -t "$prefix/share/icons/hicolor" 2>/dev/null || true
}

if [ "$uninstall" = 1 ]; then
  rm -f "$bin" "$desktop" "$icon"
  refresh
  echo "Sonata desinstalado de $prefix (tus listas y ajustes siguen en ~/.local)."
  exit 0
fi

# avisa de bibliotecas que falten antes de copiar nada
missing=$(ldd "$here/sonata" 2>/dev/null | awk '/not found/ {print $1}')
if [ -n "$missing" ]; then
  echo "Faltan bibliotecas del sistema:" >&2
  echo "$missing" | sed 's/^/  /' >&2
  echo "En Arch: sudo pacman -S --needed qt6-base ffmpeg taglib pipewire" >&2
  echo "Si ya las tienes, tu sistema es más antiguo que el de la compilación: compílalo (ver README.md)." >&2
  exit 1
fi

install -Dm755 "$here/sonata" "$bin"
install -Dm644 "$here/sonata.svg" "$icon"
# ruta completa en Exec: funciona aunque ~/.local/bin no esté en el PATH
mkdir -p "$(dirname "$desktop")"
sed "s|^Exec=sonata |Exec=$bin |" "$here/sonata.desktop" > "$desktop"
chmod 644 "$desktop"
refresh
echo "Sonata instalado en $bin"
echo "Búscalo en el lanzador de aplicaciones o ejecuta: $bin"
