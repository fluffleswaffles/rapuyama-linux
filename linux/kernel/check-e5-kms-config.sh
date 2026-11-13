#!/usr/bin/env bash
# validates the KMS default only (not E5_FBDEV=1): fails unless the E5 build
# dir <KOUT> has the KMS display driver as the display (make olddefconfig drops options whose
# dependencies are missing, so this checks the resolved .config, not what build-e5.sh asked for)
# usage: ./check-e5-kms-config.sh <KOUT>
set -u
KOUT="${1:?usage: $0 <KOUT>}"
C="$KOUT/.config"
[ -f "$C" ] || { echo "FAIL: no $C"; exit 1; }
rc=0
for l in CONFIG_DRM=y CONFIG_FB_DEVICE=y CONFIG_FONT_8x8=y CONFIG_RAPUYAMA_E5_DISPLAY=y CONFIG_DRM_FBDEV_EMULATION=y \
	"# CONFIG_FB_RAPUYAMA_E5 is not set" CONFIG_FRAMEBUFFER_CONSOLE=y; do
	if grep -qxF "$l" "$C"; then echo "ok:   $l"; else echo "FAIL: $l"; rc=1; fi
done
exit $rc
