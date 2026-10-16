"""What a detector reports.

verdict:
  CRASH  a game crashed
  FAIL   a bug the player sees (kicked, debris kills, unarmed soldiers ...)
  WARN   worth a look, not a failure on its own
  INFRA  the test itself went wrong (no ack, wrong DLL ...), not the mod
"""

ORDER = {"CRASH": 0, "FAIL": 1, "INFRA": 2, "WARN": 3}


class Finding:
    __slots__ = ("id", "verdict", "key", "ru", "en", "evidence", "count")

    def __init__(self, id, verdict, ru, en, evidence=(), key=None):
        assert verdict in ORDER, verdict
        self.id = id                  # detector id, e.g. "D03"
        self.verdict = verdict
        self.ru = ru                  # one line for the user, in Russian
        self.en = en                  # the same for Claude, in English
        self.evidence = list(evidence)  # Line records
        self.key = key or ru          # findings with the same id and key are merged
        self.count = 1

    def to_json(self):
        return {
            "id": self.id,
            "verdict": self.verdict,
            "ru": self.ru,
            "en": self.en,
            "count": self.count,
            "evidence": [{"where": l.where(), "t": l.t, "text": l.text[:300]} for l in self.evidence[:8]],
        }

    def __repr__(self):
        return "<%s %s %s x%d>" % (self.id, self.verdict, self.en, self.count)


def merge(findings):
    """Same id and key: one finding with a count and the first few lines of evidence."""
    out = {}
    for f in findings:
        k = (f.id, f.key)
        if k in out:
            g = out[k]
            g.count += f.count
            if len(g.evidence) < 8:
                g.evidence.extend(f.evidence[: 8 - len(g.evidence)])
        else:
            out[k] = f
    return sorted(out.values(), key=lambda f: (ORDER[f.verdict], f.id, f.evidence[0].no if f.evidence else 0))
