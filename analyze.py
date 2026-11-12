#!/usr/bin/env python3
"""
analyze.py - gera as tabelas-resumo e os graficos do relatorio a partir dos CSVs.

Uso, na pasta dos CSVs (padrao: a pasta deste script):
    python3 analyze.py [pasta]

Arquivos usados, quando existirem (gerados por run_all.sh):
    a_principal.csv  Cenario A, Dell em loopback          Tabela 8  e Figura 2
    a_bloco.csv      Cenario A, tamanho do bloco          Tabela 9
    a_recv.csv       Cenario A, buffer de recv()          Tabela 10
    a_ajuste.csv     Cenario A, ajuste para 10 clientes   Tabela 11
    b_principal.csv  Cenario B, Galaxy em loopback        Tabela 12 e Figura 3
    b_ajuste.csv     Cenario B, ajuste para 10 clientes   Tabela 13
    c_principal.csv  Cenario C, rede local (servidor)     Tabela 14 e Figura 4
    c_clientes.csv   Cenario C, bytes enviados pelos clientes (validacao)
    Cenarios A, B e C juntos                              Tabela 15 e Figura 5
Outros CSVs gerados por run_experiments.sh tambem sao resumidos.

Os graficos sao gravados como figura2_cenario_a.png, figura3_cenario_b.png,
figura4_cenario_c.png e figura5_comparacao.png e usam o matplotlib
(no Ubuntu: sudo apt install python3-matplotlib).
"""

import csv
import glob
import os
import statistics
import sys

FOLDER = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))

# letra: (titulo do grafico, nome na Figura 5, arquivo do grafico, numero da tabela)
SCENARIOS = {
    "a": ("Cenário A - Dell em loopback", "A - Dell / loopback", "figura2_cenario_a.png", 8),
    "b": ("Cenário B - Galaxy em loopback", "B - Galaxy / loopback", "figura3_cenario_b.png", 12),
    "c": ("Cenário C - comunicação pela rede local", "C - rede local",
          "figura4_cenario_c.png", 14),
}
KNOWN = ["a_principal.csv", "a_bloco.csv", "a_recv.csv", "a_ajuste.csv",
         "b_principal.csv", "b_ajuste.csv", "c_principal.csv", "c_clientes.csv"]

DEFAULT_CONFIG = (131072, 262144)   # bloco no cliente, recv() no servidor
TUNED_CONFIG = (65536, 16384)

# Azul, laranja e verde (as mesmas tres cores em todos os graficos).
COLORS = ["#2a78d6", "#eb6834", "#1baf7a"]
MARKERS = ["o", "s", "^"]


# ---------------------------------------------------------------- leitura

