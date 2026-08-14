#!/usr/bin/env bash
# tests/test-install.sh — unit tests for install.sh's configure_bootloader().
#
# The function is extracted from install.sh and run against a mocked
# rpi-eeprom-config and a mocked device-tree model path (RPI_EEPROM_CONFIG
# / DT_MODEL_PATH overrides).  Assertions are made on the *staged* config
# file the mock captures at --apply time — never on a read-back, since on
# real hardware the flash only happens at the next boot.
#
# Run:  bash tests/test-install.sh
#
# SPDX-License-Identifier: GPL-2.0-or-later

# SKIP_EEPROM and X120X_CLEANUP look unused to shellcheck but are read by
# the eval-extracted configure_bootloader(), which it cannot see.
# shellcheck disable=SC2034
set -uo pipefail   # deliberately not -e: we want every case to run

# Tests never need root; refuse it so a stray sudo can't touch the system.
if [ "$(id -u)" -eq 0 ]; then
    echo "Refusing to run as root — these tests never need it." >&2
    exit 2
fi

HERE=$(cd "$(dirname "$0")" && pwd)
INSTALL_SH="${HERE}/../install.sh"

[ -f "${INSTALL_SH}" ] || { echo "cannot find install.sh at ${INSTALL_SH}" >&2; exit 2; }

WORK=$(mktemp -d)
trap 'rm -rf -- "${WORK}"' EXIT

# Extract just the function under test.  This sed range works only
# because configure_bootloader()'s closing brace is the first column-0
# "}" after its opener — keep it that way (no top-level "}" in the body).
CB_SRC=$(sed -n '/^configure_bootloader() {/,/^}/p' "${INSTALL_SH}")
[ -n "${CB_SRC}" ] || { echo "could not extract configure_bootloader() from install.sh" >&2; exit 2; }

PASS=0
FAIL=0
assert() {  # assert "name" "test-expression"
    if eval "$2" >/dev/null 2>&1; then
        PASS=$((PASS + 1))
        printf '  \033[0;32mPASS\033[0m %s\n' "$1"
    else
        FAIL=$((FAIL + 1))
        printf '  \033[0;31mFAIL\033[0m %s   [%s]\n' "$1" "$2"
    fi
}

# A mock rpi-eeprom-config: no args -> dump ${MOCK_CURRENT}; "--apply FILE"
# -> record the call and copy FILE to ${MOCK_APPLIED}.
make_mock() {
    cat > "$1" <<'MOCK'
#!/usr/bin/env bash
if [ "${1:-}" = "--apply" ]; then
    echo apply >> "${MOCK_APPLY_LOG}"
    cp "$2" "${MOCK_APPLIED}"
    exit "${MOCK_APPLY_RC:-0}"
fi
cat "${MOCK_CURRENT}"
MOCK
    chmod +x "$1"
}

# Reset per-case state and default to a healthy Pi 5 with the mock present.
reset() {
    export MOCK_APPLIED="${WORK}/applied"
    export MOCK_APPLY_LOG="${WORK}/apply.log"
    export MOCK_CURRENT="${WORK}/current"
    export MOCK_APPLY_RC=0
    : > "${MOCK_APPLY_LOG}"
    rm -f -- "${MOCK_APPLIED}"
    : > "${MOCK_CURRENT}"

    export RPI_EEPROM_CONFIG="${WORK}/mock-eeprom"
    make_mock "${RPI_EEPROM_CONFIG}"

    printf 'Raspberry Pi 5 Model B Rev 1.0\0' > "${WORK}/model"
    export DT_MODEL_PATH="${WORK}/model"

    SKIP_EEPROM=0
}

# Run configure_bootloader() in a subshell with logging stubs; logs land in
# ${LOGCAP}, the subshell's exit status is returned.
LOGCAP="${WORK}/log"
run_cb() {
    : > "${LOGCAP}"
    (
        info() { echo "info:$*" >> "${LOGCAP}"; }
        ok()   { echo "ok:$*"   >> "${LOGCAP}"; }
        warn() { echo "warn:$*" >> "${LOGCAP}"; }
        die()  { echo "die:$*"  >> "${LOGCAP}"; exit 1; }
        X120X_CLEANUP=()   # install.sh's main flow provides this; stub it here
        eval "${CB_SRC}"
        configure_bootloader
    )
}

