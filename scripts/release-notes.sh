#!/usr/bin/env bash
# Imprime las notas de una versión: la sección "## <version>" de CHANGELOG.md
# más un recordatorio de instalación. Uso: scripts/release-notes.sh 1.2.0
set -euo pipefail
version="${1:?uso: release-notes.sh <version>}"
cd "$(dirname "$0")/.."

awk -v v="$version" '
  $0 == "## " v { on = 1; next }
  /^## / && on  { exit }
  on            { print }
' CHANGELOG.md | sed -e :a -e '/^\n*$/{$d;N;ba' -e '}'

cat <<'EOF'

---

### Instalación
Copia el archivo `.dusk` de tu plataforma a la carpeta de mods de Dusklight:

- Windows: `%APPDATA%\TwilitRealm\Dusklight\mods`
- Linux: `~/.local/share/TwilitRealm/Dusklight/mods`
- macOS: `~/Library/Application Support/TwilitRealm/Dusklight/mods`

Después abre Dusklight y activa HiveShock en el menú Mods. Compilado contra Dusklight v2.0.0.
EOF