def load(name):
    path = os.path.join(FOLDER, name)
    if not os.path.exists(path):
        return None
    with open(path, newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def num(row, key):
    try:
        return float(row[key])
    except (KeyError, TypeError, ValueError):
        return None


def summarize(rows):
    """Estatisticas das repeticoes de uma mesma configuracao."""
    valid = [r for r in rows if num(r, "mbps") is not None]
    if not valid:
        return None

    def values(key, keep):
        return [v for v in (num(r, key) for r in valid) if v is not None and keep(v)]

    mbps = values("mbps", lambda v: True)
    peak = values("pico_1s", lambda v: v > 0)
    cpu = values("cpu", lambda v: v >= 0)
    jain = values("jain", lambda v: v > 0)
    window = values("janela_s", lambda v: v > 0)

    return {
        "runs": len(mbps),
        "failed": len(rows) - len(valid),
        "max": max(mbps),
        "mean": statistics.mean(mbps),
        "sd": statistics.stdev(mbps) if len(mbps) > 1 else 0.0,
        "min": min(mbps),
        "peak": max(peak) if peak else None,
        "cpu": statistics.mean(cpu) if cpu else None,
        "jain": statistics.mean(jain) if jain else None,
        "window": statistics.mean(window) if window else None,
        "ok": sum(1 for r in valid if r.get("bytes_ok") == "1"),
    }


def group(rows, keys):
    """Agrupa as execucoes pelas colunas indicadas e resume cada grupo."""
    groups = {}
    for r in rows:
        try:
            key = tuple(int(float(r[k])) for k in keys)
        except (KeyError, TypeError, ValueError):
            continue
        groups.setdefault(key[0] if len(key) == 1 else key, []).append(r)
    result = {}
    for key in sorted(groups):
        s = summarize(groups[key])
        if s:
            result[key] = s
    return result


def validate_network(server_rows, client_rows):
    """Completa bytes_cli e bytes_ok do Cenario C com o CSV dos clientes."""
    sent = {(r["clientes"], r["rep"], r["bloco"], r["recv"]): r for r in client_rows}
    for r in server_rows:
        c = sent.get((r["clientes"], r["rep"], r["bloco"], r["recv"]))
        if c is None:
            continue
        r["bytes_cli"] = c.get("bytes_cli", "")
        same = r.get("bytes_srv") and r["bytes_srv"] == r["bytes_cli"]
        r["bytes_ok"] = "1" if same and c.get("bytes_ok") == "1" else "0"


# ---------------------------------------------------------------- formatacao

def br(x, decimals=0):
    """Numero no formato brasileiro: 12.345,6."""
    if x is None:
        return "-"
    text = f"{x:,.{decimals}f}"
    return text.replace(",", "_").replace(".", ",").replace("_", ".")


def size(nbytes):
    if nbytes >= 1048576 and nbytes % 1048576 == 0:
        return f"{nbytes // 1048576} MiB"
    if nbytes >= 1024 and nbytes % 1024 == 0:
        return f"{nbytes // 1024} KiB"
    return "1 byte" if nbytes == 1 else f"{nbytes} bytes"


def pct(new, base):
    return f"{br(100.0 * (new - base) / base, 1)}%"


def gbps(mbps, clients=None):
    text = f"{br(mbps / 1000, 1)} Gbps"
    return f"{text} ({clients} cliente{'s' if clients > 1 else ''})" if clients else text


def print_table(title, header, rows):
    widths = [max(len(str(c)) for c in col) for col in zip(header, *rows)]

    def line(cells):
        return "  ".join(str(c).ljust(w) if i == 0 else str(c).rjust(w)
                         for i, (c, w) in enumerate(zip(cells, widths)))

    print(f"\n{title}")
    print(line(header))
    print("-" * len(line(header)))
    for row in rows:
        print(line(row))


# ---------------------------------------------------------------- tabelas

def table_scenario(letter, summ, validated=True):
    title, _, _, number = SCENARIOS[letter]
    d = 1 if max(s["mean"] for s in summ.values()) < 1000 else 0
    header = ["Clientes", "Máximo", "Média", "Desvio", "Mínimo", "Pico 1 s", "Por cliente"]
    header += ["Janela T (s)"] if letter == "c" else []
    header += ["CPU (%)", "Jain"]
    rows = []
    for n, s in summ.items():
        row = [n, br(s["max"], d), br(s["mean"], d), br(s["sd"], d), br(s["min"], d),
               br(s["peak"], d), br(s["mean"] / n, d)]
        row += [br(s["window"], 2)] if letter == "c" else []
        row += [br(s["cpu"], 1), br(s["jain"], 4)]
        rows.append(row)
    print_table(f"Tabela {number} - Resultados do {title} (Mbps)", header, rows)

    runs = sum(s["runs"] for s in summ.values())
    failed = sum(s["failed"] for s in summ.values())
    if validated:
        ok = sum(s["ok"] for s in summ.values())
        print(f"Bytes conferidos (servidor = soma dos clientes): {ok} de {runs} execuções")
    else:
        print("Bytes não conferidos: copie c_clientes.csv da máquina dos clientes para esta pasta")
    if failed:
        print(f"Execuções sem resultado, ignoradas: {failed}")


def table_block(summ):
    rows = [[size(b), br(s["mean"] / 1000, 1), br(s["sd"] / 1000, 1),
             br(s["min"] / 1000, 1), br(s["max"] / 1000, 1), br(s["cpu"], 1)]
            for (n, b, r), s in summ.items()]
    print_table("Tabela 9 - Efeito do tamanho do bloco send() no Cenário A, 10 clientes (Gbps)",
                ["Bloco", "Média", "Desvio", "Mínimo", "Máximo", "CPU (%)"], rows)


def table_recv(summ):
    by_recv = {}
    for (n, b, r), s in summ.items():
        by_recv.setdefault(r, {})[n] = s
    counts = sorted({n for (n, b, r) in summ})
    few, many = counts[0], counts[-1]
    rows = []
    for r in sorted(by_recv):
        g = by_recv[r]
        label = size(r) + {1: " - chat original", 262144: " - padrão"}.get(r, "")
        rows.append([label,
                     br(g[few]["mean"]) if few in g else "-",
                     br(g[many]["mean"]) if many in g else "-",
                     br(g[many]["cpu"], 1) if many in g else "-"])
    print_table("Tabela 10 - Efeito do buffer de recv() no Cenário A (Mbps)",
                ["Buffer por recv()", f"{few} cliente(s)", f"{many} clientes",
                 f"CPU com {many} clientes (%)"], rows)


def tuning_by_config(summ):
    return {(b, r): s for (n, b, r), s in summ.items()}


def table_tuning(letter, summ, number):
    configs = tuning_by_config(summ)
    order = sorted(configs, key=lambda k: configs[k]["mean"], reverse=True)
    rows = []
    for b, r in order:
        s = configs[(b, r)]
        label = f"{size(b)} / {size(r)}" + (" - padrão" if (b, r) == DEFAULT_CONFIG else "")
        rows.append([label, br(s["mean"]), br(s["sd"]), br(s["min"]), br(s["max"]),
                     br(s["cpu"], 1)])
    print_table(f"Tabela {number} - Ajuste para 10 clientes no {SCENARIOS[letter][0]} (Mbps)",
                ["Bloco cliente / recv() servidor", "Média", "Desvio", "Mínimo",
                 "Máximo", "CPU (%)"], rows)

    best, default = configs[order[0]], configs.get(DEFAULT_CONFIG)
    if default and order[0] != DEFAULT_CONFIG:
        relation = "superou" if best["min"] > default["max"] else "não superou"
        print(f"Ganho de {size(order[0][0])} / {size(order[0][1])} sobre o padrão: "
              f"+{br(100 * (best['mean'] / default['mean'] - 1), 1)}%; o menor resultado "
              f"({br(best['min'])} Mbps) {relation} o maior do padrão "
              f"({br(default['max'])} Mbps)")


def table_comparison(a, b, tuning_a, tuning_b):
    def best(summ, key):
        n = max(summ, key=lambda k: summ[k][key])
        return summ[n][key], n

    rows = []
    (va, na), (vb, nb) = best(a, "max"), best(b, "max")
    rows.append(["Throughput máximo", gbps(va, na), gbps(vb, nb), pct(vb, va)])
    (va, na), (vb, nb) = best(a, "mean"), best(b, "mean")
    rows.append(["Maior média", gbps(va, na), gbps(vb, nb), pct(vb, va)])
    for n in (1, 10):
        if n in a and n in b:
            rows.append([f"Média com {n} cliente{'s' if n > 1 else ''}", gbps(a[n]["mean"]),
                         gbps(b[n]["mean"]), pct(b[n]["mean"], a[n]["mean"])])
    if tuning_a and tuning_b:
        ta = tuning_by_config(tuning_a).get(TUNED_CONFIG)
        tb = tuning_by_config(tuning_b).get(TUNED_CONFIG)
        if ta and tb:
            rows.append(["Média com 10 clientes, configuração ajustada", gbps(ta["mean"]),
                         gbps(tb["mean"]), pct(tb["mean"], ta["mean"])])
    if 10 in a and 10 in b and a[10]["cpu"] is not None and b[10]["cpu"] is not None:
        diff = b[10]["cpu"] - a[10]["cpu"]
        rows.append(["CPU com 10 clientes", f"{br(a[10]['cpu'], 1)}%", f"{br(b[10]['cpu'], 1)}%",
                     f"{'+' if diff >= 0 else ''}{br(diff, 1)} ponto percentual"])
    print_table("Tabela 15 - Comparação dos resultados em loopback",
                ["Indicador", "Cenário A - Dell", "Cenário B - Galaxy",
                 "Diferença de B em relação a A"], rows)

    diffs = [100.0 * (b[n]["mean"] - a[n]["mean"]) / a[n]["mean"] for n in a if n in b]
    print(f"Média de B em relação a A, por quantidade de clientes: entre {br(min(diffs), 1)}% "
          f"e {br(max(diffs), 1)}% (média de {br(statistics.mean(diffs), 1)}%)")
    if 1 in a and 2 in a and 1 in b and 2 in b:
        print(f"De 1 para 2 clientes, a média aumenta {br(a[2]['mean'] / a[1]['mean'], 2)} vezes "
              f"no Cenário A e {br(b[2]['mean'] / b[1]['mean'], 2)} vezes no Cenário B")


def other_csvs():
    known = set(KNOWN)
    printed = 0
    for path in sorted(glob.glob(os.path.join(FOLDER, "*.csv"))):
        name = os.path.basename(path)
        if name in known:
            continue
        summ = group(load(name), ("clientes", "bloco", "recv"))
        if not summ:
            continue
        rows = [[n, size(b), size(r), br(s["mean"]), br(s["sd"]), br(s["min"]), br(s["max"]),
                 br(s["peak"]), br(s["cpu"], 1), br(s["jain"], 4), f"{s['ok']}/{s['runs']}"]
                for (n, b, r), s in summ.items()]
        print_table(f"{name} (Mbps)",
                    ["Clientes", "Bloco", "recv()", "Média", "Desvio", "Mínimo", "Máximo",
                     "Pico 1 s", "CPU (%)", "Jain", "Bytes ok"], rows)
        printed += 1
    return printed


# ---------------------------------------------------------------- graficos

def finish(plt, fig, ax, title, xs, name):
    ax.set_title(title, pad=10)
    ax.set_xlabel("Número de clientes simultâneos")
    ax.set_ylabel("Throughput agregado (Mbps)")
    ax.set_xticks(xs)
    ax.grid(True, linestyle=":", linewidth=0.8)
    ax.legend(frameon=False, fontsize=8)
    fig.tight_layout()
    path = os.path.join(FOLDER, name)
    fig.savefig(path, dpi=200, bbox_inches="tight")
    plt.close(fig)
    print(f"Gráfico gravado: {name}")


def figure_scenario(plt, letter, summ):
    """Figuras 2, 3 e 4: media, minimo e pico em 1 s por quantidade de clientes."""
    title, _, name, _ = SCENARIOS[letter]
    xs = list(summ)
    series = [("Média", "mean"), ("Mínimo", "min"), ("Pico 1 s", "peak")]
    fig, ax = plt.subplots(figsize=(7.68, 3.925))
    for i, (label, key) in enumerate(series):
        ys = [summ[n][key] if summ[n][key] is not None else float("nan") for n in xs]
        ax.plot(xs, ys, marker=MARKERS[i], color=COLORS[i], linewidth=2, markersize=5,
                label=label)
    finish(plt, fig, ax, title, xs, name)


def figure_comparison(plt, mains):
    """Figura 5: media dos tres cenarios, em escala logaritmica."""
    fig, ax = plt.subplots(figsize=(7.68, 3.925))
    xs = []
    for i, letter in enumerate("abc"):
        if letter in mains:
            summ = mains[letter]
            xs = sorted(set(xs) | set(summ))
            ax.plot(list(summ), [s["mean"] for s in summ.values()], marker=MARKERS[i],
                    color=COLORS[i], linewidth=2, markersize=5, label=SCENARIOS[letter][1])
    ax.set_yscale("log")
    finish(plt, fig, ax, "Throughput médio por cenário", xs, "figura5_comparacao.png")


def make_figures(mains):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        plt.style.use("default")     # estilo padrao do matplotlib
        plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 10})
    except ImportError:
        print("\nmatplotlib não encontrado; gráficos não gerados "
              "(no Ubuntu: sudo apt install python3-matplotlib).")
        return
    print()
    for letter, summ in mains.items():
        figure_scenario(plt, letter, summ)
    if len(mains) >= 2:
        figure_comparison(plt, mains)


