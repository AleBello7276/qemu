#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  tests/xenon/bootcheck.sh --case CASE --qemu PATH --config PATH [--duration SEC] [--extra "..."] [--record]

Cases:
  normal_cdsha_off   Run with -M xbox360,cd-sha-bypass=off and compare/record baseline
  normal_cdsha_on    Run with -M xbox360,cd-sha-bypass=on  and compare/record baseline

Behavior:
  - Runs QEMU for a fixed duration (default: 20s).
  - Extracts a stable POST timeline from the log.
  - In compare mode (default) diffs against tests/xenon/baseline/${CASE}.postlist.
  - In record mode (--record) overwrites the baseline file.
EOF
}

case_name=""
qemu_bin=""
cfg_path=""
duration=20
extra_args=""
record=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --case) case_name="${2:-}"; shift 2 ;;
    --qemu) qemu_bin="${2:-}"; shift 2 ;;
    --config) cfg_path="${2:-}"; shift 2 ;;
    --duration) duration="${2:-}"; shift 2 ;;
    --extra) extra_args="${2:-}"; shift 2 ;;
    --record) record=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown arg: $1" >&2; usage; exit 2 ;;
  esac
done

if [[ -z "$case_name" || -z "$qemu_bin" || -z "$cfg_path" ]]; then
  usage >&2
  exit 2
fi
if [[ ! -x "$qemu_bin" ]]; then
  echo "QEMU not executable: $qemu_bin" >&2
  exit 2
fi
if [[ ! -f "$cfg_path" ]]; then
  echo "Config not found: $cfg_path" >&2
  exit 2
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
baseline_dir="${repo_root}/tests/xenon/baseline"
mkdir -p "${baseline_dir}"

baseline_file="${baseline_dir}/${case_name}.postlist"
tmp_log="$(mktemp -t xenon-bootcheck-log.XXXXXX)"
tmp_post="$(mktemp -t xenon-bootcheck-post.XXXXXX)"
trap 'rm -f "$tmp_log" "$tmp_post"' EXIT

cdsha="off"
case "$case_name" in
  normal_cdsha_off) cdsha="off" ;;
  normal_cdsha_on) cdsha="on" ;;
  *)
    echo "Unknown case: $case_name" >&2
    exit 2
    ;;
esac

# Force stable log formatting (avoid pretty TTY formatting differences).
machine_opts="xbox360,config=${cfg_path},pretty-post=off,trace-boot=off,cd-sha-bypass=${cdsha}"

set +e
timeout "${duration}s" \
  "${qemu_bin}" \
    -M "${machine_opts}" \
    -display none -serial none \
    ${extra_args} \
  >"${tmp_log}" 2>&1
qemu_rc=$?
set -e

# Extract a stable POST timeline:
#  - accept both "xbox360: POST write ..." and "[xbox360][POST] ..."
#  - strip timestamps and addresses
#  - output: "0xNN DESC..."
awk '
  function emit(code_hex, desc) {
    if (code_hex == "") return;
    post = substr(code_hex, 1, 2);
    if (desc == "") {
      printf("0x%s\n", tolower(post));
    } else {
      gsub(/[[:space:]]+$/, "", desc);
      printf("0x%s %s\n", tolower(post), desc);
    }
  }
  {
    # QEMU info_report style:
    #   ... POST write code=0x................. (DESC) @EA=... host_us=... dpost_us=...
    if (match($0, /POST write code=0x([0-9A-Fa-f]{16})/, m)) {
      code = m[1];
      desc = "";
      if (match($0, /\(([^\)]*)\)/, d)) {
        desc = d[1];
      }
      emit(code, desc);
      next;
    }
    # Pretty-post style:
    #   [xbox360][POST] code=0x................. DESC @EA=...
    if (match($0, /\[xbox360\]\[POST\][^c]*code=0x([0-9A-Fa-f]{16})[[:space:]]+([^@]*)@EA=/, p)) {
      emit(p[1], p[2]);
      next;
    }
  }
' "${tmp_log}" > "${tmp_post}"

if [[ $record -eq 1 ]]; then
  install -m 0644 "${tmp_post}" "${baseline_file}"
  echo "Recorded baseline: ${baseline_file}"
  echo "QEMU exit code: ${qemu_rc}"
  exit 0
fi

if [[ ! -f "${baseline_file}" ]]; then
  echo "Baseline missing: ${baseline_file}" >&2
  echo "Run with --record to create it." >&2
  exit 2
fi

if ! diff -u "${baseline_file}" "${tmp_post}" >/dev/null; then
  echo "POST timeline differs for case=${case_name}" >&2
  diff -u "${baseline_file}" "${tmp_post}" >&2 || true
  echo "QEMU exit code: ${qemu_rc}" >&2
  exit 1
fi

echo "OK: ${case_name} (duration=${duration}s rc=${qemu_rc})"
