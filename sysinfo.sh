#!/usr/bin/env bash
# sysinfo.sh - registra CPU, memoria, sistema operacional, compilador e rede.
# Saida: sysinfo.txt, ou o arquivo indicado em OUT.
# Exemplo: OUT=b_sysinfo.txt ./sysinfo.sh
set -uo pipefail
cd "$(dirname "$0")" || exit 1

OUT=${OUT:-sysinfo.txt}

section() {
  printf '\n== %s ==\n' "$1"
}

# Comandos do Windows, disponiveis no WSL por interoperabilidade.
windows() {
  timeout 30 "$@" 2>/dev/null | tr -d '\r'
}

{
  echo "Coletado em: $(date '+%Y-%m-%d %H:%M:%S %z')"

  section "CPU"
  lscpu 2>/dev/null | grep -E '^(Architecture|CPU\(s\)|Model name|Thread\(s\) per core|Core\(s\) per socket|Socket\(s\)|CPU max MHz|L1d|L1i|L2|L3)' \
    || grep -m1 'model name' /proc/cpuinfo

  section "Memoria"
  free -h
  grep -E '^(MemTotal|SwapTotal)' /proc/meminfo

  section "Sistema operacional"
  uname -a
  grep -E '^(PRETTY_NAME|VERSION)=' /etc/os-release 2>/dev/null
  if grep -qi microsoft /proc/version 2>/dev/null; then
    echo "WSL: sim"
    windows wsl.exe --version | tr -d '\0'
  fi

  section "Compilador e ferramentas"
  gcc --version | head -n1
  make --version | head -n1
  python3 --version

  section "TCP"
  sysctl net.ipv4.tcp_congestion_control net.ipv4.tcp_rmem net.ipv4.tcp_wmem \
    net.core.rmem_max net.core.wmem_max 2>/dev/null

  section "Rede (Linux)"
  if command -v ip >/dev/null 2>&1; then
    ip -4 -brief addr
    ip route | head -n 5
  else
    hostname -I
  fi
  for dev in /sys/class/net/*; do
    name=$(basename "$dev")
    [[ $name == lo ]] && continue
    printf '%s: MTU %s, velocidade %s Mbps\n' "$name" \
      "$(cat "$dev/mtu" 2>/dev/null)" "$(cat "$dev/speed" 2>/dev/null || echo '?')"
  done

  if command -v powershell.exe >/dev/null 2>&1; then
    section "Windows"
    windows powershell.exe -NoProfile -Command \
      "Get-CimInstance Win32_ComputerSystem | Format-List Manufacturer,Model,TotalPhysicalMemory;
       Get-CimInstance Win32_Processor | Format-List Name,NumberOfCores,NumberOfLogicalProcessors,MaxClockSpeed;
       Get-CimInstance Win32_OperatingSystem | Format-List Caption,Version,BuildNumber;
       Get-NetAdapter | Where-Object Status -eq 'Up' | Format-Table -AutoSize Name,InterfaceDescription,LinkSpeed"
  fi

  if command -v netsh.exe >/dev/null 2>&1; then
    section "Wi-Fi (Windows)"
    windows netsh.exe wlan show interfaces
  fi
} > "$OUT" 2>&1

echo "Informacoes gravadas em $OUT"