# ---------------------------------------------------------------- principal

def main():
    data = {name: load(name) for name in KNOWN}

    if data["c_principal.csv"] and data["c_clientes.csv"]:
        validate_network(data["c_principal.csv"], data["c_clientes.csv"])

    mains = {}
    for letter in "abc":
        rows = data[f"{letter}_principal.csv"]
        summ = group(rows, ("clientes",)) if rows else {}
        if summ:
            mains[letter] = summ

    tuning = {letter: group(data[f"{letter}_ajuste.csv"], ("clientes", "bloco", "recv"))
              for letter in "ab" if data[f"{letter}_ajuste.csv"]}

    if "a" in mains:
        table_scenario("a", mains["a"])
    if data["a_bloco.csv"]:
        table_block(group(data["a_bloco.csv"], ("clientes", "bloco", "recv")))
    if data["a_recv.csv"]:
        table_recv(group(data["a_recv.csv"], ("clientes", "bloco", "recv")))
    if tuning.get("a"):
        table_tuning("a", tuning["a"], 11)
    if "b" in mains:
        table_scenario("b", mains["b"])
    if tuning.get("b"):
        table_tuning("b", tuning["b"], 13)
    if "c" in mains:
        table_scenario("c", mains["c"], validated=bool(data["c_clientes.csv"]))
    if "a" in mains and "b" in mains:
        table_comparison(mains["a"], mains["b"], tuning.get("a"), tuning.get("b"))
    if "c" in mains:
        top_c = max(s["max"] for s in mains["c"].values())
        for letter in "ab":
            if letter in mains:
                top = max(s["max"] for s in mains[letter].values())
                print(f"Throughput máximo do Cenário {letter.upper()}: {br(top / top_c)} vezes "
                      f"o do Cenário C ({br(top_c, 1)} Mbps)")

    others = other_csvs()

    if not mains and not tuning and not others:
        print("Nenhum resultado encontrado. Execute ./run_all.sh antes.")
    elif mains:
        make_figures(mains)


if __name__ == "__main__":
    main()