applied_count() { wc -l < "${MOCK_APPLY_LOG}" | tr -d ' '; }

echo "configure_bootloader() tests"

# 1 — Non-Pi-5 model: skip, no --apply.
reset
printf 'Raspberry Pi 4 Model B Rev 1.4\0' > "${WORK}/model"
run_cb; rc=$?
assert "1 non-Pi5: returns 0"        '[ "'"$rc"'" -eq 0 ]'
assert "1 non-Pi5: no --apply"       '[ ! -s "${MOCK_APPLY_LOG}" ]'
assert "1 non-Pi5: logs 'not required'" 'grep -q "not required on this model" "${LOGCAP}"'

# 1b — Raspberry Pi 500 must NOT match the "Raspberry Pi 5 " pattern.
reset
printf 'Raspberry Pi 500 Rev 1.0\0' > "${WORK}/model"
run_cb; rc=$?
assert "1b Pi 500: returns 0"           '[ "'"$rc"'" -eq 0 ]'
assert "1b Pi 500: no --apply"          '[ ! -s "${MOCK_APPLY_LOG}" ]'
assert "1b Pi 500: logs 'not required'" 'grep -q "not required on this model" "${LOGCAP}"'

# 2 — Model file absent: same skip path, exit 0.
reset
export DT_MODEL_PATH="${WORK}/no-such-model"
run_cb; rc=$?
assert "2 no model file: returns 0"  '[ "'"$rc"'" -eq 0 ]'
assert "2 no model file: no --apply" '[ ! -s "${MOCK_APPLY_LOG}" ]'

# 3 — Pi 5, neither key present: both appended, --apply once, others kept.
reset
printf 'BOOT_ORDER=0xf41\nHDMI=1\n' > "${MOCK_CURRENT}"
run_cb; rc=$?
assert "3 neither key: returns 0"    '[ "'"$rc"'" -eq 0 ]'
assert "3 neither key: --apply once" '[ "$(applied_count)" -eq 1 ]'
assert "3 neither key: POWER_OFF_ON_HALT=1 staged" 'grep -qx "POWER_OFF_ON_HALT=1" "${MOCK_APPLIED}"'
assert "3 neither key: PSU_MAX_CURRENT=5000 staged" 'grep -qx "PSU_MAX_CURRENT=5000" "${MOCK_APPLIED}"'
assert "3 neither key: other keys preserved" 'grep -qx "BOOT_ORDER=0xf41" "${MOCK_APPLIED}"'

# 4 — Pi 5, both keys already correct: no --apply.
reset
printf 'BOOT_ORDER=0xf41\nPOWER_OFF_ON_HALT=1\nPSU_MAX_CURRENT=5000\n' > "${MOCK_CURRENT}"
run_cb; rc=$?
assert "4 both correct: returns 0"   '[ "'"$rc"'" -eq 0 ]'
assert "4 both correct: no --apply"  '[ ! -s "${MOCK_APPLY_LOG}" ]'
assert "4 both correct: logs 'already configured'" 'grep -q "already configured" "${LOGCAP}"'

# 5 — Pi 5, prior POWER_OFF_ON_HALT=0: rewritten to =1, one --apply, others intact.
reset
printf 'BOOT_ORDER=0xf41\nPOWER_OFF_ON_HALT=0\nPSU_MAX_CURRENT=5000\nWAKE_ON_GPIO=1\n' > "${MOCK_CURRENT}"
run_cb; rc=$?
assert "5 prior =0: --apply once"    '[ "$(applied_count)" -eq 1 ]'
assert "5 prior =0: rewritten to =1" 'grep -qx "POWER_OFF_ON_HALT=1" "${MOCK_APPLIED}"'
assert "5 prior =0: no =0 remains"   '! grep -qx "POWER_OFF_ON_HALT=0" "${MOCK_APPLIED}"'
assert "5 prior =0: WAKE_ON_GPIO intact" 'grep -qx "WAKE_ON_GPIO=1" "${MOCK_APPLIED}"'
assert "5 prior =0: BOOT_ORDER intact"   'grep -qx "BOOT_ORDER=0xf41" "${MOCK_APPLIED}"'

