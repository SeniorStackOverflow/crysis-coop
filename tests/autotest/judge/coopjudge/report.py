"""The Russian summary for the user and results.json for Claude."""

import json
import re

# "[Coop] HIT Bridge_5 by Companion weapon=VehicleShiTenV2 type=bullet ... hp 52->-26 KILLED"
# (a hit on a soldier; "HIT player ..." are hits on players)
RE_COMPANION_HIT = re.compile(r"^\[Coop\] HIT (?!player )(\S+) by Companion weapon=(\S+) .*?( KILLED)?$")

RU_VERDICT = {"CRASH": "ПАДЕНИЕ", "FAIL": "ОШИБКА", "INFRA": "ТЕСТ", "WARN": "заметка"}


def companion_combat(ctx):
    """The AI companion's hits and kills on the host (the user asks: did it kill
    anybody?): {"hits": n, "kills": n, "weapons": {weapon: kills}, "killed": [names]}"""
    hits, kills, weapons, killed = 0, 0, {}, []
    for l in ctx.lines(ctx.host):
        m = RE_COMPANION_HIT.match(l.text)
        if not m:
            continue
        hits += 1
        if m.group(3):
            kills += 1
            weapons[m.group(2)] = weapons.get(m.group(2), 0) + 1
            killed.append(m.group(1))
    return {"hits": hits, "kills": kills, "weapons": weapons, "killed": killed}


def summary_ru(ctx, findings, title=None):
    out = []
    if title:
        out.append(title)
    n = {v: sum(1 for f in findings if f.verdict == v) for v in RU_VERDICT}
    loads = ctx.ev.loads
    out.append("Загрузок чекпойнта: %d (в запущенный уровень: %d), загрузок уровня: %d, компаньон входил в игру: %d раз" % (
        len(loads), sum(1 for l in loads if l.inplace), len(ctx.ev.levels), len(ctx.ev.joins)))
    c = companion_combat(ctx)
    if ctx.ev.joins or c["hits"]:
        out.append("ИИ-компаньон: попаданий по врагам %d, убил %d%s" % (c["hits"], c["kills"],
            " (" + ", ".join("%s: %d" % kv for kv in sorted(c["weapons"].items(), key=lambda kv: -kv[1])) + ")" if c["kills"] else ""))
    if not any(n[v] for v in ("CRASH", "FAIL", "INFRA")):
        out.append("ИТОГ: ошибок не найдено" + (" (заметок: %d)" % n["WARN"] if n["WARN"] else ""))
    else:
        out.append("ИТОГ: падений %d, ошибок %d, сбоев теста %d, заметок %d" % (n["CRASH"], n["FAIL"], n["INFRA"], n["WARN"]))
    for i, f in enumerate(findings, 1):
        times = " (x%d)" % f.count if f.count > 1 else ""
        out.append("%d. [%s] %s%s" % (i, RU_VERDICT[f.verdict], f.ru, times))
        for l in f.evidence[:3]:
            out.append("     %s  %s" % (l.where(), l.text[:160]))
    return "\n".join(out)


def results_json(ctx, findings):
    return json.dumps({
        "host": ctx.host.path,
        "companion": ctx.companion.path if ctx.companion else None,
        "loads": [{"start": l.start.no, "end": l.end.no if l.end else None, "inplace": l.inplace,
                   "took": l.took, "friends_kept": l.friends_kept, "entities_kept": l.entities_kept}
                  for l in ctx.ev.loads],
        "joins": [l.no for l in ctx.ev.joins],
        "companion_combat": companion_combat(ctx),
        "findings": [f.to_json() for f in findings],
    }, ensure_ascii=False, indent=1)
