"""Settings, and where things live on disk.

Everything the web UI can change is persisted here, in one JSON file the
service owns.  Nothing of moOde's is touched.
"""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, asdict, field

DEFAULT_DIR = os.environ.get("AMPD_DIR", "/var/lib/ampd")


@dataclass
class Config:
    # --- link to the Pico
    serial_device: str = "/dev/ttyAMA0"
    serial_baud: int = 921600

    # --- metadata sources
    #: moOde's MPD.  The digital input, and nothing to improve on.
    mpd_host: str = "127.0.0.1"
    mpd_port: int = 6600

    #: A WiiM or other LinkPlay streamer on the analogue input.  Empty
    #: means there is not one, and the fingerprinter takes over.
    linkplay_host: str = ""
    linkplay_poll_s: float = 2.0

    #: "shazam", "acoustid", "audd" or "off".
    fingerprint_provider: str = "shazam"
    acoustid_key: str = ""
    audd_token: str = ""

    #: Never more often than this, whatever the music does.
    fingerprint_min_interval_s: float = 45.0

    # --- panel
    panel_width: int = 1920
    panel_height: int = 480
    #: Now playing on the left, visualiser on the right.
    nowplaying_width: int = 640

    # --- audio stream policy
    #: Seconds of audio a fingerprint capture asks the Pico for.  Timed,
    #: so a crash here cannot leave 52% of the UART running.
    capture_seconds: float = 9.0

    http_port: int = 8080

    dir: str = DEFAULT_DIR

    def path(self, name: str) -> str:
        return os.path.join(self.dir, name)

    @property
    def visualizers_path(self) -> str:
        return self.path("visualizers.json")

    @property
    def match_cache_path(self) -> str:
        return self.path("matches.json")

    @property
    def eq_path(self) -> str:
        return self.path("eq.json")

    # ---------------------------------------------------------- persist

    @classmethod
    def load(cls, path: str | None = None) -> "Config":
        cfg = cls()
        path = path or cfg.path("config.json")
        try:
            with open(path, encoding="utf-8") as fh:
                data = json.load(fh)
        except (OSError, ValueError):
            return cfg
        for k, v in data.items():
            if hasattr(cfg, k):
                setattr(cfg, k, v)
        return cfg

    def save(self, path: str | None = None) -> None:
        path = path or self.path("config.json")
        os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as fh:
            json.dump(asdict(self), fh, indent=2)
        os.replace(tmp, path)

    def public(self) -> dict:
        """What the web UI may see.  Keys are not secrets to the owner of
        the box, but there is no reason to put them on a kiosk screen."""
        d = asdict(self)
        for secret in ("acoustid_key", "audd_token"):
            d[secret] = bool(d[secret])
        return d