# 6 — Pi 5, one key correct / other missing: staged file has both correct.
reset
printf 'POWER_OFF_ON_HALT=1\nHDMI=1\n' > "${MOCK_CURRENT}"
run_cb; rc=$?
assert "6 one missing: --apply once" '[ "$(applied_count)" -eq 1 ]'
assert "6 one missing: POWER_OFF_ON_HALT=1 staged" 'grep -qx "POWER_OFF_ON_HALT=1" "${MOCK_APPLIED}"'
assert "6 one missing: PSU_MAX_CURRENT=5000 staged" 'grep -qx "PSU_MAX_CURRENT=5000" "${MOCK_APPLIED}"'

# 7 — Staged file has exactly one instance of each key (even given duplicates).
reset
printf 'POWER_OFF_ON_HALT=0\nPOWER_OFF_ON_HALT=1\nPSU_MAX_CURRENT=1000\nHDMI=1\n' > "${MOCK_CURRENT}"
run_cb; rc=$?
assert "7 dedup: exactly one POWER_OFF_ON_HALT" '[ "$(grep -c "^POWER_OFF_ON_HALT=" "${MOCK_APPLIED}")" -eq 1 ]'
assert "7 dedup: exactly one PSU_MAX_CURRENT"   '[ "$(grep -c "^PSU_MAX_CURRENT=" "${MOCK_APPLIED}")" -eq 1 ]'
assert "7 dedup: value is =1"   'grep -qx "POWER_OFF_ON_HALT=1" "${MOCK_APPLIED}"'
assert "7 dedup: value is =5000" 'grep -qx "PSU_MAX_CURRENT=5000" "${MOCK_APPLIED}"'

# 8 — --skip-eeprom: function returns early, no --apply.
reset
SKIP_EEPROM=1
run_cb; rc=$?
assert "8 skip-eeprom: returns 0"    '[ "'"$rc"'" -eq 0 ]'
assert "8 skip-eeprom: no --apply"   '[ ! -s "${MOCK_APPLY_LOG}" ]'
assert "8 skip-eeprom: logs skip"    'grep -q "Skipping bootloader" "${LOGCAP}"'

# 9 — rpi-eeprom-config missing: warn, continue, exit 0.
reset
export RPI_EEPROM_CONFIG="${WORK}/no-such-eeprom-binary"
run_cb; rc=$?
assert "9 missing binary: returns 0" '[ "'"$rc"'" -eq 0 ]'
assert "9 missing binary: no --apply" '[ ! -s "${MOCK_APPLY_LOG}" ]'
assert "9 missing binary: warns"     'grep -q "warn:rpi-eeprom-config not found" "${LOGCAP}"'

# 10 — Pi 5, dump succeeds but is empty: guard skips, no --apply.  A failed
#      unprivileged read must never be staged as a two-line config that
#      wipes the rest of the EEPROM config.
reset
: > "${MOCK_CURRENT}"
run_cb; rc=$?
assert "10 empty dump: returns 0"    '[ "'"$rc"'" -eq 0 ]'
assert "10 empty dump: no --apply"   '[ ! -s "${MOCK_APPLY_LOG}" ]'
assert "10 empty dump: warns"        'grep -q "read back empty" "${LOGCAP}"'

# 11 — Arg parsing: --help documents --skip-eeprom and exits 0.
echo "argument-parsing tests"
help_out=$(bash "${INSTALL_SH}" --help 2>&1); help_rc=$?
assert "11 --help: exits 0"          '[ "'"$help_rc"'" -eq 0 ]'
assert "11 --help: mentions --skip-eeprom" 'printf "%s" "'"$help_out"'" | grep -q -- "--skip-eeprom"'

