#!/usr/bin/env bash
# run_experiments.sh - executa uma bateria de testes do chat TCP e grava <TAG>.csv.
#
# Uso local (clientes e servidor na mesma maquina, pela interface de loopback):
#   REPS=5 DUR=10 ./run_experiments.sh
#
# Uso entre duas maquinas (Cenario C):
#   maquina do servidor, primeiro:  MODO=servidor TAG=c_principal ./run_experiments.sh
#   maquina dos clientes, depois:   MODO=clientes TAG=c_clientes HOST=<IP do servidor> ./run_experiments.sh
#
# Variaveis (padrao entre parenteses):
#   TAG      nome do arquivo gerado, <TAG>.csv                  (principal)
#   REPS     repeticoes de cada configuracao                    (5)
#   DUR      duracao de cada teste, em segundos                 (10)
#   CLIENTS  quantidades de clientes                            (1 2 3 4 5 6 7 8 9 10)
#   CONFIGS  pares bloco:recv, em bytes: bloco por send() no
#            cliente e buffer de recv() no servidor             (131072:262144)
#   MODO     local, servidor ou clientes                        (local)
#   HOST     IP do servidor visto pelos clientes                (127.0.0.1)
#   PORT     porta TCP                                          (5000)
#   PAUSE    pausa entre execucoes, em segundos                 (2)
#
# Cada repeticao executa todas as combinacoes de CLIENTS e CONFIGS em ordem
# embaralhada. Cada execucao usa um servidor novo, com os clientes iniciados
# em paralelo, e a soma dos bytes enviados pelos clientes e comparada com o
# total recebido pelo servidor (coluna bytes_ok). Entre duas maquinas, as
# duas pontas sorteiam a mesma ordem, e o analyze.py faz essa comparacao.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
SERVER="$ROOT/chat_server"
CLIENT="$ROOT/chat_client"

TAG=${TAG:-principal}
REPS=${REPS:-5}
DUR=${DUR:-10}
CLIENTS=${CLIENTS:-"1 2 3 4 5 6 7 8 9 10"}
CONFIGS=${CONFIGS:-"131072:262144"}
MODO=${MODO:-local}
HOST=${HOST:-127.0.0.1}
PORT=${PORT:-5000}
PAUSE=${PAUSE:-2}
CSV="$ROOT/$TAG.csv"

case $MODO in
  local) SEED=${SEED:-$RANDOM} ;;          # ordem diferente a cada bateria
  servidor | clientes) SEED=${SEED:-1} ;;  # mesma ordem nas duas maquinas
  *) echo "MODO deve ser local, servidor ou clientes." >&2; exit 1 ;;
esac

# kv chave "texto" -> valor do campo chave=valor
kv() {
  local field
  for field in $2; do
    if [[ $field == "$1="* ]]; then
      echo "${field#*=}"
      return
    fi
  done
}

# Tempo maximo de uma execucao, em segundos.
limit() {
  awk -v d="$DUR" -v extra="${1:-0}" 'BEGIN { print d * 2 + 30 + extra }'
}

# Combinacoes clientes:bloco:recv da repeticao $1, em ordem embaralhada.
order() {
  python3 -c '
import random, sys
seed, rep, clients, configs = sys.argv[1:5]
combos = [n + ":" + c for n in clients.split() for c in configs.split()]
random.Random(seed + "-" + rep).shuffle(combos)
print(" ".join(combos))' "$SEED" "$1" "$CLIENTS" "$CONFIGS"
}

# Inicia N clientes em paralelo, cada um como um processo separado.
start_clients() {
  local n=$1 block=$2 i
  PIDS=()
  for ((i = 1; i <= n; i++)); do
    timeout "$(limit)" "$CLIENT" "$HOST" "$PORT" -t "$DUR" -b "$block" -n "c$i" \
      >"$TMP/c$i.out" 2>"$TMP/c$i.err" &
    PIDS+=("$!")
  done
}

# Espera os clientes e soma os bytes que cada um informou ter enviado.
wait_clients() {
  local n=$1 pid i bytes
  for pid in "${PIDS[@]}"; do
    wait "$pid" || CLI_OK=0
  done
  for ((i = 1; i <= n; i++)); do
    bytes=$(kv bytes "$(grep -m1 '^CLIENT' "$TMP/c$i.out" || true)")
    if [[ -z $bytes ]]; then
      CLI_OK=0
      bytes=0
    fi
    CLI_BYTES=$((CLI_BYTES + bytes))
  done
}

make -C "$ROOT" -s --no-print-directory

