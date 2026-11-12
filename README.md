# Throughput máximo do chat TCP

Redes de Computadores

José Augusto da C. Caldeira e Rafael da Rocha de Oliveira Rodrigues.

A aplicação de chat TCP da disciplina foi modificada para medir a capacidade máxima de transmissão de dados dos clientes para o servidor, com 1 a 10 clientes simultâneos. O servidor mantém a arquitetura original, com uma thread por cliente conectado, e o modo chat continua funcionando como antes.

## Arquivos

| Arquivo | Função |
| --- | --- |
| `chat_server.c` | Servidor TCP com modo benchmark (opção `-n`) e atendimento simultâneo de vários clientes |
| `chat_client.c` | Cliente TCP com modo de transmissão contínua durante um intervalo definido (opção `-t`) |
| `Makefile` | Compila os dois programas |
| `run_experiments.sh` | Executa uma bateria de testes e grava os resultados em CSV |
| `run_all.sh` | Automatiza os experimentos principais e complementares de cada cenário |
| `sysinfo.sh` | Registra hardware, sistema operacional, compilador e rede |
| `analyze.py` | Gera as tabelas-resumo e os gráficos a partir dos CSVs |

## Requisitos

Linux, ou Windows com WSL 2, com `gcc`, `make` e `python3`. Os gráficos usam o `matplotlib`. No Ubuntu:

```
sudo apt install build-essential python3-matplotlib
```

## Compilação

```
make
```

Se os scripts perderem a permissão de execução ao baixar o repositório, use `chmod +x *.sh`.

## Uso

Sem opções, os programas funcionam como o chat original:

```
./chat_server 5000
./chat_client 127.0.0.1 5000
```

Modo benchmark:

```
./chat_server <porta> -n <clientes> [-r bytes_recv] [-i intervalo_ms] [-o amostras.csv] [-q]
./chat_client <IP> <porta> -t <segundos> [-b bytes_bloco] [-n nome]
```

Exemplo com 3 clientes transmitindo por 10 s, em terminais separados:

```
./chat_server 5000 -n 3
./chat_client 127.0.0.1 5000 -t 10 -n c1   # repetir com c2 e c3
```

O servidor só envia o sinal de início (GO) quando os N clientes estão conectados e, ao final, imprime uma linha `RESULT` com o total de bytes, a janela de medição, o throughput agregado, o pico em 1 s, a ocupação de CPU e o índice de Jain.

## Experimentos

| Comando | Máquina | Experimentos | Duração |
| --- | --- | --- | --- |
| `./run_all.sh` | Dell (Cenário A) | Principal, bloco, `recv()` e ajuste | cerca de 22 min |
| `./run_all.sh B` | Galaxy (Cenário B) | Principal e ajuste | cerca de 13 min |
| `./run_all.sh C servidor` | Galaxy, iniciado primeiro | Cenário C, lado do servidor | cerca de 11 min |
| `./run_all.sh C clientes <IP>` | Dell | Cenário C, lado dos clientes | |

Para executar só o experimento principal, com cinco repetições de 10 s:

```
REPS=5 DUR=10 ./run_experiments.sh
```

Depois, com todos os CSVs na mesma pasta:

```
python3 analyze.py
```

O script imprime as tabelas-resumo e grava os gráficos `figura2_cenario_a.png`, `figura3_cenario_b.png`, `figura4_cenario_c.png` e `figura5_comparacao.png`.

## Execução entre duas máquinas (Cenário C)

1. Na máquina do servidor, criar ou editar `%UserProfile%\.wslconfig` com o conteúdo abaixo e reiniciar o WSL com `wsl --shutdown`:

   ```
   [wsl2]
   networkingMode=mirrored
   ```

2. Ainda na máquina do servidor, liberar a porta 5000 no firewall do Hyper-V (PowerShell como administrador):

   ```
   New-NetFirewallHyperVRule -Name "ChatTCP" -DisplayName "Chat TCP 5000" -Direction Inbound -Protocol TCP -LocalPorts 5000 -VMCreatorId '{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}'
   ```

3. Na máquina do servidor, executar `./run_all.sh C servidor`. O script mostra o IP da máquina.

4. Na máquina dos clientes, executar `./run_all.sh C clientes <IP do servidor>`.

5. Copiar `c_clientes.csv` para a pasta da máquina do servidor e executar `python3 analyze.py`, que confere os bytes enviados com os recebidos.