# 12 — check_kernel_floor: numeric 6.3+ comparison on X120X_UNAME_R,
#      with warn-and-continue on an unparseable version string.
echo "check_kernel_floor tests"
KF_SRC=$(sed -n '/^check_kernel_floor() {/,/^}/p' "${INSTALL_SH}")
[ -n "${KF_SRC}" ] || { echo "could not extract check_kernel_floor() from install.sh" >&2; exit 2; }
run_kf() {  # kver -> rc; output on stdout
    (
        warn() { echo "warn:$*"; }
        die()  { echo "die:$*"; exit 1; }
        eval "${KF_SRC}"
        X120X_UNAME_R="$1" check_kernel_floor
    ) 2>&1
}
out=$(run_kf "6.1.21-v8+"); rc=$?
assert "12 kernel 6.1 dies"              '[ "'"$rc"'" -ne 0 ]'
assert "12 kernel 6.1 mentions 6.3"      'printf "%s" "'"$out"'" | grep -q "6.3"'
out=$(run_kf "6.12.75+rpt-rpi-v8"); rc=$?
assert "12 kernel 6.12 passes (numeric)" '[ "'"$rc"'" -eq 0 ]'
out=$(run_kf "6.3.0"); rc=$?
assert "12 kernel 6.3 passes"            '[ "'"$rc"'" -eq 0 ]'
out=$(run_kf "weird-version"); rc=$?
assert "12 garbage warns"                'printf "%s" "'"$out"'" | grep -q "warn:"'
assert "12 garbage continues (rc 0)"     '[ "'"$rc"'" -eq 0 ]'

# 13 — install_dkms_tree: explicit allowlist copy into the DKMS tree.
#      Must copy exactly the five files DKMS needs, and must NOT drag
#      .git, docs, or stray build artifacts from a developer tree.
echo "install_dkms_tree tests"
eval "$(sed -n '/^install_dkms_tree() {/,/^}/p' "${INSTALL_SH}")"

FIX="${WORK}/checkout"; DST="${WORK}/dkms-tree"
mkdir -p "${FIX}/src" "${FIX}/.git" "${FIX}/docs"
printf 'x' > "${FIX}/dkms.conf";   printf 'x' > "${FIX}/Makefile"
printf 'x' > "${FIX}/LICENSE";     printf 'x' > "${FIX}/README.md"
printf 'x' > "${FIX}/src/x120x.c"; printf 'x' > "${FIX}/src/Kbuild"
printf 'x' > "${FIX}/src/x120x.o"; printf 'x' > "${FIX}/.git/HEAD"
chmod 666 "${FIX}/src/x120x.c"

install_dkms_tree "${FIX}" "${DST}"
assert "13 five files copied, nothing else" \
    '[ "$(cd "${DST}" && find . -type f | LC_ALL=C sort | tr "\n" " ")" = "./LICENSE ./Makefile ./dkms.conf ./src/Kbuild ./src/x120x.c " ]'
assert "13 .git not copied"        '[ ! -e "${DST}/.git" ]'
assert "13 build artifact not copied" '[ ! -e "${DST}/src/x120x.o" ]'
assert "13 go-w stripped"          '[ -z "$(find "${DST}" -perm /022)" ]'
install_dkms_tree "${FIX}" "${DST}"
assert "13 rerun replaces cleanly" '[ -f "${DST}/dkms.conf" ]'

# 14 — resolve_battery_settings: per-setting precedence flag > existing
#      conf > default, with warn-and-default on invalid parsed values.
echo "resolve_battery_settings tests"
RBS_SRC=$(sed -n '/^resolve_battery_settings() {/,/^}/p' "${INSTALL_SH}")
[ -n "${RBS_SRC}" ] || { echo "could not extract resolve_battery_settings() from install.sh" >&2; exit 2; }
run_rbs() {  # mah-flag mode-flag board-flag conf-file
    (
        info() { :; }
        warn() { echo "warn:$*"; }
        OPT_MAH="$1"; OPT_CHARGE_MODE="$2"; OPT_BOARD="$3"; OPT_SOC_SOURCE="${5:-}"
        eval "${RBS_SRC}"
        resolve_battery_settings "$4"
        echo "V=${INPUT_MAH}/${CONSERVATION_DEFAULT}/${BOARD_VARIANT}"
        echo "S=${MAH_SRC}|${CONS_SRC}|${BOARD_SRC}"
        # set -u would abort here if the function left it unset (the
        # summary at the end of install.sh reads it)
        echo "M=${CHARGE_MODE_DEFAULT}"
        echo "SOC=${SOC_SOURCE}|${SOC_SRC}"
    ) 2>&1
}

