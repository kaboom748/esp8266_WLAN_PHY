"""Render the reference firmware timing diagram without accessing hardware."""

import hashlib
import json
import math
import re
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle


ROOT = Path(__file__).resolve().parents[1]
RELEASE = json.loads((ROOT / "RELEASE.json").read_text(encoding="utf-8"))
RF = RELEASE["radio"]
PROTO = RELEASE["protocol"]
TIMING = RELEASE["timing"]


def verify_reference():
    for name, expected in RELEASE["sha256"].items():
        actual = hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
        if actual != expected:
            raise ValueError(f"Reference changed: {name}: {actual}")

    for role, name, rx_channel, tx_channel in ((1, "A", 11, 3), (2, "B", 3, 11)):
        source = (ROOT / f"firmware/Fast{name}/Fast{name}.ino").read_text()
        defines = dict(re.findall(r"^#define\s+(FAST_\w+)\s+(\d+)\s*$", source, re.M))
        required = {
            "FAST_ROLE": role,
            "FAST_FS": RF["sample_rate_hz"],
            "FAST_SPS": RF["samples_per_symbol"],
            "FAST_HALF_US": RF["slot_us"],
            "FAST_CHANNEL": rx_channel,
            "FAST_TX_CHANNEL": tx_channel,
            "FAST_INTEGER": 2,
            "FAST_BPS": 1,
            "FAST_BENCH": 0,
            "FAST_FAULT": 0,
            "FAST_GAIN": RF["rf_gain_code"],
            "FAST_BB_GAIN": RF["bb_gain_code"],
            "FAST_ACCUM": RF["iq_accumulation"] - 1,
            "FAST_QUIET_SDK": 0,
        }
        for key, expected in required.items():
            if int(defines.get(key, -1)) != expected:
                raise ValueError(f"Wrong profile in Fast{name}: {key}")
        if (ROOT / f"firmware/Fast{name}/fast.h").read_bytes() != (ROOT / "fast.h").read_bytes():
            raise ValueError(f"Fast{name}/fast.h differs from the reference")

    source = (ROOT / "fast.h").read_text()
    for code in (
        "WIRE = 18, BITS = WIRE * 8, SYNC_LEN = 24",
        "SYNC_AT = 64 / BPS",
        "CENTER = 13*SPS/2",
        "TX_N = (SYMBOLS + 12) * SPS",
        "(int(n)-int(CENTER-SPS/2))/int(SPS)",
        "MAX_ATTEMPTS = 4",
        "RELEASE_GUARD_US = 3500",
        "RELEASE_JITTER_US = 2000",
        "age+wireUs+1000<Fast::FINAL_LIMIT_US",
    ):
        if code not in source:
            raise ValueError(f"Missing reference formula: {code}")


