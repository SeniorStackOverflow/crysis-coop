from .events import Events


class Context:
    def __init__(self, host, companion=None, window=None, map_path=None):
        self.host = host
        self.companion = companion
        self.ev = Events(host)
        self.window = window      # {log path: (first, last)} or None for whole logs
        self.map_path = map_path

    def lines(self, log):
        if log is None:
            return []
        if self.window and log.path in self.window:
            a, b = self.window[log.path]
            return log.slice(a, b)
        return log.lines

    def in_window(self, line):
        if not self.window or line.src.path not in self.window:
            return True
        a, b = self.window[line.src.path]
        return a <= line.no <= (b if b is not None else 1 << 30)