if [[ -s $CSV ]]; then
  backup="$CSV.$(date +%Y%m%d-%H%M%S).bak"
  mv "$CSV" "$backup"
  echo "Arquivo anterior preservado em $(basename "$backup")"
fi
echo "tag,clientes,rep,bloco,recv,dur,mbps,pico_1s,cpu,jain,janela_s,bytes_srv,bytes_cli,bytes_ok" > "$CSV"

read -r -a clients <<<"$CLIENTS"
read -r -a configs <<<"$CONFIGS"
total=$((REPS * ${#clients[@]} * ${#configs[@]}))
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
k=0
first=1

echo "$TAG.csv: $total execucoes de $DUR s (modo $MODO)"
if [[ $MODO == servidor ]]; then
  echo "Enderecos IPv4 desta maquina: $(hostname -I 2>/dev/null)"
  echo "Agora inicie os clientes na outra maquina."
fi

for ((rep = 1; rep <= REPS; rep++)); do
  for combo in $(order "$rep"); do
    IFS=: read -r n block recv <<<"$combo"
    k=$((k + 1))
    SRV_OK=1 CLI_OK=1 CLI_BYTES=0
    mbps="" peak="" cpu="" jain="" window="" bytes_srv="" bytes_cli="" bytes_ok=""
    rm -f "$TMP"/*

    case $MODO in
      local)
        timeout "$(limit)" "$SERVER" "$PORT" -n "$n" -r "$recv" -q \
          >"$TMP/srv.out" 2>"$TMP/srv.err" &
        server_pid=$!
        start_clients "$n" "$block"
        wait_clients "$n"
        wait "$server_pid" || SRV_OK=0
        ;;
      servidor)
        # A primeira execucao espera os clientes sem limite de tempo.
        timeout "$([[ $first == 1 ]] && echo 0 || limit 60)" \
          "$SERVER" "$PORT" -n "$n" -r "$recv" -q \
          >"$TMP/srv.out" 2>"$TMP/srv.err" || SRV_OK=0
        ;;
      clientes)
        start_clients "$n" "$block"
        wait_clients "$n"
        ;;
    esac

    if [[ $MODO != clientes ]]; then
      result=$(grep -m1 '^RESULT' "$TMP/srv.out" || true)
      mbps=$(kv mbps "$result")
      peak=$(kv peak_mbps "$result")
      cpu=$(kv cpu "$result")
      jain=$(kv jain "$result")
      window=$(kv window_s "$result")
      bytes_srv=$(kv bytes "$result")
    fi

    if [[ $MODO == local ]]; then
      bytes_cli=$CLI_BYTES
      bytes_ok=0
      if [[ $SRV_OK == 1 && $CLI_OK == 1 && -n $bytes_srv && $bytes_srv == "$bytes_cli" ]]; then
        bytes_ok=1
      fi
    elif [[ $MODO == clientes ]]; then
      bytes_cli=$CLI_BYTES
      bytes_ok=$CLI_OK
    fi

    echo "$TAG,$n,$rep,$block,$recv,$DUR,$mbps,$peak,$cpu,$jain,$window,$bytes_srv,$bytes_cli,$bytes_ok" >> "$CSV"

    if [[ $MODO == clientes ]]; then
      printf '(%d/%d) rep %d, %2d cliente(s), bloco %s: %s bytes enviados%s\n' \
        "$k" "$total" "$rep" "$n" "$block" "$CLI_BYTES" \
        "$([[ $CLI_OK == 1 ]] || echo ' - FALHA em algum cliente')"
    elif [[ -n $mbps ]]; then
      printf '(%d/%d) rep %d, %2d cliente(s), bloco %s, recv %s: %s Mbps, CPU %s%%%s\n' \
        "$k" "$total" "$rep" "$n" "$block" "$recv" "$mbps" "$cpu" \
        "$([[ $MODO == local ]] && { [[ $bytes_ok == 1 ]] && echo ', bytes conferidos' || echo ', bytes DIVERGENTES'; })"
    else
      printf '(%d/%d) rep %d, %2d cliente(s): FALHOU\n' "$k" "$total" "$rep" "$n" >&2
    fi

    if [[ $SRV_OK != 1 || $CLI_OK != 1 ]]; then
      cat "$TMP"/*.err >&2 2>/dev/null || true
    fi

    first=0
    if [[ $MODO == clientes ]]; then
      # O servidor volta a aceitar conexoes PAUSE s depois de cada execucao;
      # se os clientes chegarem antes, eles tentam de novo por ate 10 s.
      sleep "$(awk -v p="$PAUSE" 'BEGIN { print p + 1 }')"
    else
      sleep "$PAUSE"
    fi
  done
done

echo "Resultados gravados em $TAG.csv"