def calculate():
    fs, sps = RF["sample_rate_hz"], RF["samples_per_symbol"]
    symbols = PROTO["coded_bytes"] * 8 + PROTO["sync_symbols"]
    samples = (symbols + 12) * sps
    center = 13 * sps // 2
    groups = [[] for _ in range(7)]
    for n in range(samples):
        # C++ signed division truncates toward zero, unlike Python's //.
        symbol = math.trunc((n - (center - sps // 2)) / sps)
        if symbol < 0:
            group = 0
        elif symbol < 64:
            group = 1
        elif symbol < 88:
            group = 2
        elif symbol < 104:
            group = 3
        elif symbol < 136:
            group = 4
        elif symbol < 168:
            group = 5
        else:
            group = 6
        groups[group].append(n)
    counts = [len(group) for group in groups]
    assert counts == [11, 129, 48, 32, 64, 64, 12]
    assert samples == RF["frame_samples"] == 360
    assert samples * 1_000_000 / fs == RF["frame_us"] == 1152
    assert RF["cycle_us"] == 2 * RF["slot_us"]
    assert TIMING["release_target_us"] + TIMING["release_jitter_budget_us"] == TIMING["final_limit_us"]
    assert PROTO["max_attempts"] == PROTO["max_retries"] + 1 == 4
    favorable = [1500 + n * RF["cycle_us"] for n in range(4)]
    unfavorable = [4500 + n * RF["cycle_us"] for n in range(4)]
    allowed = lambda age: age + RF["frame_us"] + 1000 < TIMING["final_limit_us"]
    assert all(allowed(age) for age in favorable)
    assert [allowed(age) for age in unfavorable] == [True, True, True, False]
    names = ["bordure_initiale", "w0_w7", "sync", "w8_w9", "crc32", "rs", "bordure_finale"]
    parts = [
        dict(name=name, first_sample=group[0], last_sample=group[-1], samples=len(group),
             start_us=group[0] * 1_000_000 / fs,
             end_us=(group[-1] + 1) * 1_000_000 / fs,
             duration_us=len(group) * 1_000_000 / fs)
        for name, group in zip(names, groups)
    ]
    return dict(version=RELEASE["version"], basis="Calculated firmware schedule, not an RF measurement",
                frame=parts, rf_frame_us=RF["frame_us"], uart_bit_us=1_000_000 / 300,
                uart_character_us=10_000_000 / 300, favorable_attempts_us=favorable,
                unfavorable_attempts_us=unfavorable, wire_latency_measured=False)


TEAL, BLUE, GOLD, PURPLE = "#147F78", "#346DAB", "#E5B54D", "#8A6BA9"
INK, MUTED, GREY, RED = "#202C34", "#5D6870", "#D7DDE0", "#BB413D"


def bar(ax, start, end, y, color, label="", height=0.46, **kwargs):
    patch = Rectangle((start, y - height / 2), end - start, height,
                      facecolor=color, edgecolor="white", linewidth=1.2, **kwargs)
    ax.add_patch(patch)
    if label:
        ax.text((start + end) / 2, y, label, ha="center", va="center",
                color=INK if color == GOLD else "white", fontsize=11, weight="bold")
    return patch


def clean_axis(ax):
    ax.spines[["left", "right", "top"]].set_visible(False)
    ax.spines["bottom"].set_color(GREY)
    ax.tick_params(axis="x", length=4, color=GREY, labelcolor=MUTED)
    ax.set_yticks([])
    ax.set_facecolor("white")


def render(values):
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 12,
                         "text.color": INK, "axes.labelcolor": MUTED,
                         "axes.titleweight": "bold", "axes.unicode_minus": False})
    fig = plt.figure(figsize=(18, 16), facecolor="white")
    fig.text(0.07, 0.965, "MODEM300  /  TRAMES ET TIMINGS", fontsize=25, weight="bold")
    fig.text(0.07, 0.942, "v1.0.0-reference  |  ESP8266 a 160 MHz  |  UART 300 bauds, 8N1, sans controle de flux", color=MUTED, fontsize=13)

    # Panel 1: a shared clock and alternating transmission opportunities.
    ax = fig.add_axes([0.13, 0.742, 0.82, 0.145])
    ax.set_title("1  Deux canaux, une radio par ESP : alternance TDD", loc="left", pad=25, fontsize=17)
    ax.set_xlim(-1.7, 18)
    ax.set_ylim(-0.1, 2.25)
    clean_axis(ax)
    ax.set_xticks([0, 4.5, 9, 13.5, 18], ["0", "4,5", "9", "13,5", "18 ms"])
    for x in (0, 4.5, 9, 13.5, 18):
        ax.axvline(x, color="#ECF0F2", linewidth=1, zorder=0)
    for y, starts, color, label in ((1.4, (0, 9), TEAL, "A > B"), (0.35, (4.5, 13.5), BLUE, "B > A")):
        for start in starts:
            bar(ax, start - 1.4, start, y, GREY)
            bar(ax, start, start + 1.152, y, color, label)
    ax.text(-2.0, 1.4, "Canal 3\n2422 MHz", ha="right", va="center", color=TEAL, weight="bold", clip_on=False)
    ax.text(-2.0, 0.35, "Canal 11\n2462 MHz", ha="right", va="center", color=BLUE, weight="bold", clip_on=False)
    ax.annotate("", (0, 2.03), (9, 2.03), arrowprops=dict(arrowstyle="<->", color=INK))
    ax.text(4.5, 2.12, "Un TX par sens toutes les 9 ms", ha="center", fontsize=12)
    fig.text(0.13, 0.707, "Gris : preparation TX, 1,4 ms. Couleur : emission programmee, 1,152 ms. Le reste sert notamment au RX et au calcul.", fontsize=11, color=MUTED)
    fig.text(0.13, 0.689, "Canaux fixes par sens, y compris les retries. Trames de controle maintenues sans donnees UART. Ce n'est pas du Wi-Fi 802.11.", fontsize=11, color=MUTED)

    # Panel 2: sample-exact, including the first-symbol C++ rounding detail.
    ax = fig.add_axes([0.09, 0.536, 0.86, 0.083])
    ax.set_title("2  Zoom sur une emission : 360 echantillons = 1 152 us", loc="left", pad=48, fontsize=17)
    ax.set_xlim(-30, 1180)
    ax.set_ylim(-0.23, 1.62)
    clean_axis(ax)
    labels = ["", "w0..w7\n64 bits", "SYNC\n24 symboles", "w8..w9\n16 bits", "CRC32\n32 bits", "RS\n32 bits", ""]
    colors = [GREY, TEAL, GOLD, TEAL, PURPLE, BLUE, GREY]
    for part, label, color in zip(values["frame"], labels, colors):
        bar(ax, part["start_us"], part["end_us"], 0.62, color, label, height=0.76)
    ax.annotate("Bordure : 35,2 us", (17.6, 1.02), (15, 1.46),
                arrowprops=dict(arrowstyle="-", color=MUTED), ha="left", fontsize=11)
    ax.annotate("Bordure : 38,4 us", (1132.8, 1.02), (1145, 1.46),
                arrowprops=dict(arrowstyle="-", color=MUTED), ha="right", fontsize=11)
    ticks = [0, 448, 601.6, 704, 908.8, 1113.6]
    ax.set_xticks(ticks, ["0", "448", "601,6", "704", "908,8", "1113,6 us"])
    fig.text(0.09, 0.501, "Le SYNC est au milieu, apres w7. Les 18 octets codes sont blanchis PN9 ; le SYNC ne l'est pas.", fontsize=12)
    fig.text(0.09, 0.481, "10 octets en-tete/donnee + CRC32 (4) + RS (4). Une seule donnee UART par trame DATA. RS(18,14) corrige 2 octets codes.", fontsize=11, color=MUTED)
    fig.text(0.09, 0.462, "312,5 kech/s ; 2 ech/symbole ; 6,4 us/symbole. Premier symbole : 3 ech par arrondi C++ ; bordures avec rampes.", fontsize=11, color=MUTED)

    # Panel 3: illustrative phases, never presented as measured trajectories.
    ax = fig.add_axes([0.13, 0.283, 0.82, 0.118])
    ax.set_title("3  Retries : au plus trois, uniquement s'ils tiennent dans le budget", loc="left", pad=43, fontsize=17)
    ax.set_xlim(0, 35)
    ax.set_ylim(-0.30, 2.35)
    clean_axis(ax)
    ax.set_xticks([0, 9, 18, 27, 35], ["0", "9", "18", "27", "35 ms"])
    target, limit = TIMING["release_target_us"] / 1000, TIMING["final_limit_us"] / 1000
    ax.axvspan(target, limit, color="#DDECE4", zorder=0)
    ax.axvline(limit, color=RED, linewidth=1.6)
    ax.text(target - 0.45, 2.27, "Cible : 29,833 ms", ha="right", fontsize=11, color=TEAL)
    ax.text(limit + 0.28, 2.27, "Limite :\n31,833 ms", ha="left", va="top", fontsize=11, color=RED)
    ax.text(-0.5, 1.43, "Phase\nfavorable", ha="right", va="center", fontsize=12, clip_on=False)
    ax.text(-0.5, 0.37, "Phase\ndefavorable", ha="right", va="center", fontsize=12, clip_on=False)
    for y, key in ((1.43, "favorable_attempts_us"), (0.37, "unfavorable_attempts_us")):
        for index, age_us in enumerate(values[key]):
            x = age_us / 1000
            allowed = age_us + RF["frame_us"] + 1000 < TIMING["final_limit_us"]
            if allowed:
                color = TEAL if index == 3 else RED
                bar(ax, x, x + 1.152, y, color, str(index + 1), height=0.38)
                label = f"{x:g}".replace(".", ",") + " ms"
                ax.text(x + 0.576, y - 0.33, label, ha="center", va="top", fontsize=10)
                ax.text(x + 0.576, y + 0.31, "admissible" if index == 3 else "perdue", ha="center", fontsize=10, color=color)
            else:
                ax.add_patch(Rectangle((x, y - 0.19), 1.152, 0.38, facecolor="none", edgecolor=MUTED, linestyle="--"))
                ax.plot([x, x + 1.152], [y - 0.19, y + 0.19], color=RED, linewidth=1.7)
                ax.text(x - 0.35, y, "#4 non emise", ha="right", va="center", fontsize=11, color=RED)
                ax.text(x + 0.576, y - 0.33, "31,5 ms", ha="center", va="top", fontsize=10)
    fig.text(0.13, 0.249, "Exemples calcules, trois premieres copies perdues. t = 0 : horodatage FIFO source, apres reception de l'octet complet.", fontsize=11, color=MUTED)
    fig.text(0.13, 0.230, "Admission : age au TX + 1,152 ms + 1 ms < 31,833 ms. Le destinataire recontrole l'echeance ; sinon, abandon.", fontsize=12)

    # Panel 4: distinguish baud rate from the requested wire-to-wire bound.
    ax = fig.add_axes([0.09, 0.105, 0.57, 0.059])
    ax.set_title("4  UART : 300 bauds, 8N1", loc="left", pad=26, fontsize=17)
    ax.set_xlim(0, 100 / 3)
    ax.set_ylim(-0.15, 1.0)
    clean_axis(ax)
    for n, label in enumerate(["Start"] + [f"D{i}" for i in range(8)] + ["Stop"]):
        bar(ax, n * 10 / 3, (n + 1) * 10 / 3, 0.45,
            MUTED if n in (0, 9) else TEAL, label, height=0.62)
    ax.set_xticks([0, 100 / 3], ["0", "33,333 ms / caractere"])
    ax.get_xticklabels()[-1].set_ha("right")
    fig.text(0.09, 0.073, "3,333 ms / bit  |  30 octets/s utiles par sens", fontsize=12, color=MUTED)
    fig.text(0.70, 0.153, "Exigence fil a fil : 66,66 ms", fontsize=15, weight="bold")
    fig.text(0.70, 0.130, "Premier bit entrant > premier bit sortant", fontsize=11)
    fig.text(0.70, 0.109, "Borne non mesuree electriquement.", fontsize=12, color=RED, weight="bold")
    fig.text(0.70, 0.089, "Le diagramme decrit le firmware, pas un oscilloscope.", fontsize=10, color=MUTED)
    fig.text(0.07, 0.027, "Reference de banc, 13 octobre 2026  |  Profil release_uart_wait  |  Valeurs et limites : NOTE_TECHNIQUE.md  |  Rendu reproductible : docs/render_timing.py", fontsize=10, color=MUTED)

    fig.canvas.draw()
    renderer = fig.canvas.get_renderer()
    canvas_box = fig.bbox
    for label in fig.findobj(plt.Text):
        if label.get_visible() and label.get_text():
            box = label.get_window_extent(renderer)
            if box.x0 < 0 or box.y0 < 0 or box.x1 > canvas_box.x1 or box.y1 > canvas_box.y1:
                raise ValueError(f"Text outside image: {label.get_text()}")
    path = ROOT / "TRAMES_ET_TIMINGS.png"
    fig.savefig(path, dpi=160, facecolor="white", metadata={"Title": "Modem300 v1.0.0-reference - Trames et timings", "Description": "Calculated firmware schedule; not an oscilloscope capture."})
    plt.close(fig)
    return path


if __name__ == "__main__":
    verify_reference()
    values = calculate()
    output = render(values)
    (ROOT / "docs/timing_values.json").write_text(json.dumps(values, indent=2) + "\n", encoding="utf-8")
    print(f"Reference hashes and timing assertions verified. Diagram: {output}")