CONF="${WORK}/x120x.conf"
printf '# comment\noptions x120x battery_mah=20000 conservation_mode_default=1 board=x120x\n' > "${CONF}"
out=$(run_rbs "" "" "" "${CONF}")
assert "14 conf, no flags: all preserved"  'printf "%s" "'"$out"'" | grep -q "V=20000/1/x120x"'
assert "14 conf, no flags: mode derived (longlife)" 'printf "%s" "'"$out"'" | grep -q "M=longlife"'
assert "14 conf, no flags: sources say kept" '[ "$(printf "%s" "'"$out"'" | grep -c "kept from existing")" -ge 0 ] && printf "%s" "'"$out"'" | grep -q "S=kept from existing configuration|kept from existing configuration|kept from existing configuration"'

out=$(run_rbs "6000" "" "" "${CONF}")
assert "14 one flag: mah overridden, rest kept" 'printf "%s" "'"$out"'" | grep -q "V=6000/1/x120x"'
assert "14 one flag: mah source is the flag"    'printf "%s" "'"$out"'" | grep -q "S=from --battery-mah|kept"'

out=$(run_rbs "" "" "" "${WORK}/no-such-conf")
assert "14 no conf, no flags: defaults" 'printf "%s" "'"$out"'" | grep -q "V=1000/0/x120x"'
assert "14 no conf: mode set in Fast (set -u summary read)" 'printf "%s" "'"$out"'" | grep -q "M=fast"'

printf 'options x120x battery_mah=abc conservation_mode_default=1 board=x120x\n' > "${CONF}"
out=$(run_rbs "" "" "" "${CONF}")
assert "14 invalid mah: warns naming value" 'printf "%s" "'"$out"'" | grep -q "warn:.*battery_mah=.abc."'
assert "14 invalid mah: default, others kept" 'printf "%s" "'"$out"'" | grep -q "V=1000/1/x120x"'

printf 'options x120x battery_mah=12000 conservation_mode_default=0\n' > "${CONF}"
out=$(run_rbs "" "" "" "${CONF}")
assert "14 old conf, no board key: board defaults, rest kept" 'printf "%s" "'"$out"'" | grep -q "V=12000/0/x120x"'
assert "14 old conf: board source is default" 'printf "%s" "'"$out"'" | grep -q "|default$"'

# soc_source resolution (voltage OCV model; precedence flag > conf > default)
printf 'options x120x battery_mah=20000 conservation_mode_default=0 board=x120x\n' > "${CONF}"
out=$(run_rbs "" "" "" "${CONF}")
assert "14 soc_source: default voltage when absent" 'printf "%s" "'"$out"'" | grep -q "SOC=voltage|default"'
out=$(run_rbs "" "" "" "${CONF}" "gauge")
assert "14 soc_source: flag overrides"              'printf "%s" "'"$out"'" | grep -q "SOC=gauge|from --soc-source"'
printf 'options x120x battery_mah=20000 soc_source=gauge board=x120x\n' > "${CONF}"
out=$(run_rbs "" "" "" "${CONF}")
assert "14 soc_source: kept from existing conf"     'printf "%s" "'"$out"'" | grep -q "SOC=gauge|kept from existing"'
printf 'options x120x battery_mah=20000 soc_source=bogus board=x120x\n' > "${CONF}"
out=$(run_rbs "" "" "" "${CONF}")
assert "14 soc_source: invalid falls back to voltage" 'printf "%s" "'"$out"'" | grep -q "SOC=voltage|default"'
assert "14 soc_source: invalid warns naming value"    'printf "%s" "'"$out"'" | grep -q "warn:.*soc_source=.bogus."'

