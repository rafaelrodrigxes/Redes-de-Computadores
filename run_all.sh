#!/usr/bin/env bash
# run_all.sh - executa a bateria automatizada de experimentos de um cenario.
#
#   ./run_all.sh                   Cenario A (padrao), Dell em loopback:
#                                  principal, bloco, recv() e ajuste (cerca de 22 min)
#   ./run_all.sh B                 Cenario B, Galaxy em loopback:
#                                  principal e ajuste (cerca de 13 min)
#   ./run_all.sh C servidor        Cenario C, na maquina do servidor (Galaxy),
#                                  iniciado primeiro (cerca de 11 min)
#   ./run_all.sh C clientes <IP>   Cenario C, na maquina dos clientes (Dell)
#
# Arquivos gerados:
#   a_principal.csv  a_bloco.csv  a_recv.csv  a_ajuste.csv  a_sysinfo.txt
#   b_principal.csv  b_ajuste.csv  b_sysinfo.txt
#   c_principal.csv  c_servidor_sysinfo.txt   (maquina do servidor)
#   c_clientes.csv   c_clientes_sysinfo.txt   (maquina dos clientes)
#
# Com todos os CSVs na mesma pasta, "python3 analyze.py" gera as tabelas e os
# graficos. Variaveis como REPS, DUR e PORT sao repassadas a run_experiments.sh.
set -euo pipefail
cd "$(dirname "$0")"

SCENARIO=$(echo "${1:-A}" | tr 'abc' 'ABC')

usage() {
  sed -n '3,10p' "$0" | sed 's/^# \{0,1\}//' >&2
  exit 1
}

make -s --no-print-directory

# Ajuste para 10 clientes (Secoes 6.4 e 6.6): bloco no cliente : recv() no servidor.
TUNING="65536:16384 4096:16384 131072:262144"

case $SCENARIO in
  A)
    OUT=a_sysinfo.txt bash sysinfo.sh
    TAG=a_principal bash run_experiments.sh
    TAG=a_bloco CLIENTS=10 REPS="${REPS:-3}" \
      CONFIGS="1024:262144 4096:262144 16384:262144 65536:262144 131072:262144 1048576:262144" \
      bash run_experiments.sh
    TAG=a_recv CLIENTS="1 10" REPS="${REPS:-3}" \
      CONFIGS="131072:1 131072:1024 131072:16384 131072:262144" \
      bash run_experiments.sh
    TAG=a_ajuste CLIENTS=10 CONFIGS="$TUNING" bash run_experiments.sh
    ;;
  B)
    OUT=b_sysinfo.txt bash sysinfo.sh
    TAG=b_principal bash run_experiments.sh
    TAG=b_ajuste CLIENTS=10 CONFIGS="$TUNING" bash run_experiments.sh
    ;;
  C)
    case ${2:-} in
      servidor)
        OUT=c_servidor_sysinfo.txt bash sysinfo.sh
        MODO=servidor TAG=c_principal bash run_experiments.sh
        ;;
      clientes)
        [[ -n ${3:-} ]] || usage
        OUT=c_clientes_sysinfo.txt bash sysinfo.sh
        MODO=clientes TAG=c_clientes HOST="$3" bash run_experiments.sh
        echo "Copie c_clientes.csv para a pasta da maquina do servidor."
        exit 0
        ;;
      *) usage ;;
    esac
    ;;
  *) usage ;;
esac

python3 analyze.py