# The install script runs under set -euo pipefail, so the function
# must return 0 on every path — a trailing `[ cond ] && x` made it
# return 1 in Fast mode and killed the installer silently after
# Step 4 (caught by the v0.4.8 Phase-2 hardware validation, missed
# here because this harness runs without -e).  Drive it under the
# caller's real shell options.
run_rbs_set_e() {  # conf-file
    (
        set -euo pipefail
        info() { :; }; warn() { :; }
        OPT_MAH=""; OPT_CHARGE_MODE=""; OPT_BOARD=""; OPT_SOC_SOURCE=""
        eval "${RBS_SRC}"
        resolve_battery_settings "$1"
        echo "survived"
    ) 2>&1
}
printf 'options x120x battery_mah=20000 conservation_mode_default=0 board=x120x\n' > "${CONF}"
out=$(run_rbs_set_e "${CONF}"); rc=$?
assert "14 set -e, Fast mode: function returns 0"      '[ "'"$rc"'" -eq 0 ]'
assert "14 set -e, Fast mode: caller survives"         'printf "%s" "'"$out"'" | grep -q "survived"'
out=$(run_rbs_set_e "${WORK}/no-such-conf"); rc=$?
assert "14 set -e, defaults path: caller survives"     '[ "'"$rc"'" -eq 0 ]'

# 15 — cell geometry (capacity from cells × per-cell) + derived/overridden R.
echo "cell-geometry + pack-resistance tests"
run_cells() {  # cells cell-mah cell-size resistance battery-mah conf
    (
        set -euo pipefail
        info() { :; }; warn() { echo "warn:$*"; }
        die()  { echo "die:$*"; exit 3; }
        OPT_MAH="${5:-}"; OPT_CHARGE_MODE=""; OPT_BOARD=""; OPT_SOC_SOURCE=""
        OPT_CELLS="${1:-}"; OPT_CELL_MAH="${2:-}"; OPT_CELL_SIZE="${3:-}"; OPT_RESISTANCE="${4:-}"
        eval "${RBS_SRC}"
        resolve_battery_settings "${6:-${WORK}/no-such-conf}"
        echo "MAH=${INPUT_MAH}|${MAH_SRC}"
        echo "RES=${PACK_RESISTANCE:-none}|${RES_SRC}"
    ) 2>&1
}
out=$(run_cells 4 5000 21700 "" "" )
assert "15 4×5000 21700: capacity 20000" 'printf "%s" "'"$out"'" | grep -q "MAH=20000|from 4×5000"'
assert "15 4P: derived R = 30 (matches built-in)" 'printf "%s" "'"$out"'" | grep -q "RES=30|derived"'
out=$(run_cells 2 3000 21700 "" "" )
assert "15 2×3000: capacity 6000" 'printf "%s" "'"$out"'" | grep -q "MAH=6000|"'
assert "15 2P: derived R = 35" 'printf "%s" "'"$out"'" | grep -q "RES=35|derived"'
out=$(run_cells 1 "" 21700 "" "" )
assert "15 1 cell, default 21700 mAh: R = 45" 'printf "%s" "'"$out"'" | grep -q "RES=45|derived"'
out=$(run_cells 4 3000 18650 "" "" )
assert "15 18650 default range accepts 3000" 'printf "%s" "'"$out"'" | grep -q "MAH=12000|"'
out=$(run_cells "" 6500 18650 "" "" )
assert "15 cell-mah 6500 invalid for 18650: dies" 'printf "%s" "'"$out"'" | grep -q "die:.*out of range for a 18650"'
out=$(run_cells "" "" "" 50 "" )
assert "15 --pack-resistance-mohm 50 overrides" 'printf "%s" "'"$out"'" | grep -q "RES=50|from --pack-resistance"'
out=$(run_cells "" "" "" "" 12000 )
assert "15 --battery-mah wins, no cells: R unset (built-in)" 'printf "%s" "'"$out"'" | grep -q "MAH=12000|from --battery-mah"'
assert "15 no cells/override: R none => driver default" 'printf "%s" "'"$out"'" | grep -q "RES=none|driver built-in"'
printf 'options x120x battery_mah=8000 pack_resistance_mohm=42 board=x120x\n' > "${CONF}"
out=$(run_cells "" "" "" "" "" "${CONF}")
assert "15 conf pack_resistance kept on bare re-run" 'printf "%s" "'"$out"'" | grep -q "RES=42|kept from existing"'

echo
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
